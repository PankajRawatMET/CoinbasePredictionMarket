// md_client: log on to the CPM FIX market data gateway, keep the session alive
// with heartbeats, and log out cleanly on Ctrl+C / SIGTERM / --duration.
// --list-instruments instead prints the gateway's instruments and logs out.
// --capture SYMBOL (repeatable) subscribes to each instrument's book, trades and
// lifecycle, with --depth 1, 10 or both (two book subscriptions);
// --all-lifecycle / --all-trades subscribe for all instruments (146=0);
// A gap, a stale book, any reject or a disconnect stops the run: the feed is
// no longer trustworthy and nothing is resubscribed (exit code 4).
// --probe-unknown sends one book request for a symbol that cannot exist.
// Every decoded event is printed as one "ev " line beside the raw message log;
// --no-events prints only the raw log. Decoded events also build per-instrument
// state (book, last trade, volume, open interest, lifecycle), which is printed
// as a summary on exit; anomalies in the feed are logged as warnings.
//
// Credentials and endpoint come from the environment (see docs/design.md §3):
//   source creds.env && ./md_client [--duration SECS] [--heartbeat SECS] ...
#include "cpm/common/config.hpp"
#include "cpm/common/logging.hpp"
#include "cpm/md/feed_monitor.hpp"
#include "cpm/md/market_state.hpp"
#include "cpm/md/md_client.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Args {
    int duration_secs = 0;  // 0 = until signal
    int logon_timeout_secs = 20;
    int heartbeat_secs = 30;
    std::string dict_dir = CPM_DEFAULT_DICT_DIR;
    bool verbose = false;
    bool list_instruments = false;
    std::vector<std::string> capture_symbols;
    std::vector<int> depths{10};
    bool all_lifecycle = false;
    bool all_trades = false;
    bool probe_unknown = false;
    bool print_events = true;
    int stale_secs = 30;  // Q25

    bool subscribes() const { return !capture_symbols.empty() || all_lifecycle || all_trades || probe_unknown; }
};

// Deliberately unlisted symbol, used to record the server's real reject (281=0).
constexpr const char* kProbeSymbol = "CPM-PROBE-UNKNOWN-SYMBOL-0";

// Discovery stops once snapshots have been quiet this long, or at the timeout.
constexpr std::chrono::seconds kListQuiet{5};
constexpr std::chrono::seconds kListTimeout{60};

void usage() {
    std::cerr << "usage: md_client [--duration SECS] [--logon-timeout SECS] [--heartbeat SECS]\n"
                 "                 [--dict-dir DIR] [--verbose] [--list-instruments]\n"
                 "                 [--capture SYMBOL]... [--depth 1|10|both] [--all-lifecycle]\n"
                 "                 [--all-trades] [--probe-unknown] [--no-events]\n"
                 "                 [--stale-secs SECS]\n"
                 "env: CPM_API_KEY CPM_PASSPHRASE CPM_SECRET CPM_FIX_HOST CPM_FIX_PORT\n"
                 "     [CPM_FIX_TLS=1] [CPM_FIX_CA_FILE]\n";
}

int to_int(std::string_view flag, const char* value) {
    char* end = nullptr;
    const long v = std::strtol(value, &end, 10);
    if (*value == '\0' || *end != '\0' || v < 0 || v > 86400) {
        throw std::invalid_argument(std::string(flag) + " needs an integer in 0..86400");
    }
    return static_cast<int>(v);
}

Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string_view flag = argv[i];
        auto value = [&]() -> const char* {
            if (i + 1 >= argc) throw std::invalid_argument(std::string(flag) + " needs a value");
            return argv[++i];
        };
        if (flag == "--duration") a.duration_secs = to_int(flag, value());
        else if (flag == "--logon-timeout") a.logon_timeout_secs = to_int(flag, value());
        else if (flag == "--heartbeat") a.heartbeat_secs = to_int(flag, value());
        else if (flag == "--dict-dir") a.dict_dir = value();
        else if (flag == "--verbose") a.verbose = true;
        else if (flag == "--list-instruments") a.list_instruments = true;
        else if (flag == "--capture") a.capture_symbols.emplace_back(value());
        else if (flag == "--depth") {
            const std::string_view v = value();
            if (v == "1") a.depths = {1};
            else if (v == "10") a.depths = {10};
            else if (v == "both") a.depths = {10, 1};
            else throw std::invalid_argument("--depth must be 1, 10 or both");
        }
        else if (flag == "--all-lifecycle") a.all_lifecycle = true;
        else if (flag == "--all-trades") a.all_trades = true;
        else if (flag == "--probe-unknown") a.probe_unknown = true;
        else if (flag == "--no-events") a.print_events = false;
        else if (flag == "--stale-secs") a.stale_secs = to_int(flag, value());
        else if (flag == "--help" || flag == "-h") { usage(); std::exit(0); }
        else throw std::invalid_argument("unknown option " + std::string(flag));
    }
    if (a.heartbeat_secs < 1) throw std::invalid_argument("--heartbeat must be >= 1");
    if (a.list_instruments && a.subscribes()) {
        throw std::invalid_argument("--list-instruments cannot be combined with subscriptions");
    }
    return a;
}


// One line per decoded event, written through the Logger so it interleaves
// safely with the raw message log (QuickFIX calls these on its own threads).
cpm::md::MdCallbacks event_printer(cpm::Logger& logger) {
    using namespace cpm::md;

    const auto instrument = [](const std::string& symbol, const std::string& security_id) {
        return symbol + (security_id.empty() ? "" : "/" + security_id);
    };
    const auto levels = [](const std::vector<BookLevel>& side) {
        std::string out;
        for (const auto& level : side) {
            out += (out.empty() ? "" : " ") + cpm::format_price(level.price) + "x" + cpm::format_qty(level.qty);
        }
        return "[" + out + "]";
    };

    MdCallbacks callbacks;
    callbacks.book_snapshot = [&logger, instrument, levels](const BookSnapshot& e) {
        logger.info("ev snapshot  " + e.md_req_id + " " + instrument(e.symbol, e.security_id) + " yes" +
                    levels(e.yes) + " no" + levels(e.no) + " @" + to_utc_string(e.transact_time));
    };
    callbacks.book_update = [&logger, instrument](const BookUpdate& e) {
        std::string changes;
        for (const auto& change : e.changes) {
            changes += " " + std::string(to_string(change.action)) + " " + to_string(change.side) + " " +
                       cpm::format_price(change.price) + "x" + cpm::format_qty(change.qty) +
                       (change.level > 0 ? " L" + std::to_string(change.level) : "");
        }
        logger.info("ev book      " + e.md_req_id + " " + instrument(e.symbol, e.security_id) + changes + " @" +
                    to_utc_string(e.transact_time));
    };
    callbacks.trade = [&logger, instrument](const Trade& e) {
        logger.info("ev trade     " + e.md_req_id + " " + instrument(e.symbol, e.security_id) + " yes " +
                    cpm::format_price(e.yes_price) + " (no " + cpm::format_price(cpm::kPriceOne - e.yes_price) +
                    ") x" + cpm::format_qty(e.qty) + " " + to_string(e.aggressor) + " id=" + e.entry_id + " @" +
                    to_utc_string(e.transact_time));
    };
    callbacks.volume = [&logger, instrument](const Volume& e) {
        logger.info(std::string("ev volume    ") + e.md_req_id + " " + instrument(e.symbol, e.security_id) + " " +
                    cpm::format_qty(e.contracts) + (e.snapshot ? " (snapshot)" : "") + " @" +
                    to_utc_string(e.transact_time));
    };
    callbacks.open_interest = [&logger, instrument](const OpenInterest& e) {
        logger.info(std::string("ev oi        ") + e.md_req_id + " " + instrument(e.symbol, e.security_id) + " " +
                    cpm::format_qty(e.contracts) + (e.snapshot ? " (snapshot)" : "") + " @" +
                    to_utc_string(e.transact_time));
    };
    callbacks.lifecycle = [&logger, instrument](const InstrumentLifecycle& e) {
        logger.info(std::string("ev lifecycle ") + instrument(e.symbol, e.security_id) + " " + to_string(e.status) +
                    "(" + std::to_string(e.status_code) + ")" + (e.text.empty() ? "" : " " + e.text) + " @" +
                    to_utc_string(e.transact_time));
    };
    callbacks.reject = [&logger](const MdReject& e) {
        const bool market_data = e.kind == MdReject::Kind::MarketDataRequest;
        logger.warn(std::string("ev reject    ") + (market_data ? "Y 262=" + e.md_req_id + " 281=" : "j 380=") +
                    e.reason_code + (e.text.empty() ? "" : ": " + e.text));
    };
    // The session log already carries the decode error and the raw message.
    return callbacks;
}


// What the session built, per instrument, at the end of the run.
void print_summary(const cpm::md::MarketState& state) {
    using namespace cpm::md;
    const auto levels = [](const std::vector<BookLevel>& side) {
        std::string out;
        for (const auto& level : side) {
            out += (out.empty() ? "" : " ") + cpm::format_price(level.price) + "x" + cpm::format_qty(level.qty);
        }
        return "[" + out + "]";
    };
    const auto price = [](const std::optional<cpm::Price>& p) {
        return p ? cpm::format_price(*p) : std::string("-");
    };
    const auto qty = [](const std::optional<cpm::Qty>& q) {
        return q ? cpm::format_qty(*q) : std::string("-");
    };

    const auto instruments = state.instruments();
    std::cout << "\n" << instruments.size() << " instrument(s)\n";
    for (const auto& i : instruments) {
        std::cout << "\n" << i.symbol << "  " << i.security_id << "  " << to_string(i.status) << "("
                  << i.status_code << ")" << (i.amendments ? "  amended x" + std::to_string(i.amendments) : "")
                  << "\n";
        if (i.book_valid) {
            std::cout << "  book " << i.book_md_req_id << "  yes" << levels(i.yes) << " no" << levels(i.no)
                      << "  yes_ask " << price(i.implied_yes_ask) << "  no_ask " << price(i.implied_no_ask)
                      << (i.crossed_or_locked ? "  CROSSED/LOCKED" : "") << "  @" << to_utc_string(i.book_time)
                      << "\n";
            std::cout << "  book stats: " << i.book_counters.updates << " updates, "
                      << i.book_counters.level_changes << " level changes, "
                      << i.book_counters.price_levels_checked << " 1023 checks, " << i.book_counters.anomalies
                      << " anomalies\n";
        } else {
            std::cout << "  book: no snapshot\n";
        }
        if (i.last_trade) {
            std::cout << "  last trade yes " << cpm::format_price(i.last_trade->yes_price) << " x"
                      << cpm::format_qty(i.last_trade->qty) << " " << to_string(i.last_trade->aggressor) << " @"
                      << to_utc_string(i.last_trade->time) << "\n";
        }
        std::cout << "  trades " << i.trades << " (" << i.duplicate_trades << " duplicate)"
                  << "  volume " << qty(i.volume) << "  open interest " << qty(i.open_interest) << "\n";
    }
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    try {
        args = parse_args(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "md_client: " << e.what() << '\n';
        usage();
        return 64;
    }

    // Block SIGINT/SIGTERM before QuickFIX starts its threads so they inherit the
    // mask; the main thread then collects the signals synchronously.
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);

    cpm::Logger logger(std::cout, args.verbose ? cpm::LogLevel::Debug : cpm::LogLevel::Info);

    try {
        cpm::md::MdClientOptions options;
        options.config = cpm::load_md_config(cpm::process_env());
        options.config.heartbeat_secs = args.heartbeat_secs;
        options.dict_dir = args.dict_dir;

        cpm::md::MdClient client(std::move(options), logger);
        cpm::md::MarketState state;
        // Stage 5 is fail-stop: the monitor reports, the main loop stops the
        // feed. Stopping from the handler would block a QuickFIX thread.
        std::atomic<bool> halted{false};
        cpm::md::FeedMonitor monitor(
            {std::chrono::seconds(args.stale_secs), std::chrono::milliseconds(250)},
            [&](const cpm::md::FeedHalt& halt) {
                logger.error(std::string("feed halted: ") + to_string(halt.reason) +
                             (halt.md_req_id.empty() ? "" : " on " + halt.md_req_id) +
                             (halt.symbol.empty() ? "" : " (" + halt.symbol + ")") +
                             (halt.detail.empty() ? "" : ": " + halt.detail));
                state.invalidate_books();
                halted.store(true);
            });
        state.on_anomaly([&logger](const cpm::md::BookAnomaly& anomaly) {
            logger.warn(std::string("feed anomaly: ") + to_string(anomaly.kind) + " on " + anomaly.symbol +
                        (anomaly.detail.empty() ? "" : " (" + anomaly.detail + ")"));
        });
        client.on_session_event(monitor.session_callback());
        auto consumers = cpm::md::combine(state.callbacks(), monitor.callbacks());
        client.on_market_data(args.print_events ? cpm::md::combine(event_printer(logger), std::move(consumers))
                                                : std::move(consumers));
        monitor.start();
        client.start();

        if (!client.wait_logged_on(std::chrono::seconds(args.logon_timeout_secs))) {
            const auto reason = client.last_logout_reason();
            logger.error("logon failed within " + std::to_string(args.logon_timeout_secs) + "s" +
                         (reason.empty() ? "" : ": " + reason));
            client.stop();
            return 2;
        }

        if (args.list_instruments) {
            const auto list = client.list_instruments(kListQuiet, kListTimeout);
            client.stop();
            if (!list.reject.empty()) {
                logger.error("instrument list rejected: " + list.reject);
                return 3;
            }
            std::cout << "\n" << list.instruments.size() << " instrument(s)\n";
            for (const auto& i : list.instruments) std::cout << i.symbol << "  " << i.security_id << '\n';
            return list.instruments.empty() ? 3 : 0;
        }

        const auto subscribe = [&](const cpm::md::ChannelRequest& request) {
            monitor.track(client.subscribe(request), request);
        };
        for (const auto& symbol : args.capture_symbols) {
            const std::vector<std::string> symbols{symbol};
            for (const int depth : args.depths) subscribe({cpm::md::Channel::Book, symbols, depth});
            subscribe({cpm::md::Channel::Trades, symbols, 0});
            subscribe({cpm::md::Channel::Lifecycle, symbols, 0});
        }
        if (args.all_lifecycle) subscribe({cpm::md::Channel::Lifecycle, {}, 0});
        if (args.all_trades) subscribe({cpm::md::Channel::Trades, {}, 0});
        if (args.probe_unknown) subscribe({cpm::md::Channel::Book, {kProbeSymbol}, args.depths.front()});

        int elapsed = 0;
        while ((args.duration_secs == 0 || elapsed < args.duration_secs) && !halted.load()) {
            const timespec one_second{1, 0};
            const int sig = sigtimedwait(&signals, nullptr, &one_second);
            if (sig == SIGINT || sig == SIGTERM) {
                logger.info(std::string("received ") + (sig == SIGINT ? "SIGINT" : "SIGTERM") +
                            ", logging out");
                break;
            }
            ++elapsed;
            // Settled instruments are never coming back, so they are dropped
            // after a grace period rather than accumulating for the life of
            // the process.
            if (elapsed % 60 == 0) {
                if (const auto dropped = state.evict_finished(std::chrono::minutes(5))) {
                    logger.debug("dropped " + std::to_string(dropped) + " settled instrument(s)");
                }
            }
        }

        monitor.stop();  // before the client: our own logout is not a disconnect
        if (halted.load()) {
            // The feed is untrustworthy, so stop without sending anything more.
            client.stop();
            if (!state.symbols().empty()) print_summary(state);
            return 4;
        }
        client.unsubscribe_all();
        client.stop();
        logger.info("stopped; last logout reason: " + client.last_logout_reason());
        if (!state.symbols().empty()) print_summary(state);
        return 0;
    } catch (const cpm::ConfigError& e) {
        logger.error(std::string("configuration: ") + e.what());
        return 78;
    } catch (const std::exception& e) {
        logger.error(std::string("fatal: ") + e.what());
        return 1;
    }
}
