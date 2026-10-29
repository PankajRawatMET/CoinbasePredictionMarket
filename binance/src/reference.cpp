#include "cpm/binance/reference.hpp"

#include "cpm/common/fixed.hpp"
#include "cpm/common/json.hpp"
#include "cpm/common/websocket.hpp"

#include <algorithm>

namespace cpm::binance {
namespace {

std::string lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

std::string upper(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return out;
}

}  // namespace

std::optional<BookTicker> parse_book_ticker(std::string_view json) {
    const auto parsed = cpm::json_parse(json);
    if (!parsed.ok() || !parsed.value.is_object()) return std::nullopt;

    // A combined stream wraps the payload; a single stream does not. Both
    // shapes are accepted so the path can change without this changing.
    const JsonValue* payload = &parsed.value;
    if (const auto* data = parsed.value.find("data"); data != nullptr && data->is_object()) {
        payload = data;
    }

    const auto* symbol = payload->find("s");
    const auto* bid = payload->find("b");
    const auto* ask = payload->find("a");
    if (symbol == nullptr || bid == nullptr || ask == nullptr) return std::nullopt;

    // Prices arrive as strings, which is what keeps them out of a double.
    const auto bid_value = cpm::parse_scaled(bid->text, kScale);
    const auto ask_value = cpm::parse_scaled(ask->text, kScale);
    if (!bid_value || !ask_value) return std::nullopt;
    // A crossed or empty touch has no mid worth acting on.
    if (*bid_value <= 0 || *ask_value <= 0 || *ask_value < *bid_value) return std::nullopt;

    BookTicker out;
    out.symbol = upper(symbol->text);
    out.bid = *bid_value;
    out.ask = *ask_value;
    return out;
}

ReferenceFeed::ReferenceFeed(Options options, Logger& logger)
    : options_(std::move(options)), logger_(logger) {
    for (auto& symbol : options_.symbols) symbol = upper(symbol);
}

ReferenceFeed::~ReferenceFeed() { stop(); }

void ReferenceFeed::on_update(std::function<void()> waker) {
    std::lock_guard lock(mutex_);
    waker_ = std::move(waker);
}

void ReferenceFeed::want(const std::vector<std::string>& symbols) {
    std::set<std::string> next;
    for (const auto& symbol : symbols) next.insert(upper(symbol));
    // Whatever was asked for at startup stays up whatever the strategies
    // do: it is the operator saying "keep this warm".
    for (const auto& symbol : options_.symbols) next.insert(symbol);

    std::lock_guard lock(mutex_);
    wanted_ = std::move(next);
}

std::vector<std::string> ReferenceFeed::subscribed() const {
    std::lock_guard lock(mutex_);
    return {live_.begin(), live_.end()};
}

void ReferenceFeed::handle_control(std::string_view text) {
    // {"result":null,"id":1} is success for a subscribe or an unsubscribe,
    // and {"code":N,"msg":"..."} is the venue refusing one. Neither is a
    // price, and a feed that read them as one would be inventing numbers.
    const auto parsed = cpm::json_parse(text);
    if (!parsed.ok() || !parsed.value.is_object()) return;
    if (const auto* code = parsed.value.find("code"); code != nullptr) {
        const auto* message = parsed.value.find("msg");
        logger_.warn("binance refused a subscription change: " + code->text + " " +
                     (message != nullptr ? message->text : std::string()));
    }
}

bool ReferenceFeed::reconcile_streams(cpm::ws::Client& client, Clock::time_point now) {
    std::vector<std::string> add;
    std::vector<std::string> drop;
    std::uint64_t id = 0;
    {
        std::lock_guard lock(mutex_);
        if (options_.control_per_sec > 0) {
            const auto gap = std::chrono::milliseconds(1000 / options_.control_per_sec);
            if (now - last_control_ < gap) return true;
        }
        for (const auto& symbol : wanted_) {
            if (live_.count(symbol) == 0) add.push_back(symbol);
        }
        for (const auto& symbol : live_) {
            if (wanted_.count(symbol) == 0) drop.push_back(symbol);
        }
        if (add.empty() && drop.empty()) return true;
        last_control_ = now;
        id = ++control_id_;
    }

    // One message each way however many symbols moved: `params` is an
    // array, and the venue counts messages, not streams.
    const auto send = [&](const char* method, const std::vector<std::string>& symbols,
                          std::uint64_t request_id) {
        std::string params;
        for (const auto& symbol : symbols) {
            if (!params.empty()) params += ",";
            params += "\"" + lower(symbol) + "@bookTicker\"";
        }
        const std::string message = std::string(R"({"method":")") + method + R"(","params":[)" +
                                    params + R"(],"id":)" + std::to_string(request_id) + "}";
        return client.send_text(message);
    };

    if (!add.empty()) {
        if (const auto failed = send("SUBSCRIBE", add, id); !failed.empty()) {
            logger_.warn("binance: could not subscribe: " + failed);
            return false;
        }
        std::lock_guard lock(mutex_);
        for (const auto& symbol : add) live_.insert(symbol);
        logger_.info("binance: subscribed " + std::to_string(add.size()) + " stream(s): " +
                     [&add] {
                         std::string names;
                         for (const auto& s : add) names += (names.empty() ? "" : ", ") + s;
                         return names;
                     }());
    }
    if (!drop.empty()) {
        if (const auto failed = send("UNSUBSCRIBE", drop, id + 1); !failed.empty()) {
            logger_.warn("binance: could not unsubscribe: " + failed);
            return false;
        }
        std::lock_guard lock(mutex_);
        for (const auto& symbol : drop) {
            live_.erase(symbol);
            // And forget its price: a mid from a stream nobody is listening
            // to is not stale, it is gone, and must not be served as either.
            latest_.erase(symbol);
        }
        logger_.info("binance: unsubscribed " + std::to_string(drop.size()) + " stream(s): " +
                     [&drop] {
                         std::string names;
                         for (const auto& s : drop) names += (names.empty() ? "" : ", ") + s;
                         return names;
                     }());
    }
    return true;
}

std::string ReferenceFeed::start() {
    if (running_.load()) return {};
    // No symbols is the normal state, not an error: the connection comes
    // up carrying nothing and streams are subscribed as strategies start
    // (B-Q14).

    // The tick size is fetched where the subscription is, not here: a
    // symbol can be wanted at any time now that streams follow the
    // strategies (B-Q14), and a lookup that only ran at startup over the
    // pinned list left every later symbol without one - which means
    // usable() is false and the guard refuses to quote.
    //
    // It also keeps the daemon's startup off a third party's REST
    // endpoint: nothing waits on Binance before the FIX sessions log on.
    want({});

    running_.store(true);
    thread_ = std::thread([this] { run(); });
    return {};
}

void ReferenceFeed::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    connected_.store(false);
}

void ReferenceFeed::apply(const BookTicker& tick) {
    std::function<void()> waker;
    {
        std::lock_guard lock(mutex_);
        // An UNSUBSCRIBE and a message already in flight cross; the message
        // is not a reason to resurrect a stream nobody is listening to.
        if (live_.count(tick.symbol) == 0) return;
        auto& entry = latest_[tick.symbol];
        entry.mid = tick.mid();
        entry.at = Clock::now();
        entry.present = true;
        waker = waker_;
    }
    // Outside the lock: whoever is woken must never be able to block the
    // reader, and must never need this lock to do its work.
    if (waker) waker();
}

ReferenceView ReferenceFeed::latest(const std::string& symbol, Clock::time_point now) const {
    ReferenceView view;
    std::lock_guard lock(mutex_);
    if (const auto tick = options_.tick_size.find(symbol); tick != options_.tick_size.end()) {
        view.tick_size = tick->second;
    }
    const auto found = latest_.find(symbol);
    if (found == latest_.end() || !found->second.present) return view;

    view.present = true;
    view.mid = found->second.mid;
    view.age = std::chrono::duration_cast<std::chrono::milliseconds>(now - found->second.at);
    view.stale = view.age > options_.stale_after;
    return view;
}

void ReferenceFeed::run() {
    // One connection for the life of the daemon, with no streams in the
    // URL: everything is subscribed and unsubscribed on it as strategies
    // come and go (B-Q14). The venue allows 1024 streams on one connection,
    // which is three orders of magnitude more than this will ever want.
    const std::string path = options_.path;

    auto backoff = options_.reconnect_after;
    while (running_.load()) {
        cpm::ws::Client client;
        cpm::ws::Client::Options options;
        options.host = options_.host;
        options.port = options_.port;
        options.path = path;
        options.tls = options_.tls;
        options.ca_file = options_.ca_file;

        if (const auto failed = client.connect(options); !failed.empty()) {
            // Not an error worth stopping for: the feed is simply stale
            // until it is back, and every strategy that needs it withdraws
            // on that alone (B-Q4, B-Q13).
            logger_.warn("binance: " + failed + "; retrying in " +
                         std::to_string(backoff.count()) + "ms");
            const auto until = Clock::now() + backoff;
            while (running_.load() && Clock::now() < until) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            backoff = std::min(backoff * 2, options_.max_reconnect_after);
            continue;
        }

        logger_.info("binance: connected to " + options_.host + path);
        connected_.store(true);
        backoff = options_.reconnect_after;
        {
            // A new connection carries nothing, so everything wanted is
            // wanted again. want() is declarative for exactly this reason.
            std::lock_guard lock(mutex_);
            live_.clear();
            last_control_ = Clock::time_point{};
        }

        while (running_.load()) {
            if (!reconcile_streams(client, Clock::now())) break;
            const auto pumped = client.pump(std::chrono::milliseconds(200),
                                            [this](std::string_view text) {
                                                if (const auto tick = parse_book_ticker(text)) {
                                                    apply(*tick);
                                                    return;
                                                }
                                                handle_control(text);
                                            });
            if (pumped == cpm::ws::Client::Pump::Closed) {
                logger_.warn("binance: " + client.error() + "; reconnecting");
                break;
            }
        }
        connected_.store(false);
        client.close();
    }
}

}  // namespace cpm::binance
