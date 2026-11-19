// crypto_mm: quotes a bid and an ask a fixed number of ticks apart around the
// mid, each for a fixed quantity, on one instrument.
//
// It is the first binary that uses both services: the book comes from the
// market data session and the orders go out on the order entry session, each
// with its own credentials and its own endpoint guard.
//
//   source config/creds.env
//   ./crypto_mm --symbol BTCD15M-... --width-ticks 2 --qty 1 --allow-orders
//
// Without --allow-orders it runs as a dry run: it computes and logs the quotes
// it would place and sends nothing.
//
// Since G1 the strategy runs under StrategyHost, the same host cpm_trader will
// use, with exactly one strategy in it. The flags are unchanged: they are
// validated against the CryptoMM parameter schema, so this binary and the GUI
// cannot disagree about what a parameter means.
#include "cpm/common/config.hpp"
#include "cpm/common/logging.hpp"
#include "cpm/md/market_state.hpp"
#include "cpm/md/md_client.hpp"
#include "cpm/oms/oe_client.hpp"
#include "cpm/oms/order_rules.hpp"
#include "cpm/oms/order_state.hpp"
#include "cpm/oms/reconciler.hpp"
#include "cpm/strategy/crypto_mm.hpp"
#include "cpm/strategy/host.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

namespace {

using namespace std::chrono_literals;

struct Args {
    // Everything the schema describes arrives here as typed text and is
    // validated by cpm::strategy::validate(), not by this file.
    cpm::strategy::ParamValues params;
    int duration_secs = 60;
    bool allow_orders = false;
    bool verbose = false;
    std::string dict_dir = CPM_DEFAULT_DICT_DIR;
    std::string bookmark = "crypto-mm-bookmark.txt";
};

void usage() {
    std::cerr << "usage: crypto_mm --symbol SYMBOL [--width-ticks N] [--qty Q]\n"
                 "                 [--max-inventory Q] [--requote-threshold N]\n"
                 "                 [--withdraw-before SECS] [--ticks GRID]\n"
                 "                 [--duration SECS]\n"
                 "                 [--allow-orders] [--bookmark PATH] [--verbose]\n"
                 "env: the market data set (CPM_*) and the order entry set (CPM_OE_*)\n"
                 "Without --allow-orders nothing is sent: it logs the quotes it would place.\n";
    std::cerr << "\nstrategy parameters:\n";
    for (const auto& spec : cpm::strategy::crypto_mm_schema()) {
        std::cerr << "  --" << spec.name << "  " << spec.help;
        if (spec.required()) {
            std::cerr << " (required)";
        } else {
            std::cerr << " (default " << spec.default_value << ")";
        }
        std::cerr << '\n';
    }
}

int to_int(std::string_view flag, const char* value) {
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (*value == '\0' || *end != '\0') {
        throw std::invalid_argument(std::string(flag) + " needs an integer");
    }
    return static_cast<int>(parsed);
}

Args parse_args(int argc, char** argv) {
    Args a;
    const auto schema = cpm::strategy::crypto_mm_schema();
    for (int i = 1; i < argc; ++i) {
        const std::string_view flag = argv[i];
        auto value = [&]() -> const char* {
            if (i + 1 >= argc) throw std::invalid_argument(std::string(flag) + " needs a value");
            return argv[++i];
        };
        if (flag == "--duration") a.duration_secs = to_int(flag, value());
        else if (flag == "--allow-orders") a.allow_orders = true;
        else if (flag == "--bookmark") a.bookmark = value();
        else if (flag == "--dict-dir") a.dict_dir = value();
        else if (flag == "--verbose") a.verbose = true;
        else if (flag == "--help" || flag == "-h") { usage(); std::exit(0); }
        else if (flag.size() > 2 && flag.substr(0, 2) == "--") {
            // Any remaining --name is a strategy parameter if the schema has
            // one by that name, so the flags and the GUI form stay the same
            // set by construction.
            const std::string name(flag.substr(2));
            const bool known = std::any_of(schema.begin(), schema.end(),
                                           [&](const cpm::strategy::ParamSpec& spec) {
                                               return spec.name == name;
                                           });
            if (!known) throw std::invalid_argument("unknown option " + std::string(flag));
            a.params[name] = value();
        }
        else throw std::invalid_argument("unknown option " + std::string(flag));
    }
    // Validated here so a bad flag is refused before any session is opened.
    const auto checked = cpm::strategy::validate(schema, a.params);
    if (!checked.ok()) throw std::invalid_argument(checked.error);
    a.params = checked.values;
    return a;
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    try {
        args = parse_args(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "crypto_mm: " << e.what() << '\n';
        usage();
        return 64;
    }
    const std::string symbol = cpm::strategy::param_text(args.params, "symbol");
    const auto quantity = cpm::strategy::param_decimal(args.params, "qty", cpm::kQtyScale);

    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    // An SSH drop sends SIGHUP, which is default-terminate: without this the
    // run would die without withdrawing its quotes (GUI-N2).
    sigaddset(&signals, SIGHUP);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);

    cpm::Logger logger(std::cout, args.verbose ? cpm::LogLevel::Debug : cpm::LogLevel::Info);

    try {
        const auto env = cpm::process_env();

        // --- the book -----------------------------------------------------
        cpm::md::MdClientOptions md_options;
        md_options.config = cpm::load_md_config(env);
        md_options.dict_dir = args.dict_dir;
        cpm::md::MarketState book;
        book.on_anomaly([&logger](const cpm::md::BookAnomaly& anomaly) {
            logger.warn(std::string("feed anomaly: ") + to_string(anomaly.kind) + " on " + anomaly.symbol +
                        (anomaly.detail.empty() ? "" : " (" + anomaly.detail + ")"));
        });
        cpm::md::MdClient md(std::move(md_options), logger);
        md.on_market_data(book.callbacks());

        // --- the orders ---------------------------------------------------
        cpm::oms::OeClientOptions oe_options;
        oe_options.config = cpm::load_oe_config(env);
        oe_options.dict_dir = args.dict_dir;
        if (oe_options.config.subaccount.empty()) {
            logger.error("CPM_OE_SUBACCOUNT is required to place an order");
            return 78;
        }
        cpm::oms::OeClient oe(std::move(oe_options), logger);
        cpm::oms::OrderState orders;
        std::atomic<bool> halted{false};
        cpm::oms::Reconciler reconciler({}, oe, orders, cpm::oms::BookmarkFile(args.bookmark), logger,
                                        [&](const cpm::oms::Halt& halt) {
                                            logger.error(std::string("halted: ") + to_string(halt.reason) +
                                                         " - " + halt.detail);
                                            halted.store(true);
                                        });
        orders.on_anomaly(reconciler.anomaly_handler());
        oe.on_session_event(reconciler.session_callback());

        // --- the strategy, under the host ---------------------------------
        cpm::strategy::StrategyRegistry registry;
        cpm::strategy::register_crypto_mm(registry);

        // Unique across restarts by the run tag in the prefix, not by a
        // counter saved to disk on every allocation (cpm::oms::run_tag).
        const auto cl_ord_prefix = cpm::oms::run_prefix("mm-", cpm::oms::run_tag());
        std::atomic<std::uint64_t> counter{0};
        cpm::strategy::StrategyHooks venue;
        venue.can_submit = [&] { return args.allow_orders && reconciler.submission_allowed(); };
        venue.orders_permitted = [&] { return args.allow_orders; };
        venue.send = [&](const cpm::oms::NewOrder& order) {
            orders.record_sent(order);
            return oe.send(order);
        };
        venue.cancel = [&](const cpm::oms::CancelOrder& request) {
            orders.record_cancel_sent(request);
            return oe.cancel(request);
        };
        venue.order_finished = [&](const std::string& cl_ord_id) {
            const auto record = orders.order(cl_ord_id);
            return record && record->terminal;
        };
        venue.resting_orders = [&](const std::string& symbol) {
            std::vector<cpm::strategy::LiveLeg> out;
            for (const auto& order : orders.open_orders()) {
                if (order.symbol != symbol || order.leaves_quantity <= 0) continue;
                cpm::strategy::LiveLeg leg;
                leg.side = order.side;
                leg.cl_ord_id = order.cl_ord_id;
                leg.price = order.price;
                leg.quantity = order.leaves_quantity;
                leg.cancel_pending = order.cancel_pending;
                out.push_back(std::move(leg));
            }
            return out;
        };
        venue.next_cl_ord_id = [&] { return cpm::oms::make_cl_ord_id(cl_ord_prefix, ++counter); };
        venue.log = [&logger](const std::string& line) { logger.info(line); };

        cpm::strategy::StrategyHost::Feeds feeds;
        feeds.book = [&book](const std::string& name) { return book.instrument(name); };
        feeds.position = [&orders](const std::string& name) {
            return orders.position(name).value_or(cpm::oms::Position{});
        };
        cpm::strategy::StrategyHost host({}, registry, std::move(venue), std::move(feeds), logger);

        std::string id;
        if (const auto error = host.create({"CryptoMM", args.params}, id); !error.empty()) {
            logger.error(error);
            return 64;
        }

        cpm::oms::OeCallbacks strategy_callbacks;
        strategy_callbacks.report = [&](const cpm::oms::ExecutionReport& report) {
            host.on_report(report);
        };
        oe.on_events(cpm::oms::combine_oe(
            cpm::oms::combine_oe(orders.callbacks(), reconciler.callbacks()), strategy_callbacks));
        cpm::oms::OrderLimits limits;
        limits.allow_orders = args.allow_orders;
        limits.max_quantity = quantity * 2;
        // Two sides, a few re-quotes: a few orders a second is plenty.
        limits.max_orders_per_sec = 4;
        limits.max_notional = quantity * cpm::kPriceOne * 10;
        oe.set_limits(limits);

        if (!args.allow_orders) {
            logger.warn("dry run: computing quotes but sending nothing (pass --allow-orders to trade)");
        }

        reconciler.start();
        oe.start();
        md.start();
        const auto oe_logon_timeout = args.allow_orders ? 20s : 3s;
        if (!md.wait_logged_on(20s)) {
            logger.error("market data logon failed: " + md.last_logout_reason());
            return 2;
        }
        if (!oe.wait_logged_on(oe_logon_timeout)) {
            const auto reason = oe.last_logout_reason();
            if (args.allow_orders) {
                logger.error("order entry logon failed: " + reason);
                return 2;
            }
            // A dry run only needs the book: it computes the quotes it would
            // place and sends nothing, so an order session is not required.
            logger.warn("order entry is not available (" +
                        (reason.empty() ? "no logon" : reason) +
                        "), continuing as a dry run on market data alone");
        }
        // The book, and the lifecycle so the strategy knows when to stop.
        md.subscribe({cpm::md::Channel::Book, {symbol}, 10});
        md.subscribe({cpm::md::Channel::Lifecycle, {symbol}, 0});

        host.start(id);
        // Armed from the command line: --allow-orders is the other key, and
        // the host's can_submit folds both together.
        if (args.allow_orders) {
            if (const auto error = host.arm(id); !error.empty()) logger.warn(error);
        }

        int elapsed_ms = 0;
        while ((args.duration_secs == 0 || elapsed_ms < args.duration_secs * 1000) && !halted.load()) {
            const timespec quarter_second{0, 250'000'000};
            const int sig = sigtimedwait(&signals, nullptr, &quarter_second);
            if (sig == SIGINT || sig == SIGTERM || sig == SIGHUP) {
                logger.info("interrupted; withdrawing quotes");
                break;
            }
            elapsed_ms += 250;
            host.tick(cpm::oms::Clock::now());
        }
        if (halted.load()) host.halt("order entry halted");

        // Nothing is left resting on the way out. The host keeps ticking so
        // the cancels' reports are delivered, but a stopped strategy is never
        // driven again, so nothing re-quotes.
        host.stop_all("shutting down");
        cpm::oms::wait_for([&] { host.tick(cpm::oms::Clock::now()); return host.quiescent(); }, 3s);
        reconciler.stop();
        reconciler.save_bookmark();
        md.unsubscribe_all();
        md.stop();
        oe.stop();

        const auto info = host.info(id);
        const auto status = info ? info->status : cpm::strategy::StrategyStatus{};
        std::cout << "\nquotes placed " << status.placed << ", cancelled " << status.cancelled
                  << ", fills " << status.fills << '\n';
        if (status.placed == 0) {
            // A run that quoted nothing should say why on its way out.
            std::cout << status.passes << " pass(es) over the book, "
                      << status.passes_without_target << " with no quotable target\n";
            // Most specific first: the reason it could not quote matters more
            // than the reason it would not have sent.
            if (status.passes == 0) {
                std::cout << "no book ever arrived for " << symbol
                          << ": check the symbol is live (md_client --list-instruments)\n";
            } else if (!status.reason.empty()) {
                std::cout << "could not quote: " << status.reason << '\n';
            }
            if (!args.allow_orders) {
                std::cout << "and nothing would have been sent anyway: --allow-orders was not given\n";
            }
        }
        for (const auto& position : orders.positions()) {
            std::cout << position.symbol << "  net " << cpm::format_qty(position.net) << "  cash "
                      << cpm::format_amount(position.cash) << "  fees "
                      << cpm::format_amount(position.fees) << '\n';
        }
        return halted.load() ? 4 : 0;
    } catch (const cpm::ConfigError& e) {
        logger.error(std::string("configuration: ") + e.what());
        return 78;
    } catch (const std::exception& e) {
        logger.error(std::string("fatal: ") + e.what());
        return 1;
    }
}
