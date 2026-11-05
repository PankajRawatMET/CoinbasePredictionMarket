// cpm_trader: the trading daemon. One market data session, one order entry
// session, and a host running N strategies over them.
//
// It owns one of each because the gateway allows one FIX session per API key
// and displaces the older one without firing cancel-on-disconnect, which would
// leave a displaced strategy's quotes resting with nobody managing them
// (docs/open-questions-gui.txt GUI-Q1, spec OE:197-201).
//
//   source config/creds.env
//   ./cpm_trader --allow-orders
//
// One at a time, too: it holds an exclusive flock on <bookmark>.lock for its
// life, so a second daemon on the same durable state is refused with exit 69
// before it reads a credential (design-gui.md section 2, GUI-N4).
//
// One file, one credential, three services: the CPM_ set is market data, the
// CPM_OE_ set order entry, and the CPM_REST_ set the instrument catalog
// (docs/design-roll.md R1b). Without the last of them a strategy can only
// name a full ticker: a
// series target like BTCD15M is refused, because FIX cannot say which window
// is open now.
//
// It serves the operator's page on 127.0.0.1 and is reached from Windows
// through an SSH local forward; the banner it prints says how. stdin still
// takes the same commands, which is the quickest way to drive it from a
// script and the fallback when there is no tunnel.
#include "cpm/binance/config.hpp"
#include "cpm/common/config.hpp"
#include "cpm/common/lock_file.hpp"
#include "cpm/common/log_files.hpp"
#include "cpm/common/log_ring.hpp"
#include "cpm/gui/server.hpp"
#include "cpm/common/logging.hpp"
#include "cpm/oms/order_rules.hpp"
#include "cpm/strategy/crypto_mm.hpp"
#include "cpm/trader/trader.hpp"

#include <poll.h>
#include <unistd.h>

#include <cerrno>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

// "2026-11-04T08:00:00.000Z" -> "080000": this run's directory inside the
// day's. The time it started is the one name that is unique per run, sorts
// in order, and means something to the operator who started it.
std::string run_directory_name(std::string_view stamp) {
    std::string name;
    const auto time = stamp.find('T');
    if (time == std::string_view::npos) return name;
    for (std::size_t i = time + 1; i < stamp.size() && name.size() < 6; ++i) {
        if (stamp[i] >= '0' && stamp[i] <= '9') name.push_back(stamp[i]);
    }
    return name;
}

// The one line the operator is looking for at startup is also the one that
// scrolls away fastest: the feed starts talking within seconds of it. So the
// link is shown once more, last of all, between two rules and in colour,
// where an eye can find it in the scrollback without reading timestamps.
//
// Only on a terminal, and never through the Logger. Escape codes belong in
// neither the log files nor the page's log panel, and both already hold the
// plain `open:` banner line - `grep -m1 open: logs/latest/cpm-trader.log`
// is what a script or a second window should read. A redirected or piped run
// therefore prints exactly what it printed before. NO_COLOR and TERM=dumb
// drop the colour and keep the rules.
void print_open_link(std::ostream& out, const std::string& url) {
    if (::isatty(STDOUT_FILENO) != 1) return;

    const char* term = std::getenv("TERM");
    const bool colour = std::getenv("NO_COLOR") == nullptr &&
                        (term == nullptr || std::string_view(term) != "dumb");
    const char* bright = colour ? "\033[1;96m" : "";  // bold bright cyan: the link
    const char* frame = colour ? "\033[36m" : "";    // cyan: the rules
    const char* faint = colour ? "\033[2m" : "";     // the label above it
    const char* off = colour ? "\033[0m" : "";

    // ASCII, not box drawing: this is read in PowerShell over SSH as often
    // as in a Linux terminal, and a legacy console renders U+2500 as noise.
    const std::string rule(url.size() + 4, '-');

    out << '\n'
        << frame << rule << off << '\n'
        << "  " << faint << "open the operator page:" << off << '\n'
        << "  " << bright << url << off << '\n'
        << frame << rule << off << "\n\n"
        << std::flush;
}

using namespace std::chrono_literals;
using cpm::trader::ControlApi;
using cpm::trader::Snapshot;

struct Args {
    bool allow_orders = false;
    bool verbose = false;
    int duration_secs = 0;  // 0 = until stopped
    std::string dict_dir = CPM_DEFAULT_DICT_DIR;
    std::string bookmark = "cpm-trader-bookmark.txt";
    // Where the daemon's own log files go, in a directory per UTC day under
    // it. Empty is stdout only, which is what every run before this did.
    std::string log_dir = "logs";
    // Off by default: a market maker changes what it is showing about once a
    // second per strategy, and most runs do not want that written down.
    bool quote_records = false;
    // The session's limits, shared by every strategy (GUI-Q10).
    cpm::Qty max_order_qty = 1000;          // 10.00 contracts per order
    double max_orders_per_sec = 8;
    cpm::Amount max_notional = 0;           // 0 = derived from max_order_qty
    int stale_after_secs = 30;
    // 1 or 10, as the spec allows. It is what the ladder in the GUI can show.
    int book_depth = 10;
    // The price grid the ladder is drawn on, and what a quote may be priced
    // at. Not on the FIX feed, so it is configured (O5).
    std::string ticks = "tapered";
    // The GUI. Loopback only, always: --port moves it, nothing widens it
    // (GUI-Q3). 0 asks the kernel for a free one.
    int port = 8080;
    // The reference venue. No --binance symbol means no feed and no guard:
    // the daemon is then exactly what it was before the speed bump.
    // --binance-on holds the connection open with nothing pinned, which is
    // the usual way to run it: streams come and go with the strategies.
    bool binance_on = false;
    std::vector<std::string> binance_symbols;
    // One override for the whole address, matching the one line in the
    // config file. Empty leaves whatever the file or the defaults say.
    std::string binance_url;
    // Where the venue and the mappings come from. Empty means
    // <dict-dir>/binance.conf if it is there.
    std::string binance_config;
    // Series the daemon watches for its own sake. Empty means the series
    // named in binance.conf, which is what an operator has already said they
    // trade; --watch none watches nothing.
    std::vector<std::string> watch;
    int binance_stale_ms = 5000;
    bool binance_stale_set = false;
};

void usage() {
    std::cerr << "usage: cpm_trader [--allow-orders] [--bookmark PATH] [--dict-dir DIR]\n"
                 "                  [--log-dir DIR] (a directory per day under it;\n"
                 "                                   empty for stdout only) [--quote-records]\n"
                 "                  [--max-order-qty Q] [--max-orders-per-sec N]\n"
                 "                  [--max-notional A] [--stale-after SECS]\n"
                 "                  [--port PORT] [--book-depth 1|10]\n"
                 "                  [--tick-structure tapered|tapered-cent|cent|deci]\n"
                 "                  [--duration SECS] [--verbose]\n"
                 "                  [--binance-on] [--binance SYMBOL]... [--binance-url URL]\n"
                 "                  [--binance-config PATH] [--binance-stale-after MS]\n"
                 "                  [--watch SERIES]... (default: the series in binance.conf)\n"
                 "env: the market data set (CPM_*), the order entry set (CPM_OE_*), and\n"
                 "     the REST set (CPM_REST_*) to target a series rather than a ticker\n"
                 "Without --allow-orders no strategy can be armed: it is an observer.\n"
                 "One daemon per --bookmark: it is locked (<bookmark>.lock) for the life of\n"
                 "     the process, and a second daemon on it is refused.\n";
}

int to_int(std::string_view flag, const char* value) {
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (*value == '\0' || *end != '\0') {
        throw std::invalid_argument(std::string(flag) + " needs an integer");
    }
    return static_cast<int>(parsed);
}

std::int64_t to_decimal(std::string_view flag, const char* value, int scale) {
    const auto parsed = cpm::parse_scaled(value, scale);
    if (!parsed) throw std::invalid_argument(std::string(flag) + " needs a decimal");
    return *parsed;
}

Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string_view flag = argv[i];
        auto value = [&]() -> const char* {
            if (i + 1 >= argc) throw std::invalid_argument(std::string(flag) + " needs a value");
            return argv[++i];
        };
        if (flag == "--allow-orders") a.allow_orders = true;
        else if (flag == "--binance-on") a.binance_on = true;
        else if (flag == "--binance") a.binance_symbols.push_back(value());
        else if (flag == "--binance-config") a.binance_config = value();
        else if (flag == "--watch") a.watch.push_back(value());
        else if (flag == "--binance-url") a.binance_url = value();
        else if (flag == "--binance-stale-after") { a.binance_stale_ms = to_int(flag, value()); a.binance_stale_set = true; }
        else if (flag == "--verbose") a.verbose = true;
        else if (flag == "--duration") a.duration_secs = to_int(flag, value());
        else if (flag == "--bookmark") a.bookmark = value();
        else if (flag == "--log-dir") a.log_dir = value();
        else if (flag == "--quote-records") a.quote_records = true;
        else if (flag == "--dict-dir") a.dict_dir = value();
        else if (flag == "--stale-after") a.stale_after_secs = to_int(flag, value());
        else if (flag == "--port") a.port = to_int(flag, value());
        else if (flag == "--book-depth") a.book_depth = to_int(flag, value());
        else if (flag == "--tick-structure") a.ticks = value();
        else if (flag == "--max-orders-per-sec") a.max_orders_per_sec = to_int(flag, value());
        else if (flag == "--max-order-qty") a.max_order_qty = to_decimal(flag, value(), cpm::kQtyScale);
        else if (flag == "--max-notional") a.max_notional = to_decimal(flag, value(), cpm::kAmtScale);
        else if (flag == "--help" || flag == "-h") { usage(); std::exit(0); }
        else throw std::invalid_argument("unknown option " + std::string(flag));
    }
    if (a.max_order_qty <= 0) throw std::invalid_argument("--max-order-qty must be positive");
    if (a.max_orders_per_sec <= 0) throw std::invalid_argument("--max-orders-per-sec must be positive");
    if (a.port < 0 || a.port > 65535) throw std::invalid_argument("--port must be a port number");
    if (a.book_depth != 1 && a.book_depth != 10) {
        throw std::invalid_argument("--book-depth must be 1 or 10, which is what the spec allows");
    }
    return a;
}

// `heading`, when given, says what this snapshot is. The shutdown summary
// needs it: that snapshot is taken before the sessions are logged out, so its
// first line would otherwise read "daemon live  md up  oe up" printed directly
// underneath the two logout confirmations.
void print_snapshot(const Snapshot& snapshot, const char* heading = nullptr) {
    if (heading) std::cout << heading << '\n';
    std::cout << "daemon " << to_string(snapshot.state)
              << (snapshot.allow_orders ? " [orders allowed]" : " [observer: --allow-orders not given]")
              << "  md " << (snapshot.market_data.logged_on ? "up" : "down") << "  oe "
              << (snapshot.order_entry.logged_on ? "up" : "down") << "  recovery "
              << snapshot.recovery << "  rev " << snapshot.revision << '\n';
    if (!snapshot.halt_reason.empty()) std::cout << "HALTED: " << snapshot.halt_reason << '\n';
    for (const auto& strategy : snapshot.strategies) {
        std::cout << "  " << strategy.id << "  " << strategy.kind << "  "
                  << to_string(strategy.state) << (strategy.armed ? " ARMED" : " disarmed");
        for (const auto& leg : strategy.status.legs) {
            std::cout << "  " << to_string(leg.side) << " " << cpm::format_qty(leg.quantity) << "@"
                      << cpm::format_price(leg.price) << (leg.cancel_pending ? "(cancelling)" : "");
        }
        std::cout << "  placed " << strategy.status.placed << " cancelled "
                  << strategy.status.cancelled << " fills " << strategy.status.fills;
        if (!strategy.status.reason.empty()) std::cout << "  (" << strategy.status.reason << ")";
        if (!strategy.note.empty()) std::cout << "  note: " << strategy.note;
        std::cout << '\n';
    }
    for (const auto& book : snapshot.books) {
        std::cout << "  " << book.symbol << "  "
                  << (book.best_yes_bid ? cpm::format_price(*book.best_yes_bid) : "-") << " / "
                  << (book.implied_yes_ask ? cpm::format_price(*book.implied_yes_ask) : "-")
                  << (book.mid ? "  mid " + cpm::format_price(*book.mid) : "")
                  << "  " << book.status << (book.crossed_or_locked ? "  CROSSED" : "")
                  << (book.valid ? "" : "  (no valid book)") << '\n';
    }
    for (const auto& position : snapshot.positions) {
        std::cout << "  " << position.symbol << "  net " << cpm::format_qty(position.net)
                  << "  cash " << cpm::format_amount(position.cash) << "  fees "
                  << cpm::format_amount(position.fees) << "  fills " << position.fills << '\n';
    }
    // P&L under the positions it belongs to. A dash is "no mark", which is
    // not the same answer as zero (GUI-Q15).
    const auto amount = [](const std::optional<cpm::Amount>& value) {
        return value ? cpm::format_amount(*value) : std::string("-");
    };
    for (const auto& pnl : snapshot.pnl) {
        std::cout << "  " << pnl.symbol << "  realized " << cpm::format_amount(pnl.realized)
                  << "  unrealized " << amount(pnl.unrealized) << "  total "
                  << amount(pnl.total)
                  << (pnl.mark ? "  at mid " + cpm::format_price(*pnl.mark) : "  (no mid)")
                  << '\n';
    }
    if (!snapshot.pnl.empty()) {
        std::cout << "  P&L  realized " << cpm::format_amount(snapshot.pnl_total.realized)
                  << "  unrealized " << amount(snapshot.pnl_total.unrealized) << "  total "
                  << amount(snapshot.pnl_total.total) << '\n';
    }
}

void print_help(const ControlApi& api) {
    std::cout << "commands:\n"
                 "  list                         the snapshot\n"
                 "  kinds                        the strategy kinds and their parameters\n"
                 "  create KIND name=value ...   create a strategy\n"
                 "  set ID name=value ...        retune one that is not armed\n"
                 "  start|stop|arm|disarm|rm ID  act on one\n"
                 "  start-feed|stop-feed ID      its book, without its quotes\n"
                 "  flatten SYMBOL [QTY]         exit a position, all of it by default\n"
                 "  stop-all                     the kill switch\n"
                 "  quit                         shut down cleanly\n";
    for (const auto& kind : api.kinds()) {
        std::cout << "  " << kind.name << ": " << kind.help << '\n';
        for (const auto& spec : kind.schema) {
            std::cout << "      " << spec.name << "=" << (spec.required() ? "REQUIRED" : spec.default_value)
                      << "  " << spec.help << '\n';
        }
    }
}

// An arrow key in a cooked-mode terminal is not handled for us: the three
// bytes ESC [ B land in the line like any other input, and a line of them
// would come back as "refused: unknown command ^[[B^[[B...". There is no line
// editing or history on this console (the page is the richer surface, and
// rlwrap gives the rest), so the least surprising thing is to ignore the keys
// that do nothing rather than answer them with a refusal.
std::string without_control_bytes(std::string_view line) {
    std::string out;
    out.reserve(line.size());
    for (std::size_t i = 0; i < line.size(); ++i) {
        const unsigned char byte = static_cast<unsigned char>(line[i]);
        if (byte == 0x1b) {
            // CSI (ESC [) and SS3 (ESC O) run to a final byte in 0x40..0x7e;
            // anything else after ESC is a single-byte sequence.
            if (i + 1 < line.size() && (line[i + 1] == '[' || line[i + 1] == 'O')) {
                i += 2;
                while (i < line.size() && (static_cast<unsigned char>(line[i]) < 0x40 ||
                                           static_cast<unsigned char>(line[i]) > 0x7e)) {
                    ++i;
                }
            } else {
                ++i;
            }
            continue;
        }
        if (byte == 0x7f || (byte < 0x20 && byte != '\t')) continue;
        out.push_back(line[i]);
    }
    return out;
}

// Whole lines from a file descriptor, without blocking and without iostreams.
// std::cin cannot be used with poll(): the first getline() drains the
// descriptor into the stream's own buffer, so poll() then reports nothing
// available while complete lines are still queued inside std::cin - which
// silently stalled every command after the first - and a getline() on a
// partial line would block the daemon's tick loop outright.
class LineReader {
public:
    enum class Status { Ok, Closed };

    Status read_into(std::vector<std::string>& lines) {
        char chunk[4096];
        const ssize_t got = ::read(STDIN_FILENO, chunk, sizeof(chunk));
        if (got == 0) return Status::Closed;
        if (got < 0) {
            return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? Status::Ok
                                                                              : Status::Closed;
        }
        buffer_.append(chunk, static_cast<std::size_t>(got));
        std::size_t start = 0;
        for (std::size_t i = 0; i < buffer_.size(); ++i) {
            if (buffer_[i] != '\n') continue;
            lines.push_back(without_control_bytes(std::string_view(buffer_).substr(start, i - start)));
            start = i + 1;
        }
        // Anything after the last newline is a partial line: it waits here for
        // the rest rather than blocking a read.
        buffer_.erase(0, start);
        return Status::Ok;
    }

private:
    std::string buffer_;
};

// Returns false when the operator asked to quit.
bool handle(const std::string& line, ControlApi& api) {
    std::istringstream input(line);
    std::string command;
    if (!(input >> command)) return true;

    const auto report = [](const cpm::trader::Result& result) {
        if (result.ok()) {
            std::cout << "ok" << (result.id.empty() ? "" : " " + result.id) << '\n';
        } else {
            std::cout << "refused: " << result.error << '\n';
        }
    };

    if (command == "quit" || command == "exit") return false;
    if (command == "help") { print_help(api); return true; }
    if (command == "list") { print_snapshot(api.snapshot()); return true; }
    if (command == "kinds") { print_help(api); return true; }
    if (command == "stop-all") { report(api.stop_all()); return true; }

    if (command == "create") {
        cpm::strategy::StrategySpec spec;
        if (!(input >> spec.kind)) { std::cout << "refused: create needs a kind\n"; return true; }
        std::string pair;
        while (input >> pair) {
            const auto equals = pair.find('=');
            if (equals == std::string::npos) {
                std::cout << "refused: " << pair << " is not name=value\n";
                return true;
            }
            spec.params[pair.substr(0, equals)] = pair.substr(equals + 1);
        }
        report(api.create(spec));
        return true;
    }

    // Before the generic path below, because what follows it is a symbol and
    // not a strategy id. "flatten" and not "exit": exit already quits.
    if (command == "flatten") {
        std::string symbol;
        if (!(input >> symbol)) {
            std::cout << "refused: flatten needs a symbol\n";
            return true;
        }
        cpm::Qty qty = 0;  // all of it
        std::string requested;
        if (input >> requested) {
            const auto parsed = cpm::parse_qty(requested);
            if (!parsed) {
                std::cout << "refused: " << requested << " is not a quantity\n";
                return true;
            }
            qty = *parsed;
        }
        report(api.exit_position(symbol, qty));
        return true;
    }

    std::string id;
    if (!(input >> id)) { std::cout << "refused: " << command << " needs a strategy id\n"; return true; }

    if (command == "set") {
        // The console's half of GUI-Q30: retune a strategy that is not
        // armed, in the same name=value shape create takes.
        cpm::strategy::ParamValues values;
        std::string pair;
        while (input >> pair) {
            const auto equals = pair.find('=');
            if (equals == std::string::npos) {
                std::cout << "refused: " << pair << " is not name=value\n";
                return true;
            }
            values[pair.substr(0, equals)] = pair.substr(equals + 1);
        }
        if (values.empty()) { std::cout << "refused: set needs name=value\n"; return true; }
        report(api.update(id, values));
        return true;
    }

    if (command == "start") report(api.start(id));
    else if (command == "stop") report(api.stop(id));
    else if (command == "arm") report(api.arm(id));
    else if (command == "disarm") report(api.disarm(id));
    else if (command == "start-feed") report(api.start_feed(id));
    else if (command == "stop-feed") report(api.stop_feed(id));
    else if (command == "rm" || command == "destroy") report(api.destroy(id));
    else std::cout << "refused: unknown command " << command << " (try help)\n";
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    try {
        args = parse_args(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "cpm_trader: " << e.what() << '\n';
        usage();
        return 64;
    }

    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    // Closing the PowerShell window sends SIGHUP, which is default-terminate:
    // without this the daemon would die without withdrawing (GUI-N2). Run it
    // detached (systemd-run, screen, tmux) so a dropped connection does not
    // stop a live strategy at all.
    sigaddset(&signals, SIGHUP);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);

    cpm::Logger logger(std::cout, args.verbose ? cpm::LogLevel::Debug : cpm::LogLevel::Info);

    // The log on disk, before anything else is done: a run whose first line
    // is a configuration failure is exactly the run whose log is wanted
    // afterwards. stdout keeps everything either way - these files are a
    // copy of it, and a daemon that cannot write them still trades.
    // This run's own directory, named for the moment it started: one
    // daemon's whole account in one place. Appending six starts into one
    // file made every question about 2026-11-04 begin by working out which
    // process was speaking.
    const auto started = cpm::utc_stamp();
    const std::string run_id = run_directory_name(started);
    std::optional<cpm::LogFiles> log_files;
    if (!args.log_dir.empty()) {
        log_files.emplace(args.log_dir, run_id);
        if (const auto failed = log_files->open_now(started); !failed.empty()) {
            logger.warn("logging to stdout only: " + failed);
            log_files.reset();
        } else {
            logger.also_to(log_files->sink());
            logger.info("log " + log_files->narrative_path() + ", a strategy's own quoting in " +
                        log_files->strategy_path() + ", wire traffic in " +
                        log_files->wire_path());
            // A fixed name for "the run going on now", so an operator and a
            // script can both find it without reading the banner.
            if (const auto failed_link = cpm::point_latest_at(args.log_dir, started, run_id);
                !failed_link.empty()) {
                logger.debug("no latest link: " + failed_link);
            }
        }
    }

    // One daemon per bookmark, and this is where that is enforced. Two of
    // them would allocate ClOrdIDs from the same counter and save over each
    // other's last_exec_id, so neither could replay after a restart
    // (OE-Q7); worse, the second logon would displace the first session at
    // the gateway WITHOUT firing cancel-on-disconnect, leaving a live
    // strategy's quotes resting with nobody managing them (GUI-Q1).
    //
    // The lock is a sidecar and never the bookmark itself: BookmarkFile
    // replaces that file by rename on every save, so a lock on it would
    // stop protecting anything after the first write. It follows
    // --bookmark, so a second daemon on its own state - a simulator run
    // beside a sandbox one - is still allowed, and two on the same state
    // are not.
    //
    // Before the credentials are read, so a refused daemon never opens a
    // session; after the log files, so the refusal is in the day's log.
    cpm::LockFile lock((args.bookmark.empty() ? std::string("cpm-trader") : args.bookmark) +
                       ".lock");
    if (const auto refused = lock.acquire(); !refused.empty()) {
        logger.error("a cpm_trader is already running: " + refused +
                     " - stop it, or give this one its own --bookmark");
        return 69;
    }

    try {
        const auto env = cpm::process_env();

        cpm::trader::TraderOptions options;
        options.md = cpm::load_md_config(env);
        options.oe = cpm::load_oe_config(env);
        options.dict_dir = args.dict_dir;
        options.bookmark = args.bookmark;
        // The record files live beside the .log files, in the same directory
        // and under the same flag.
        options.log_dir = args.log_dir;
        options.log_run = run_id;
        options.quote_records = args.quote_records;
        options.allow_orders = args.allow_orders;
        options.stale_after = std::chrono::seconds(args.stale_after_secs);
        options.book_depth = args.book_depth;
        // Optional: without it a series cannot be resolved at startup, and
        // everything that names a full ticker still works (R-Q2).
        const auto rest = cpm::load_rest_config(env);
        options.rest.api_key = rest.api_key;
        options.rest.passphrase = rest.passphrase;
        options.rest.secret_b64 = rest.secret_b64;
        options.rest.url = rest.url;
        options.rest.ca_file = rest.ca_file;
        if (!options.rest.configured()) {
            logger.warn(
                "REST is not configured, so a series like BTCD15M cannot be resolved: name a "
                "full ticker, or source config/creds.env and restart");
        }
        options.reference_enabled = args.binance_on || !args.binance_symbols.empty();

        // The file first, then the one flag that overrides it: an operator
        // who has set the venue once in config/binance.conf should not have
        // to repeat it on every command line, and one who points a single
        // run somewhere else should not have to edit the file (B-Q15).
        cpm::binance::Config reference;
        const std::string config_path =
            args.binance_config.empty() ? args.dict_dir + "/binance.conf" : args.binance_config;
        bool config_found = false;
        if (const auto failed = cpm::binance::load_config(config_path, reference, config_found);
            !failed.empty()) {
            logger.error(failed);
            return 78;
        }
        if (!config_found && !args.binance_config.empty()) {
            // Named explicitly and not there: that is a mistake, not a
            // default. Carrying on would point the daemon somewhere the
            // operator did not choose.
            logger.error("no such file: " + config_path);
            return 78;
        }
        options.reference = reference.feed;
        options.reference_symbols = reference.symbols;

        // What the daemon watches on its own: the series named with --watch,
        // or, when none were, the ones binance.conf already maps - an
        // operator who has said which series they price has said which ones
        // they trade. "none" is how to ask for the old behaviour, where a
        // book existed only because a strategy claimed it.
        options.watch = args.watch;
        if (options.watch.size() == 1 && options.watch.front() == "none") {
            options.watch.clear();
        } else if (options.watch.empty()) {
            // Whatever the file declares. A key is matched against both a
            // ticker's series and its asset when pricing (BTCD15M and BTC
            // would both reach BTCUSDT), but only what is written in the
            // file is here, and a key that names no series this venue lists
            // is reported as "not watching <key>" and left alone.
            for (const auto& [key, mapping] : reference.symbols) {
                (void)mapping;
                options.watch.push_back(key);
            }
        }

        for (const auto& symbol : args.binance_symbols) options.reference.symbols.push_back(symbol);
        if (!args.binance_url.empty()) {
            if (const auto failed = cpm::binance::parse_ws_url(args.binance_url, options.reference);
                !failed.empty()) {
                logger.error("--binance-url: " + failed);
                return 64;
            }
        }
        if (args.binance_stale_set) {
            options.reference.stale_after = std::chrono::milliseconds(args.binance_stale_ms);
        }
        if (options.reference_enabled) {
            logger.info(std::string("reference venue ") +
                        (options.reference.tls ? "wss://" : "ws://") + options.reference.host +
                        ":" + std::to_string(options.reference.port) + options.reference.path +
                        (config_found ? ", from " + config_path : " (built-in default)"));
            if (options.reference_symbols.empty()) {
                // Loud, because the guard cannot work without one and the
                // only symptom otherwise is a strategy that never quotes.
                logger.warn("no instrument is mapped to a reference symbol, so no strategy can "
                            "use PricingMidMoveThreshold: add a map. line to " + config_path);
            }
            for (const auto& [instrument, mapping] : options.reference_symbols) {
                logger.info("  " + instrument + " -> " + mapping.symbol + ", tick " +
                            cpm::format_scaled(mapping.tick_size, cpm::binance::kScale));
            }
        }

        if (const auto structure = cpm::md::tick_structure_from(args.ticks); structure) {
            options.ticks = *structure;
        } else {
            logger.error("--tick-structure must be tapered, tapered-cent, cent or deci");
            return 64;
        }
        options.limits.max_quantity = args.max_order_qty;
        options.limits.max_orders_per_sec = args.max_orders_per_sec;
        // A per-order cap, not an exposure limit: the worst a single order of
        // the largest admissible size could cost.
        options.limits.max_notional =
            args.max_notional > 0 ? args.max_notional : args.max_order_qty * cpm::kPriceOne * 10;

        if (args.allow_orders && options.oe.subaccount.empty()) {
            logger.error("CPM_OE_SUBACCOUNT is required to place an order");
            return 78;
        }
        if (!args.allow_orders) {
            logger.warn("observer: no strategy can be armed (restart with --allow-orders to trade)");
        }

        cpm::strategy::StrategyRegistry registry;
        cpm::strategy::register_crypto_mm(registry);

        cpm::trader::Trader trader(std::move(options), registry, logger);
        if (const auto error = trader.start(); !error.empty()) {
            logger.error(error);
            return 2;
        }
        // The tail of the log, for the panel at the bottom of the page: the
        // daemon's own events, and anything that went wrong wherever it
        // came from. The raw FIX traffic and a strategy's running
        // commentary on its own quotes are the bulk of what this daemon
        // writes and are left out, because a panel scrolling past at thirty
        // lines a second hides the one line an operator is looking for
        // (GUI-Q27, GUI-Q28). Both still go to the log file in full.
        cpm::LogRing log_tail(500);
        logger.also_to([&log_tail](cpm::LogLevel level, cpm::LogSource source,
                                   std::string_view stamp, std::string_view message) {
            if (source != cpm::LogSource::Daemon && level < cpm::LogLevel::Warn) return;
            log_tail.add(level, stamp, message);
        });

        cpm::gui::Server::Options gui;
        gui.port = args.port;
        gui.logs = &log_tail;
        cpm::gui::Server server(trader.api(), std::move(gui), logger);
        if (const auto error = server.start(); !error.empty()) {
            logger.error(error);
            trader.stop();
            return 73;
        }
        for (const auto& line : server.banner(args.allow_orders)) logger.info(line);
        logger.info("ready: the page has the controls, and stdin takes the same ones (help)");
        print_open_link(std::cout, server.url());

        LineReader console;
        bool stdin_open = true;
        int elapsed_ms = 0;
        bool quit = false;
        // 250 ms is the floor, not the cadence. The reference feed writes a
        // byte when the underlying moves, and that must run a pass now
        // rather than at the end of a sleep: it is the whole of the speed
        // bump (B-Q5, docs/design-binance.md section 5).
        auto last_tick = cpm::oms::Clock::now();
        while (!quit && (args.duration_secs == 0 || elapsed_ms < args.duration_secs * 1000)) {
            pollfd watch[2];
            int watched = 0;
            if (stdin_open) watch[watched++] = pollfd{STDIN_FILENO, POLLIN, 0};
            const int wake = trader.wake_fd();
            if (wake >= 0) watch[watched++] = pollfd{wake, POLLIN, 0};

            if (watched == 0) {
                const timespec quarter_second{0, 250'000'000};
                nanosleep(&quarter_second, nullptr);
            } else if (poll(watch, static_cast<nfds_t>(watched), 250) > 0) {
                for (int i = 0; i < watched; ++i) {
                    if ((watch[i].revents & POLLIN) == 0) continue;
                    if (watch[i].fd == wake) {
                        trader.drain_wake();
                        continue;
                    }
                    std::vector<std::string> lines;
                    if (console.read_into(lines) == LineReader::Status::Closed) {
                        // Redirected input that has run out, or a closed
                        // terminal: stop polling it rather than spinning.
                        stdin_open = false;
                        logger.info("stdin closed; running until a signal or --duration");
                    }
                    // Every line that arrived, not just the first.
                    for (const auto& line : lines) {
                        if (!handle(line, trader.api())) {
                            quit = true;
                            break;
                        }
                    }
                }
            }

            // Non-blocking: just checking whether one has arrived.
            const timespec none{0, 0};
            const int sig = sigtimedwait(&signals, nullptr, &none);
            if (sig == SIGINT || sig == SIGTERM || sig == SIGHUP) {
                logger.info("interrupted; withdrawing every strategy");
                break;
            }
            // Time really elapsed, not a count of passes: a busy reference
            // feed now wakes this loop many times a second, and --duration
            // must still mean seconds.
            const auto now = cpm::oms::Clock::now();
            elapsed_ms += static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(now - last_tick).count());
            last_tick = now;
            trader.tick(now);
        }

        const auto final_snapshot = trader.api().snapshot();
        // The page goes before the daemon does: a browser reading a halt is
        // better served by a closed stream than by a snapshot of a daemon
        // that is in the middle of logging out.
        server.stop();
        trader.stop();
        std::cout << '\n';
        print_snapshot(final_snapshot,
                       "-- where things stood when the shutdown began; the sessions are down now --");
        return trader.halted() ? 4 : 0;
    } catch (const cpm::ConfigError& e) {
        logger.error(std::string("configuration: ") + e.what());
        return 78;
    } catch (const std::exception& e) {
        logger.error(std::string("fatal: ") + e.what());
        return 1;
    }
}
