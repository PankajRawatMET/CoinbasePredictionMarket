// oe_client: log on to the CPM FIX order entry gateway, keep the session alive
// with heartbeats, and log out cleanly on Ctrl+C / SIGTERM / --duration.
//
// By default it sends NO orders: it logs on, heartbeats and logs out, and every
// inbound message is decoded and printed as one "ev " line beside the raw log.
//
// --test-order places ONE limit order and cancels it again, which is how the
// real ExecutionReport layouts get captured. It needs --allow-orders as well,
// so no accident can open a position. --mass-cancel cancels everything the API
// key has, and needs no --allow-orders: stopping must always be possible.
//
// Credentials and endpoint come from the environment (see creds-oe.env):
//   source creds-oe.env && ./oe_client [--duration SECS] [--verbose]
#include "cpm/common/config.hpp"
#include "cpm/common/logging.hpp"
#include "cpm/oms/oe_client.hpp"
#include "cpm/oms/order_rules.hpp"
#include "cpm/oms/order_state.hpp"
#include "cpm/oms/reconciler.hpp"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <string>
#include <string_view>

namespace {

struct Args {
    int duration_secs = 30;  // 0 = until signal
    int logon_timeout_secs = 20;
    std::string dict_dir = CPM_DEFAULT_DICT_DIR;
    bool verbose = false;

    bool allow_orders = false;
    bool mass_cancel = false;
    std::string test_order_symbol;
    cpm::oms::Side side = cpm::oms::Side::Buy;
    cpm::Qty quantity = 1;   // 0.01 contract, the minimum increment
    cpm::Price price = 0;    // required with --test-order
    int cancel_after_secs = 5;
    cpm::Qty max_quantity = 100;        // 1.00 contract
    cpm::Amount max_notional = 1000000; // 1.00 USDC
    std::string bookmark = "oe-bookmark.txt";
};

void usage() {
    std::cerr << "usage: oe_client [--duration SECS] [--logon-timeout SECS] [--dict-dir DIR]\n"
                 "                 [--verbose] [--allow-orders]\n"
                 "                 [--test-order SYMBOL --price P [--qty Q] [--side buy|sell]\n"
                 "                  [--cancel-after SECS]] [--mass-cancel]\n"
                 "                 [--max-qty Q] [--max-notional USDC] [--bookmark PATH]\n"
                 "env: CPM_OE_API_KEY CPM_OE_PASSPHRASE CPM_OE_SECRET CPM_OE_HOST CPM_OE_PORT\n"
                 "     [CPM_OE_TLS=1] [CPM_OE_CA_FILE] [CPM_OE_HEARTBEAT=30]\n"
                 "     [CPM_OE_CANCEL_ON_DISCONNECT=A] [CPM_OE_SETTLEMENT_REPORTS=Y]\n"
                 "     [CPM_OE_MANUAL_ORDERS=N] [CPM_OE_SUBACCOUNT]\n"
                 "Places no orders unless --test-order and --allow-orders are both given.\n";
}

int to_int(std::string_view flag, const char* value) {
    char* end = nullptr;
    const long v = std::strtol(value, &end, 10);
    if (*value == '\0' || *end != '\0' || v < 0 || v > 86400) {
        throw std::invalid_argument(std::string(flag) + " needs an integer in 0..86400");
    }
    return static_cast<int>(v);
}

// Decimal command-line values go through the same strict parser as the wire,
// so nothing reaches an order through a double.
std::int64_t to_decimal(std::string_view flag, const char* value, int scale) {
    const auto parsed = cpm::parse_scaled(value, scale);
    if (!parsed) {
        throw std::invalid_argument(std::string(flag) + " needs a decimal with at most " +
                                    std::to_string(scale) + " places, got '" + value + "'");
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
        if (flag == "--duration") a.duration_secs = to_int(flag, value());
        else if (flag == "--logon-timeout") a.logon_timeout_secs = to_int(flag, value());
        else if (flag == "--dict-dir") a.dict_dir = value();
        else if (flag == "--verbose") a.verbose = true;
        else if (flag == "--allow-orders") a.allow_orders = true;
        else if (flag == "--mass-cancel") a.mass_cancel = true;
        else if (flag == "--test-order") a.test_order_symbol = value();
        else if (flag == "--cancel-after") a.cancel_after_secs = to_int(flag, value());
        else if (flag == "--side") {
            const std::string_view v = value();
            if (v == "buy") a.side = cpm::oms::Side::Buy;
            else if (v == "sell") a.side = cpm::oms::Side::Sell;
            else throw std::invalid_argument("--side must be buy or sell");
        }
        else if (flag == "--qty") a.quantity = to_decimal(flag, value(), cpm::kQtyScale);
        else if (flag == "--price") a.price = to_decimal(flag, value(), cpm::kPriceScale);
        else if (flag == "--max-qty") a.max_quantity = to_decimal(flag, value(), cpm::kQtyScale);
        else if (flag == "--max-notional") a.max_notional = to_decimal(flag, value(), cpm::kAmtScale);
        else if (flag == "--bookmark") a.bookmark = value();
        else if (flag == "--help" || flag == "-h") { usage(); std::exit(0); }
        else throw std::invalid_argument("unknown option " + std::string(flag));
    }
    if (!a.test_order_symbol.empty() && a.price == 0) {
        throw std::invalid_argument("--test-order needs --price (a resting price, away from the touch)");
    }
    if (!a.test_order_symbol.empty() && !a.allow_orders) {
        throw std::invalid_argument("--test-order also needs --allow-orders");
    }
    return a;
}

// One line per decoded event, through the Logger so it interleaves safely with
// the raw log.
cpm::oms::OeCallbacks event_printer(cpm::Logger& logger) {
    using namespace cpm::oms;
    cpm::oms::OeCallbacks callbacks;
    callbacks.report = [&logger](const ExecutionReport& r) {
        std::string line = std::string("ev report    ") + to_string(r.exec_type) + "/" +
                           to_string(r.status) + " " + r.cl_ord_id + " order=" + r.order_id +
                           " exec=" + r.exec_id + " " + r.symbol + " " + to_string(r.side) +
                           " leaves=" + cpm::format_qty(r.leaves_quantity) +
                           " cum=" + cpm::format_qty(r.cumulative_quantity);
        if (r.price) line += " px=" + cpm::format_price(*r.price);
        if (r.average_price) line += " avg=" + cpm::format_price(*r.average_price);
        if (r.fill) {
            line += " fill=" + cpm::format_qty(r.fill->quantity) + "@" + cpm::format_price(r.fill->price) +
                    (r.fill->aggressor ? " taker" : " maker");
            if (r.fill->fee) line += " fee=" + cpm::format_amount(*r.fill->fee);
            if (!r.fill->trade_match_id.empty()) line += " trade=" + r.fill->trade_match_id;
        }
        if (r.reject_reason) line += " rej=" + std::to_string(*r.reject_reason);
        if (r.cancel_reason != CancelReason::None) line += std::string(" why=") + to_string(r.cancel_reason);
        if (!r.text.empty()) line += " text=" + r.text;
        if (r.replayed) line += " (replayed)";
        logger.info(line);
    };
    callbacks.cancel_reject = [&logger](const CancelReject& e) {
        logger.warn("ev cancel-rej " + e.cl_ord_id + " orig=" + e.orig_cl_ord_id + " order=" + e.order_id +
                    " reason=" + (e.reason ? std::to_string(*e.reason) : "-") + " " + e.text);
    };
    callbacks.mass_cancel_report = [&logger](const MassCancelReport& e) {
        logger.info(std::string("ev mass-cancel ") + (e.accepted ? "accepted" : "rejected") + " " +
                    e.cl_ord_id + " report=" + e.report_id + " affected=" +
                    (e.affected_orders ? std::to_string(*e.affected_orders) : "-") +
                    (e.text.empty() ? "" : " " + e.text));
    };
    callbacks.settlement = [&logger](const SettlementReport& e) {
        logger.info("ev settlement " + e.symbol + " price=" + cpm::format_price(e.settlement_price) +
                    " long=" + cpm::format_qty(e.long_quantity) +
                    " short=" + cpm::format_qty(e.short_quantity) +
                    " payout=" + cpm::format_amount(e.payout));
    };
    callbacks.last_exec_id = [&logger](const LastExecId& e) {
        logger.info("ev last-exec-id " + e.exec_id + " (ref " + e.ref_seq_num + ")");
    };
    callbacks.resend_complete = [&logger](const ResendComplete& e) {
        logger.info("ev resend-complete " + std::to_string(e.event_count) + " event(s)");
    };
    callbacks.resend_reject = [&logger](const ResendReject& e) {
        logger.warn("ev resend-reject reason=" + std::to_string(e.reason) + " " + e.text);
    };
    return callbacks;
}

// What the session built: every order, and the positions the fills imply.
void print_state(const cpm::oms::OrderState& state, cpm::Logger& logger) {
    const auto orders = state.orders();
    const auto positions = state.positions();
    if (orders.empty() && positions.empty()) return;

    std::cout << "\n" << orders.size() << " order(s)\n";
    for (const auto& order : orders) {
        std::cout << "  " << order.cl_ord_id << "  " << order.symbol << "  "
                  << to_string(order.side) << " " << cpm::format_qty(order.order_quantity) << " @"
                  << cpm::format_price(order.price) << "  " << to_string(order.status)
                  << (order.terminal ? " (final)" : "") << "  leaves "
                  << cpm::format_qty(order.leaves_quantity) << "  filled "
                  << cpm::format_qty(order.cumulative_quantity);
        if (order.cumulative_quantity > 0) {
            std::cout << " @" << cpm::format_price(order.average_price) << "  fees "
                      << cpm::format_amount(order.fees);
        }
        if (!order.text.empty()) std::cout << "  " << order.text;
        std::cout << '\n';
    }
    if (positions.empty()) return;
    std::cout << "\n" << positions.size() << " position(s), derived from fills\n";
    for (const auto& position : positions) {
        std::cout << "  " << position.symbol << "  net " << cpm::format_qty(position.net)
                  << "  bought " << cpm::format_qty(position.bought) << "  sold "
                  << cpm::format_qty(position.sold) << "  cash " << cpm::format_amount(position.cash)
                  << "  fees " << cpm::format_amount(position.fees);
        if (position.settlement_payout) {
            std::cout << "  settled at " << cpm::format_price(*position.settlement_price) << " payout "
                      << cpm::format_amount(*position.settlement_payout);
        }
        std::cout << '\n';
    }
    logger.info("order state: " + std::to_string(state.open_orders().size()) + " still open, " +
                std::to_string(state.anomalies()) + " anomaly(ies), " +
                std::to_string(state.duplicate_reports()) + " replayed report(s)");
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    try {
        args = parse_args(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "oe_client: " << e.what() << '\n';
        usage();
        return 64;
    }

    // Block the signals before QuickFIX starts its threads, so they inherit the
    // mask and the main thread collects them synchronously.
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);

    cpm::Logger logger(std::cout, args.verbose ? cpm::LogLevel::Debug : cpm::LogLevel::Info);

    try {
        cpm::oms::OeClientOptions options;
        options.config = cpm::load_oe_config(cpm::process_env());
        options.dict_dir = args.dict_dir;
        if (options.config.subaccount.empty()) {
            logger.warn("CPM_OE_SUBACCOUNT is not set: this session can log on, but an order "
                        "needs Parties/PartyRole=24 (see docs/open-questions-oms.txt OE-Q2)");
        }

        const auto subaccount = options.config.subaccount;
        cpm::oms::OeClient client(std::move(options), logger);

        // Stages 4-6: every report builds order and position state, and the
        // reconciler decides when submission is allowed and when to stop.
        cpm::oms::OrderState state;
        std::atomic<bool> halted{false};
        cpm::oms::Reconciler reconciler({}, client, state, cpm::oms::BookmarkFile(args.bookmark),
                                        logger, [&](const cpm::oms::Halt& halt) {
                                            logger.error(std::string("run halted: ") +
                                                         to_string(halt.reason) + " - " + halt.detail);
                                            halted.store(true);
                                        });
        state.on_anomaly(reconciler.anomaly_handler());
        client.on_session_event(reconciler.session_callback());
        client.on_events(cpm::oms::combine_oe(
            event_printer(logger), cpm::oms::combine_oe(state.callbacks(), reconciler.callbacks())));
        client.set_submission_gate([&reconciler] { return reconciler.submission_allowed(); });
        reconciler.start();
        cpm::oms::OrderLimits limits;
        limits.allow_orders = args.allow_orders;
        limits.max_quantity = args.max_quantity;
        limits.max_notional = args.max_notional;
        limits.max_orders_per_sec = 2;
        client.set_limits(limits);
        client.start();

        if (!client.wait_logged_on(std::chrono::seconds(args.logon_timeout_secs))) {
            const auto reason = client.last_logout_reason();
            logger.error("logon failed within " + std::to_string(args.logon_timeout_secs) + "s" +
                         (reason.empty() ? "" : ": " + reason));
            client.stop();
            return 2;
        }

        // Every id this tool sends carries the run tag, for the same reason
        // the daemon's do: two runs must not mint the same ClOrdID
        // (cpm::oms::run_tag).
        const auto run = cpm::oms::run_tag();

        // The kill switch: cancel everything this API key has, then stop.
        if (args.mass_cancel) {
            cpm::oms::MassCancel request;
            request.cl_ord_id = cpm::oms::make_cl_ord_id(cpm::oms::run_prefix("kill-", run), 1);
            request.scope = cpm::oms::MassCancel::Scope::All;
            if (!client.mass_cancel(request)) {
                logger.error("mass cancel could not be sent");
                client.stop();
                return 3;
            }
        }

        std::string test_order_id;
        if (!args.test_order_symbol.empty()) {
            if (subaccount.empty()) {
                logger.error("--test-order needs CPM_OE_SUBACCOUNT: Parties/PartyRole=24 is required "
                             "on every order, and the first order creates the sub-account");
                client.stop();
                return 78;
            }
            // Wait for reconciliation: nothing may be submitted before it.
            if (!cpm::oms::wait_for([&] { return reconciler.submission_allowed() || halted.load(); },
                                    std::chrono::seconds(15))) {
                logger.error("reconciliation did not finish; not sending anything");
                reconciler.stop();
                client.stop();
                return 4;
            }
            if (halted.load()) {
                reconciler.stop();
                client.stop();
                return 4;
            }
            cpm::oms::NewOrder order;
            order.cl_ord_id = cpm::oms::make_cl_ord_id(cpm::oms::run_prefix("t-", run), 1);
            order.symbol = args.test_order_symbol;
            order.side = args.side;
            order.quantity = args.quantity;
            order.type = cpm::oms::OrderType::Limit;
            order.price = args.price;
            order.time_in_force = cpm::oms::TimeInForce::Gtc;
            // Post-only: if the price turns out to be aggressive the order is
            // refused rather than filled, which is what a capture run wants.
            order.post_only = cpm::oms::PostOnly::Reject;
            try {
                state.record_sent(order);
                test_order_id = client.send(order);
            } catch (const std::exception& e) {
                logger.error(std::string("order refused before sending: ") + e.what());
                client.stop();
                return 3;
            }
        }

        int elapsed = 0;
        while ((args.duration_secs == 0 || elapsed < args.duration_secs) && client.logged_on() &&
               !halted.load()) {
            const timespec one_second{1, 0};
            const int sig = sigtimedwait(&signals, nullptr, &one_second);
            if (sig == SIGINT || sig == SIGTERM) {
                logger.info(std::string("received ") + (sig == SIGINT ? "SIGINT" : "SIGTERM") +
                            ", logging out");
                break;
            }
            ++elapsed;
            // Cancel the test order again, so nothing is left resting.
            if (!test_order_id.empty() && elapsed == args.cancel_after_secs) {
                cpm::oms::CancelOrder request;
                request.cl_ord_id = cpm::oms::make_cl_ord_id(cpm::oms::run_prefix("c-", run),
                                                             static_cast<std::uint64_t>(elapsed));
                request.orig_cl_ord_id = test_order_id;
                request.symbol = args.test_order_symbol;
                request.side = args.side;
                state.record_cancel_sent(request);
                if (!client.cancel(request)) logger.error("cancel could not be sent");
            }
        }

        reconciler.stop();
        reconciler.save_bookmark();
        const bool replaced = client.replaced();
        client.stop();
        logger.info("stopped; last logout reason: " + client.last_logout_reason());
        print_state(state, logger);
        if (halted.load()) return 4;
        return replaced ? 5 : 0;
    } catch (const cpm::ConfigError& e) {
        logger.error(std::string("configuration: ") + e.what());
        return 78;
    } catch (const std::exception& e) {
        logger.error(std::string("fatal: ") + e.what());
        return 1;
    }
}
