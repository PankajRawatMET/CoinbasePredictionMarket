// The whole stack, end to end: both simulators, the real daemon over real FIX
// sessions, and the GUI driven the way a browser drives it - HTTP requests and
// an SSE stream, no ControlApi call anywhere in this file.
//
// Everything above this line in the tree has been tested against fakes. This
// is the one place where a quote reaching the order entry simulator is caused
// by a POST.
#include "cpm/gui/server.hpp"

#include "cpm/gui/json.hpp"
#include "cpm/md/sim/simulator.hpp"
#include "cpm/oms/sim/simulator.hpp"
#include "cpm/strategy/crypto_mm.hpp"
#include "cpm/trader/trader.hpp"
#include "support.hpp"

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <atomic>
#include <optional>
#include <chrono>
#include <sstream>
#include <thread>

namespace {

using namespace std::chrono_literals;
using cpm::gui::Server;
using cpm::gui::test::Client;
using cpm::gui::test::body_of;
using cpm::gui::test::status_of;
using cpm::trader::Trader;
using cpm::trader::TraderOptions;

const std::string kSym = "SIMGAME-26NOV01HOMAWY-HOM";

std::uint16_t free_port() {
    const int socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    ::bind(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    socklen_t length = sizeof(address);
    ::getsockname(socket_fd, reinterpret_cast<sockaddr*>(&address), &length);
    const auto port = ntohs(address.sin_port);
    ::close(socket_fd);
    return port;
}

// Both simulators, the daemon, the GUI, and the tick loop the daemon needs -
// which in cpm_trader is main()'s loop and here is a thread, because the
// browser's requests arrive on the server's thread either way.
struct Live {
    // `sim_goes_quiet_after` is the simulator's fault flag: it stops sending
    // after that many seconds, which is what makes the daemon call the feed
    // stale. It has to be known at construction, because the simulator is
    // started here.
    explicit Live(bool allow_orders = true, std::chrono::seconds stale = 2s,
                  std::optional<int> sim_goes_quiet_after = std::nullopt) {
        cpm::strategy::register_crypto_mm(registry);

        const cpm::Credentials md_creds{"md-key", "md-pass", "bWQtc2ltLXNlY3JldA=="};
        const cpm::Credentials oe_creds{"oe-key", "oe-pass", "b2Utc2ltLXNlY3JldA=="};
        const auto md_port = free_port();
        const auto oe_port = free_port();

        cpm::md::sim::SimOptions md_options;
        md_options.credentials = md_creds;
        md_options.port = md_port;
        md_options.dict_dir = CPM_DICT_DIR;
        md_options.stale_after_secs = sim_goes_quiet_after;
        md_sim = std::make_unique<cpm::md::sim::Simulator>(std::move(md_options), sim_logger);
        md_sim->start();

        cpm::oms::sim::SimOptions oe_options;
        oe_options.credentials = oe_creds;
        oe_options.port = oe_port;
        oe_options.dict_dir = CPM_DICT_DIR;
        oe_options.instruments = {{kSym, true, std::chrono::milliseconds{0}}};
        oe_sim = std::make_unique<cpm::oms::sim::Simulator>(std::move(oe_options), sim_logger);
        oe_sim->start();

        TraderOptions options;
        options.md.credentials = md_creds;
        options.md.endpoint = {"127.0.0.1", md_port, false, ""};
        options.oe.credentials = oe_creds;
        options.oe.endpoint = {"127.0.0.1", oe_port, false, ""};
        options.oe.subaccount = "dev-1";
        options.dict_dir = CPM_DICT_DIR;
        options.bookmark = bookmark_path();
        options.allow_orders = allow_orders;
        options.limits.max_quantity = 1000;
        options.limits.max_notional = 10'000'000;
        options.limits.max_orders_per_sec = 20;
        options.stale_after = stale;
        trader = std::make_unique<Trader>(std::move(options), registry, logger);
    }

    std::string start() {
        const auto error = trader->start();
        if (!error.empty()) return error;

        Server::Options gui;
        gui.port = 0;
        server = std::make_unique<Server>(trader->api(), std::move(gui), logger);
        if (const auto gui_error = server->start(); !gui_error.empty()) return gui_error;

        ticking = true;
        ticker = std::thread([this] {
            while (ticking) {
                trader->tick(cpm::oms::Clock::now());
                std::this_thread::sleep_for(25ms);
            }
        });
        return {};
    }

    ~Live() {
        ticking = false;
        if (ticker.joinable()) ticker.join();
        if (server) server->stop();
        if (trader) trader->stop();
        if (oe_sim) oe_sim->stop();
        if (md_sim) md_sim->stop();
        ::unlink(bookmark_path().c_str());
    }

    static std::string bookmark_path() {
        return "cpm-gui-live-bookmark-" + std::to_string(::getpid()) + ".txt";
    }

    std::string logs() const { return log.str() + sim_log.str(); }

    // One request, on its own connection, exactly as the page would send it.
    std::string request(const std::string& method, const std::string& path,
                        const std::string& body = {}) {
        Client client;
        EXPECT_TRUE(client.connect(server->port()));
        std::string text = method + " " + path + " HTTP/1.1\r\nHost: localhost\r\n" +
                           "Authorization: Bearer " + server->token() + "\r\n";
        if (!body.empty()) text += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        text += "Connection: close\r\n\r\n" + body;
        EXPECT_TRUE(client.write(text));
        return client.response();
    }

    cpm::gui::JsonValue snapshot() {
        const auto parsed = cpm::gui::json_parse(body_of(request("GET", "/api/snapshot")));
        EXPECT_TRUE(parsed.ok()) << parsed.error;
        return parsed.value;
    }

    // Polls the page's own view of the world, not the daemon's internals: if
    // a condition cannot be seen over HTTP, the operator cannot see it either.
    bool eventually(const std::function<bool(const cpm::gui::JsonValue&)>& ready,
                    std::chrono::milliseconds timeout = 15s) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (ready(snapshot())) return true;
            std::this_thread::sleep_for(100ms);
        }
        return ready(snapshot());
    }

    cpm::strategy::StrategyRegistry registry;
    std::ostringstream log, sim_log;
    cpm::Logger logger{log, cpm::LogLevel::Info};
    cpm::Logger sim_logger{sim_log};
    std::unique_ptr<cpm::md::sim::Simulator> md_sim;
    std::unique_ptr<cpm::oms::sim::Simulator> oe_sim;
    std::unique_ptr<Trader> trader;
    std::unique_ptr<Server> server;
    std::atomic<bool> ticking{false};
    std::thread ticker;
};

const std::string kCreate =
    R"({"kind":"CryptoMM","params":{"symbol":")" + kSym + R"(","qty":"1","width-ticks":"2"}})";

std::string id_of(const std::string& response) {
    const auto parsed = cpm::gui::json_parse(body_of(response));
    const auto* id = parsed.value.find("id");
    return id == nullptr ? std::string() : id->text;
}

const cpm::gui::JsonValue* first_strategy(const cpm::gui::JsonValue& snapshot) {
    const auto* strategies = snapshot.find("strategies");
    if (strategies == nullptr || strategies->items.empty()) return nullptr;
    return &strategies->items[0];
}

TEST(Live, APostPutsQuotesInTheMarket) {
    Live live;
    ASSERT_EQ(live.start(), "") << live.logs();

    const auto created = live.request("POST", "/api/strategies", kCreate);
    ASSERT_EQ(status_of(created), 200) << body_of(created);
    const auto id = id_of(created);
    ASSERT_FALSE(id.empty());

    // The book has to arrive before anything can be quoted around it, and it
    // arrives because create() opened the subscription.
    ASSERT_TRUE(live.eventually([](const cpm::gui::JsonValue& snapshot) {
        const auto* books = snapshot.find("books");
        return books != nullptr && !books->items.empty() &&
               books->items[0].find("mid")->type != cpm::gui::JsonValue::Type::Null;
    })) << live.logs();

    ASSERT_EQ(status_of(live.request("POST", "/api/strategies/" + id + "/start")), 200);
    ASSERT_EQ(status_of(live.request("POST", "/api/strategies/" + id + "/arm")), 200);

    // Orders at the venue, caused by an HTTP request and visible in the page's
    // own snapshot.
    ASSERT_TRUE(live.eventually([](const cpm::gui::JsonValue& snapshot) {
        const auto* strategy = first_strategy(snapshot);
        return strategy != nullptr && !strategy->find("legs")->items.empty();
    })) << live.logs();

    const auto snapshot = live.snapshot();
    const auto* strategy = first_strategy(snapshot);
    ASSERT_NE(strategy, nullptr);
    EXPECT_EQ(strategy->find("state")->text, "running");
    EXPECT_TRUE(strategy->find("armed")->boolean);
    const auto& legs = strategy->find("legs")->items;
    EXPECT_EQ(legs.size(), 2u) << "a maker quotes both sides";
    for (const auto& leg : legs) {
        // Prices arrive as text from scaled integers, all the way from the
        // FIX session to the browser (design §8).
        EXPECT_FALSE(leg.find("price")->text.empty());
        EXPECT_EQ(leg.find("price")->type, cpm::gui::JsonValue::Type::String);
    }
    EXPECT_GT(std::stoi(strategy->find("counts")->find("placed")->text), 0);
}

TEST(Live, TheLadderShowsTheRealBookWithOurOrdersOnIt) {
    Live live;
    ASSERT_EQ(live.start(), "") << live.logs();
    const auto id = id_of(live.request("POST", "/api/strategies", kCreate));
    ASSERT_FALSE(id.empty());
    ASSERT_EQ(status_of(live.request("POST", "/api/strategies/" + id + "/start")), 200);
    ASSERT_EQ(status_of(live.request("POST", "/api/strategies/" + id + "/arm")), 200);

    ASSERT_TRUE(live.eventually([](const cpm::gui::JsonValue& snapshot) {
        const auto* strategy = first_strategy(snapshot);
        return strategy != nullptr && !strategy->find("legs")->items.empty();
    })) << live.logs();

    const auto snapshot = live.snapshot();
    const auto& book = snapshot.find("books")->items[0];
    const auto& bids = book.find("bids")->items;
    const auto& asks = book.find("asks")->items;

    // Depth, not just the top: the subscription asks for ten rungs and the
    // simulator's book is deeper than one.
    EXPECT_GT(bids.size(), 1u) << "the ladder has no depth to show";
    EXPECT_GT(asks.size(), 1u);
    EXPECT_EQ(bids[0].find("price")->text, book.find("best_yes_bid")->text);
    EXPECT_EQ(asks[0].find("price")->text, book.find("implied_yes_ask")->text);

    // Every price our own orders rest at can be found on the ladder's axis by
    // its tick count - which is what the page highlights on.
    const auto& legs = first_strategy(snapshot)->find("legs")->items;
    ASSERT_FALSE(legs.empty());
    for (const auto& leg : legs) {
        EXPECT_FALSE(leg.find("price_ticks")->text.empty());
        EXPECT_EQ(leg.find("price_ticks")->type, cpm::gui::JsonValue::Type::Number);
    }

    // And the ask rungs are the implied YES chances: each one plus the NO bid
    // the venue is really holding comes to exactly 100% (Q9). Both are read
    // back as percentages, which is the unit the GUI speaks in.
    for (const auto& ask : asks) {
        const auto yes = std::stoll(ask.find("price_ticks")->text);
        const auto venue = cpm::parse_percent(ask.find("venue_price")->text);
        ASSERT_TRUE(venue.has_value()) << ask.find("venue_price")->text;
        EXPECT_EQ(yes + *venue, cpm::kPriceOne);
    }
    EXPECT_EQ(snapshot.find("price_unit")->text, "percent");
}

TEST(Live, TheKillSwitchLeavesNothingResting) {
    Live live;
    ASSERT_EQ(live.start(), "") << live.logs();
    const auto id = id_of(live.request("POST", "/api/strategies", kCreate));
    ASSERT_FALSE(id.empty());
    ASSERT_EQ(status_of(live.request("POST", "/api/strategies/" + id + "/start")), 200);
    ASSERT_EQ(status_of(live.request("POST", "/api/strategies/" + id + "/arm")), 200);
    ASSERT_TRUE(live.eventually([](const cpm::gui::JsonValue& snapshot) {
        const auto* strategy = first_strategy(snapshot);
        return strategy != nullptr && !strategy->find("legs")->items.empty();
    })) << live.logs();

    ASSERT_EQ(status_of(live.request("POST", "/api/stop-all")), 200);

    // Stopped means nothing of its own is left at the venue, not merely that
    // it was told to stop.
    ASSERT_TRUE(live.eventually([](const cpm::gui::JsonValue& snapshot) {
        const auto* strategy = first_strategy(snapshot);
        return strategy != nullptr && strategy->find("state")->text == "stopped" &&
               strategy->find("legs")->items.empty();
    })) << live.logs();
    EXPECT_FALSE(first_strategy(live.snapshot())->find("armed")->boolean);
}

TEST(Live, TheStreamCarriesWhatTheDaemonDoesWhileNobodyAsks) {
    Live live;
    ASSERT_EQ(live.start(), "") << live.logs();

    Client stream;
    ASSERT_TRUE(stream.connect(live.server->port()));
    ASSERT_TRUE(stream.write("GET /api/events?t=" + live.server->token() +
                             " HTTP/1.1\r\nHost: localhost\r\n\r\n"));
    ASSERT_FALSE(stream.drain_headers().empty());

    const auto first = stream.event();
    ASSERT_NE(first.find("event: snapshot"), std::string::npos) << first;

    const auto id = id_of(live.request("POST", "/api/strategies", kCreate));
    ASSERT_FALSE(id.empty());
    ASSERT_EQ(status_of(live.request("POST", "/api/strategies/" + id + "/start")), 200);

    // Each of those crossed the stream without the page asking for anything.
    bool saw_running = false;
    for (int i = 0; i < 6 && !saw_running; ++i) {
        const auto event = stream.event();
        if (event.find("event: snapshot") == std::string::npos) continue;
        const auto data = event.substr(event.find("data: ") + 6);
        const auto parsed = cpm::gui::json_parse(data);
        ASSERT_TRUE(parsed.ok()) << parsed.error;
        const auto* strategy = first_strategy(parsed.value);
        saw_running = strategy != nullptr && strategy->find("state")->text == "running";
    }
    EXPECT_TRUE(saw_running) << "the stream never carried the start";
}

TEST(Live, TheStreamFollowsTheMarketAndNotJustTheOperator) {
    // The bug this test exists for: the stream used to wake on the control
    // API's revision, which counts button presses. A book that moves, a quote
    // that is re-placed and a fill that arrives all reach the daemon without
    // touching ControlApi, so a page watching a running strategy froze on the
    // last thing its operator did - showing stale quotes on a live ladder.
    Live live;
    ASSERT_EQ(live.start(), "") << live.logs();
    const auto id = id_of(live.request("POST", "/api/strategies", kCreate));
    ASSERT_FALSE(id.empty());
    ASSERT_EQ(status_of(live.request("POST", "/api/strategies/" + id + "/start")), 200);
    ASSERT_EQ(status_of(live.request("POST", "/api/strategies/" + id + "/arm")), 200);

    Client stream;
    ASSERT_TRUE(stream.connect(live.server->port()));
    ASSERT_TRUE(stream.write("GET /api/events?t=" + live.server->token() +
                             " HTTP/1.1\r\nHost: localhost\r\n\r\n"));
    ASSERT_FALSE(stream.drain_headers().empty());

    // From here on nothing is asked of the daemon: every event that arrives
    // is one it decided to send.
    std::string first_legs, first_book;
    std::uint64_t revision = 0;
    bool legs_changed = false, book_changed = false;
    for (int i = 0; i < 40 && !(legs_changed && book_changed); ++i) {
        const auto event = stream.event();
        if (event.find("event: snapshot") == std::string::npos) continue;
        const auto parsed = cpm::gui::json_parse(event.substr(event.find("data: ") + 6));
        ASSERT_TRUE(parsed.ok()) << parsed.error;

        const auto* strategy = first_strategy(parsed.value);
        ASSERT_NE(strategy, nullptr);
        std::string legs;
        for (const auto& leg : strategy->find("legs")->items) {
            legs += leg.find("side")->text + leg.find("price")->text + "/";
        }
        const auto& book = parsed.value.find("books")->items;
        const std::string top = book.empty() ? "" : book[0].find("best_yes_bid")->text + ":" +
                                                        std::to_string(book[0].find("bids")->items.size());

        if (first_legs.empty() && !legs.empty()) {
            first_legs = legs;
            first_book = top;
            revision = static_cast<std::uint64_t>(
                std::stoull(parsed.value.find("revision")->text));
            continue;
        }
        if (first_legs.empty()) continue;  // nothing to compare against yet
        if (legs != first_legs) legs_changed = true;
        if (!first_book.empty() && top != first_book) book_changed = true;
        // And none of it was an operator action: the revision never moved.
        EXPECT_EQ(std::stoull(parsed.value.find("revision")->text), revision);
    }

    EXPECT_TRUE(book_changed) << "the stream never carried a book that moved";
    EXPECT_TRUE(legs_changed) << "the stream never carried a re-quote";
}

TEST(Live, AStaleFeedHaltsAndThePageStaysUpToSayWhy) {
    Live live(true, 2s, 4);  // the simulator goes quiet after 4 s
    ASSERT_EQ(live.start(), "") << live.logs();
    const auto id = id_of(live.request("POST", "/api/strategies", kCreate));
    ASSERT_FALSE(id.empty());
    ASSERT_EQ(status_of(live.request("POST", "/api/strategies/" + id + "/start")), 200);
    ASSERT_EQ(status_of(live.request("POST", "/api/strategies/" + id + "/arm")), 200);

    ASSERT_TRUE(live.eventually(
        [](const cpm::gui::JsonValue& snapshot) {
            return snapshot.find("state")->text == "halted";
        },
        25s)) << live.logs();

    // The whole point of GUI-Q4: the process is still here, and it can still
    // be asked what happened.
    const auto snapshot = live.snapshot();
    EXPECT_FALSE(snapshot.find("halt_reason")->text.empty());
    EXPECT_NE(snapshot.find("halt_reason")->text.find("stale"), std::string::npos)
        << snapshot.find("halt_reason")->text;

    // A halt is two phases, and the page sees both: every strategy withdraws
    // first, and the sessions go down only once those cancels are accounted
    // for. The reason is readable throughout, which is the point.
    EXPECT_TRUE(live.eventually(
        [](const cpm::gui::JsonValue& state) {
            return !state.find("market_data")->find("logged_on")->boolean &&
                   !state.find("order_entry")->find("logged_on")->boolean;
        },
        15s)) << live.logs();

    const auto* strategy = first_strategy(snapshot);
    ASSERT_NE(strategy, nullptr);
    EXPECT_EQ(strategy->find("state")->text, "halted");
    EXPECT_FALSE(strategy->find("armed")->boolean);

    // And nothing can be started again, in this process, ever.
    const auto again = live.request("POST", "/api/strategies/" + id + "/arm");
    EXPECT_EQ(status_of(again), 409);
    EXPECT_NE(body_of(again).find("halted"), std::string::npos) << body_of(again);
    EXPECT_EQ(status_of(live.request("POST", "/api/strategies", kCreate)), 409);

    // The page itself is still served: a halt at 02:00 must leave something to
    // read, not an exit code on a closed window.
    EXPECT_EQ(status_of(live.request("GET", "/")), 200);
}

TEST(Live, AnObserverRunRefusesToArmOverHttpToo) {
    Live live(false);
    ASSERT_EQ(live.start(), "") << live.logs();
    const auto id = id_of(live.request("POST", "/api/strategies", kCreate));
    ASSERT_FALSE(id.empty());
    ASSERT_EQ(status_of(live.request("POST", "/api/strategies/" + id + "/start")), 200);

    const auto armed = live.request("POST", "/api/strategies/" + id + "/arm");
    EXPECT_EQ(status_of(armed), 409);
    EXPECT_NE(body_of(armed).find("--allow-orders"), std::string::npos) << body_of(armed);

    const auto snapshot = live.snapshot();
    EXPECT_FALSE(snapshot.find("allow_orders")->boolean);
    // It still runs and still computes its quotes; it simply sends nothing.
    ASSERT_TRUE(live.eventually([](const cpm::gui::JsonValue& state) {
        const auto* strategy = first_strategy(state);
        return strategy != nullptr && strategy->find("counts")->find("passes")->text != "0";
    })) << live.logs();
    EXPECT_TRUE(first_strategy(live.snapshot())->find("legs")->items.empty());
}

}  // namespace
