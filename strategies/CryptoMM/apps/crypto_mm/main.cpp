// crypto_mm: quotes a bid and an ask a fixed number of ticks either side of the
// mid, each for a fixed quantity, on one instrument.
//
// It is the first binary that uses both services: the book comes from the
// market data session and the orders go out on the order entry session, each
// with its own credentials and its own endpoint guard.
//
//   source creds.env && source creds-oe.env && \
//     ./crypto_mm --symbol BTCD15M-... --width-ticks 2 --qty 1 --allow-orders
//
// Without --allow-orders it runs as a dry run: it computes and logs the quotes
// it would place and sends nothing.
#include "cpm/common/config.hpp"
#include "cpm/common/logging.hpp"
#include "cpm/md/market_state.hpp"
#include "cpm/md/md_client.hpp"
#include "cpm/oms/oe_client.hpp"
#include "cpm/oms/order_rules.hpp"
#include "cpm/oms/order_state.hpp"
#include "cpm/oms/reconciler.hpp"
#include "cpm/strategy/crypto_mm.hpp"

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
    std::string symbol;
    int width_ticks = 2;
    cpm::Qty quantity = 100;      // 1.00 contract
    cpm::Qty max_inventory = 500; // 5.00 contracts
    int requote_threshold = 1;
    cpm::strategy::TickStructure ticks = cpm::strategy::TickStructure::TaperedDeciCent;
    int duration_secs = 60;
    bool allow_orders = false;
    bool verbose = false;
    std::string dict_dir = CPM_DEFAULT_DICT_DIR;
    std::string bookmark = "crypto-mm-bookmark.txt";
};

void usage() {
    std::cerr << "usage: crypto_mm --symbol SYMBOL [--width-ticks N] [--qty Q]\n"
                 "                 [--max-inventory Q] [--requote-threshold N]\n"
                 "                 [--ticks tapered|cent|deci] [--duration SECS]\n"
                 "                 [--allow-orders] [--bookmark PATH] [--verbose]\n"
                 "env: the market data set (CPM_*) and the order entry set (CPM_OE_*)\n"
                 "Without --allow-orders nothing is sent: it logs the quotes it would place.\n";
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
    if (!parsed) {
        throw std::invalid_argument(std::string(flag) + " needs a decimal with at most " +
                                    std::to_string(scale) + " places");
    }
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
        if (flag == "--symbol") a.symbol = value();
        else if (flag == "--width-ticks") a.width_ticks = to_int(flag, value());
        else if (flag == "--qty") a.quantity = to_decimal(flag, value(), cpm::kQtyScale);
        else if (flag == "--max-inventory") a.max_inventory = to_decimal(flag, value(), cpm::kQtyScale);
        else if (flag == "--requote-threshold") a.requote_threshold = to_int(flag, value());
        else if (flag == "--duration") a.duration_secs = to_int(flag, value());
        else if (flag == "--allow-orders") a.allow_orders = true;
        else if (flag == "--bookmark") a.bookmark = value();
        else if (flag == "--dict-dir") a.dict_dir = value();
        else if (flag == "--verbose") a.verbose = true;
        else if (flag == "--ticks") {
            const std::string_view which = value();
            if (which == "tapered") a.ticks = cpm::strategy::TickStructure::TaperedDeciCent;
            else if (which == "cent") a.ticks = cpm::strategy::TickStructure::LinearCent;
            else if (which == "deci") a.ticks = cpm::strategy::TickStructure::DeciCent;
            else throw std::invalid_argument("--ticks must be tapered, cent or deci");
        }
        else if (flag == "--help" || flag == "-h") { usage(); std::exit(0); }
        else throw std::invalid_argument("unknown option " + std::string(flag));
    }
    if (a.symbol.empty()) throw std::invalid_argument("--symbol is required");
    if (a.width_ticks < 1) throw std::invalid_argument("--width-ticks must be at least 1");
    if (a.quantity <= 0) throw std::invalid_argument("--qty must be positive");
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

    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
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

        // --- the strategy -------------------------------------------------
        cpm::strategy::Params params;
        params.symbol = args.symbol;
        params.half_width_ticks = args.width_ticks;
        params.quantity = args.quantity;
        params.max_inventory = args.max_inventory;
        params.requote_threshold_ticks = args.requote_threshold;
        params.ticks = args.ticks;

        std::atomic<std::uint64_t> counter{reconciler.cl_ord_counter()};
        cpm::strategy::QuoteManager::Hooks hooks;
        hooks.can_submit = [&] { return args.allow_orders && reconciler.submission_allowed(); };
        hooks.send = [&](const cpm::oms::NewOrder& order) {
            orders.record_sent(order);
            return oe.send(order);
        };
        hooks.cancel = [&](const cpm::oms::CancelOrder& request) {
            orders.record_cancel_sent(request);
            return oe.cancel(request);
        };
        hooks.next_cl_ord_id = [&] {
            const auto next = ++counter;
            reconciler.note_cl_ord_counter(next);
            return cpm::oms::make_cl_ord_id("mm-", next);
        };
        hooks.log = [&logger](const std::string& line) { logger.info(line); };
        cpm::strategy::QuoteManager manager(params, std::move(hooks));

        cpm::oms::OeCallbacks strategy_callbacks;
        strategy_callbacks.report = [&](const cpm::oms::ExecutionReport& report) {
            manager.on_report(report);
        };
        oe.on_events(cpm::oms::combine_oe(
            cpm::oms::combine_oe(orders.callbacks(), reconciler.callbacks()), strategy_callbacks));
        cpm::oms::OrderLimits limits;
        limits.allow_orders = args.allow_orders;
        limits.max_quantity = args.quantity * 2;
        // Two sides, a few re-quotes: a few orders a second is plenty.
        limits.max_orders_per_sec = 4;
        limits.max_notional = args.quantity * cpm::kPriceOne * 10;
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
        md.subscribe({cpm::md::Channel::Book, {args.symbol}, 10});
        md.subscribe({cpm::md::Channel::Lifecycle, {args.symbol}, 0});

        int elapsed_ms = 0;
        bool withdrawn = false;
        while ((args.duration_secs == 0 || elapsed_ms < args.duration_secs * 1000) && !halted.load()) {
            const timespec quarter_second{0, 250'000'000};
            const int sig = sigtimedwait(&signals, nullptr, &quarter_second);
            if (sig == SIGINT || sig == SIGTERM) {
                logger.info("interrupted; withdrawing quotes");
                break;
            }
            elapsed_ms += 250;

            const auto view = book.instrument(args.symbol);
            if (!view) {
                // A symbol with no book at all is almost always the wrong
                // symbol, or a window that has already closed (O2/O6).
                if (elapsed_ms % 5000 == 0) {
                    logger.warn("no book for " + args.symbol + " yet after " +
                                std::to_string(elapsed_ms / 1000) + "s: is the symbol live? " +
                                "md_client --list-instruments names the live ones");
                }
                continue;
            }
            // An instrument that has stopped trading is not quotable, and its
            // 15-minute window closes without warning (O2/O6).
            const bool tradable = view->status == cpm::md::LifecycleStatus::Open ||
                                  view->status == cpm::md::LifecycleStatus::Unknown;
            if (!tradable) {
                if (!withdrawn) {
                    manager.withdraw(std::string("instrument is ") + to_string(view->status));
                    withdrawn = true;
                }
                continue;
            }
            withdrawn = false;
            const auto position = orders.position(args.symbol).value_or(cpm::oms::Position{});
            manager.on_market(*view, position, cpm::oms::Clock::now());
        }

        // Nothing is left resting on the way out.
        manager.withdraw("shutting down");
        cpm::oms::wait_for([&] { return manager.live_bid() == std::nullopt &&
                                        manager.live_ask() == std::nullopt; }, 3s);
        reconciler.stop();
        reconciler.save_bookmark();
        md.unsubscribe_all();
        md.stop();
        oe.stop();

        std::cout << "\nquotes placed " << manager.quotes_placed() << ", cancelled "
                  << manager.quotes_cancelled() << ", fills " << manager.fills() << '\n';
        if (manager.quotes_placed() == 0) {
            // A run that quoted nothing should say why on its way out.
            std::cout << manager.passes() << " pass(es) over the book, "
                      << manager.passes_without_quotes() << " with no quotable target\n";
            // Most specific first: the reason it could not quote matters more
            // than the reason it would not have sent.
            if (manager.passes() == 0) {
                std::cout << "no book ever arrived for " << args.symbol
                          << ": check the symbol is live (md_client --list-instruments)\n";
            } else if (!manager.last_reason().empty()) {
                std::cout << "could not quote: " << manager.last_reason() << '\n';
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
