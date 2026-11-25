#include "cpm/trader/trader.hpp"

#include "fill_recovery.hpp"

#include <fcntl.h>
#include <unistd.h>

#include "cpm/oms/order_rules.hpp"
#include "cpm/oms/pnl.hpp"

#include <algorithm>
#include <utility>

namespace cpm::trader {
namespace {

using namespace std::chrono_literals;

// What the record files call an order that belongs to no strategy: the
// operator's own exit (G13). The column has to say something, and a blank
// one would read as "attribution lost" rather than "nobody's but yours".
constexpr const char* kOperatorId = "operator";

cpm::md::MdClientOptions md_options(const TraderOptions& options) {
    cpm::md::MdClientOptions out;
    out.config = options.md;
    out.dict_dir = options.dict_dir;
    return out;
}

cpm::oms::OeClientOptions oe_options(const TraderOptions& options) {
    cpm::oms::OeClientOptions out;
    out.config = options.oe;
    out.dict_dir = options.dict_dir;
    return out;
}

// How long a quote rested before it was hit, in seconds to the millisecond:
// "9.052s". Microseconds are what the record carries and what rest_us in
// fills.csv keeps, but a line being read by eye wants the magnitude, and the
// last three digits of nine seconds are not it.
std::string rested_text(std::chrono::microseconds rested) {
    return cpm::format_scaled(rested.count() / 1000, 3) + "s";
}

// The venue's round trip in whole milliseconds: "250ms". Microseconds are
// what the record carries and what latency_us in orders.csv keeps, but a line
// read by eye wants the magnitude, and a quarter of a second does not need
// six digits to say so. Empty when there is no number - never a zero, which
// would read as "answered instantly" rather than "not measured".
std::string took(const std::optional<std::chrono::microseconds>& value) {
    return value ? std::to_string(value->count() / 1000) + "ms" : std::string();
}

// One report as a line of a strategy's own log, with the level to write it at.
// Empty text for an event this does not narrate.
struct Narrated {
    std::string text;
    bool warn = false;
};

// An order's whole life in strategy.log, beside the reasoning that produced
// it: the send is already there from the reconciler ("quoting buy 1 @0.78 as
// mm-u1l"), and these are the venue's answers to it, which were not written
// down anywhere but the CSVs.
//
// Narrated from here rather than from QuoteReconciler::on_report, which sees
// the same reports, because this is where the latencies are measured and
// where the venue's OrderID is known. An "added" line without the ack latency
// is worth little: on 2026-11-19 at 07:01 a quote was sent at .621,
// abandoned by the strategy at .696 and only reached the book at .871, where
// it was taken 56ms later - and none of those 250ms was visible in this file.
// It is also the only place that sees the operator's own exit orders.
//
// One shape for the whole family - "<verb>: <side> <qty> @<price>, <detail>
// (<cl_ord_id>)" - so the lines read as one timeline and the ClOrdID is
// always in the same place.
//
// Not narrated: a CancelReject, which is not an order event and which the
// reconciler already judges and reports (OE-Q6); the absence of a "deleted"
// line is what says a cancel did not land. Replaced and PendingCancel are
// declared by the spec and never emitted.
Narrated narrate(const cpm::oms::AppliedReport& applied, const std::string& strategy) {
    if (applied.kind != cpm::oms::AppliedReport::Kind::Report) return {};
    const auto& report = applied.report;
    const auto& order = applied.order;

    // The venue's view of the order where it has one, ours where it does not:
    // a report need not repeat the size and the price it already knows.
    const Qty quantity = report.order_quantity ? *report.order_quantity : order.order_quantity;
    const Price price = report.price ? *report.price : order.price;
    // The order's own id, never the cancel's: a cancel's report names the
    // cancel in ClOrdID (OE-O2), and it is the order this line is about.
    const std::string& cl_ord_id =
        order.cl_ord_id.empty() ? report.cl_ord_id : order.cl_ord_id;

    const auto head = [&](const char* verb) {
        return cpm::strategy::log_tag(strategy, report.symbol) + verb + ": " +
               to_string(report.side) + " " + cpm::format_qty(quantity) + " @" +
               cpm::format_price(price);
    };
    const auto tail = [&](std::string line) {
        if (!cl_ord_id.empty()) line += " (" + cl_ord_id + ")";
        // A report the gateway sent again after a restart, not something that
        // just happened: the stamp on this line is this run's, not the event's.
        if (report.replayed) line += " replayed";
        return line;
    };

    switch (report.exec_type) {
        case cpm::oms::ExecType::New: {
            // Resting, and only now cancellable: a cancel cannot be sent for
            // an order the venue has not acknowledged, which is why the gap
            // between the send and this line is the window in which a quote
            // is live and unmanageable.
            std::string line = head("added");
            if (const auto latency = took(applied.latency); !latency.empty()) {
                line += ", acked in " + latency;
            }
            if (!report.order_id.empty()) line += ", oid " + report.order_id;
            return {tail(std::move(line)), false};
        }
        case cpm::oms::ExecType::PendingNew: {
            // Held in an instrument's speed bump: accepted, not matchable.
            std::string line = head("held") + " in the speed bump";
            if (const auto latency = took(applied.latency); !latency.empty()) {
                line += ", answered in " + latency;
            }
            return {tail(std::move(line)), false};
        }
        case cpm::oms::ExecType::Canceled: {
            std::string line = head("deleted");
            if (report.cancel_reason != cpm::oms::CancelReason::None) {
                // The venue's own doing - cancel-on-disconnect, a closing
                // window, self-match prevention - and not an answer to
                // anything we sent. Worth telling apart: one is the strategy
                // working, the other is the strategy being overruled.
                line += " by the venue: " + std::string(to_string(report.cancel_reason));
            } else {
                if (const auto latency = took(applied.latency); !latency.empty()) {
                    line += ", cancelled in " + latency;
                }
                // Which cancel did it, so the request logged earlier can be
                // joined to its answer.
                if (!report.orig_cl_ord_id.empty() && !report.cl_ord_id.empty()) {
                    line += " by " + report.cl_ord_id;
                }
            }
            return {tail(std::move(line)), false};
        }
        case cpm::oms::ExecType::Expired: {
            // The bell, or an IOC remainder. Not a problem and not ours.
            std::string line = head("expired");
            if (report.leaves_quantity > 0) {
                line += ", " + cpm::format_qty(report.leaves_quantity) + " unfilled";
            }
            return {tail(std::move(line)), false};
        }
        case cpm::oms::ExecType::Rejected: {
            // Warn, so it reaches the narrative as well. The reconciler
            // already decides whether a reject is forgivable or halts the
            // daemon; this is the same event in the strategy's own timeline,
            // where its absence would leave an order that was sent and then
            // simply stopped being mentioned.
            std::string line = head("rejected");
            if (report.reject_reason) line += ", reason " + std::to_string(*report.reject_reason);
            if (!report.text.empty()) line += ": " + report.text;
            return {tail(std::move(line)), true};
        }
        case cpm::oms::ExecType::Trade: break;
        default: return {};
    }

    // A fill: the same facts as a fills.csv row, in the order a fill is asked
    // about - what traded, at what, on which side of the book, what it cost,
    // how long the quote waited, and where it leaves the position. Not a
    // replacement for the row, which keeps the ids and the latencies this
    // leaves out, but an operator reading why a strategy did something should
    // not have to open a second file to find out whether it worked.
    const auto& fill = report.fill;
    std::string line = cpm::strategy::log_tag(strategy, report.symbol) + "fill: " +
                       to_string(report.side);
    if (fill) {
        line += " " + cpm::format_qty(fill->quantity) + " @" + cpm::format_price(fill->price) +
                (fill->aggressor ? " taker" : " maker");
        // Absent is not free: MiscFeeAmt(137) is simply not on every fill,
        // and a "0" here would say the venue charged nothing.
        if (fill->fee) line += ", fee " + cpm::format_amount(*fill->fee);
    }
    if (applied.rested) line += ", rested " + rested_text(*applied.rested);
    line += ", order " + cpm::format_qty(report.cumulative_quantity) + "/" +
            cpm::format_qty(order.order_quantity);
    if (report.leaves_quantity > 0) line += " left " + cpm::format_qty(report.leaves_quantity);
    if (applied.position) {
        // Signed: a short YES position is negative, and format_qty would
        // render it as a flat 0.
        line += ", net " + cpm::format_signed(applied.position->net, cpm::kQtyScale) + ", cash " +
                cpm::format_amount(applied.position->cash);
    }
    return {tail(std::move(line)), false};
}

}  // namespace

Trader::Trader(TraderOptions options, const cpm::strategy::StrategyRegistry& registry,
               Logger& logger)
    : options_(std::move(options)),
      logger_(logger),
      md_(md_options(options_), logger),
      oe_(oe_options(options_), logger),
      host_({}, registry,
            // The venue end of the hooks. can_submit here is the owner's
            // answer only; the host folds in each strategy's arming.
            [&]() {
                cpm::strategy::StrategyHooks hooks;
                hooks.can_submit = [this] {
                    return options_.allow_orders && reconciler_ && reconciler_->submission_allowed();
                };
                hooks.orders_permitted = [this] { return options_.allow_orders; };
                hooks.send = [this](const cpm::oms::NewOrder& order) {
                    orders_.record_sent(order);
                    return oe_.send(order);
                };
                hooks.cancel = [this](const cpm::oms::CancelOrder& request) {
                    orders_.record_cancel_sent(request);
                    return oe_.cancel(request);
                };
                hooks.order_finished = [this](const std::string& cl_ord_id) {
                    const auto record = orders_.order(cl_ord_id);
                    return record && record->terminal;
                };
                hooks.resting_orders = [this](const std::string& symbol) {
                    return resting_on(symbol);
                };
                // Who sent what, for the record files. Called by the host
                // just before the send it wraps.
                hooks.attribute = [this](const std::string& cl_ord_id,
                                         const std::string& strategy_id) {
                    std::lock_guard lock(attribution_mutex_);
                    attribution_[cl_ord_id] = strategy_id;
                };
                // No disk in here. The run tag makes the id unique across a
                // restart, so this is an atomic increment and a base36 format
                // on the tick thread, where it used to be two fsyncs.
                hooks.next_cl_ord_id = [this] {
                    return cpm::oms::make_cl_ord_id(mm_prefix_, ++cl_ord_counter_);
                };
                // A strategy talking about its own quoting: the whole of it
                // goes to the log file, and none of it to the operator's
                // panel, where thirty lines a second would hide the one
                // that matters (GUI-Q28).
                hooks.log = [this](const std::string& line) {
                    logger_.info(line, cpm::LogSource::Strategy);
                };
                return hooks;
            }(),
            [&]() {
                cpm::strategy::StrategyHost::Feeds feeds;
                feeds.book = [this](const std::string& symbol) { return book_.instrument(symbol); };
                feeds.position = [this](const std::string& symbol) {
                    return orders_.position(symbol).value_or(cpm::oms::Position{});
                };
                feeds.reference = [this](const std::string& symbol) {
                    return reference_for(symbol);
                };
                feeds.stale_book_reason = [this](const std::string& symbol, Clock::time_point now) {
                    const auto age = feed_->stale_book(symbol, now);
                    if (!age) return std::string{};
                    return "stale feed on " + symbol + " - no book update for " +
                           std::to_string(age->count()) + "s";
                };
                return feeds;
            }(),
            logger),
      run_tag_(options_.run_tag.empty() ? cpm::oms::run_tag() : options_.run_tag),
      mm_prefix_(cpm::oms::run_prefix("mm-", run_tag_)),
      ex_prefix_(cpm::oms::run_prefix("ex-", run_tag_)) {
    // --- market data --------------------------------------------------
    book_.on_anomaly([this](const cpm::md::BookAnomaly& anomaly) {
        logger_.warn(std::string("feed anomaly: ") + to_string(anomaly.kind) + " on " +
                     anomaly.symbol + (anomaly.detail.empty() ? "" : " (" + anomaly.detail + ")"));
    });

    cpm::md::FeedMonitor::Options feed_options;
    feed_options.stale_after = options_.stale_after;
    // The host stops strategies on stale books. Feed integrity failures and
    // disconnects still take the daemon-wide halt path.
    feed_options.halt_on_stale = false;
    feed_ = std::make_unique<cpm::md::FeedMonitor>(
        feed_options, [this](const cpm::md::FeedHalt& halt) {
            request_halt(std::string("market data: ") + to_string(halt.reason) +
                         (halt.symbol.empty() ? "" : " on " + halt.symbol) +
                         (halt.detail.empty() ? "" : " - " + halt.detail));
        });
    cpm::md::MdCallbacks to_series;
    to_series.lifecycle = [this](const cpm::md::InstrumentLifecycle& event) {
        // Only records. Which strategy moves where is decided on the tick
        // thread, in roll_series().
        series_.apply(event);
    };
    md_.on_market_data(
        cpm::md::combine(cpm::md::combine(book_.callbacks(), feed_->callbacks()), to_series));
    md_.on_session_event(feed_->session_callback());

    // --- order entry --------------------------------------------------
    // One line the operator can grep a whole run by: every ClOrdID this
    // daemon sends carries this tag, whoever built it.
    logger_.info("ClOrdID run tag " + run_tag_ + ": ids look like " + mm_prefix_ + "1");
    cpm::oms::Reconciler::Options reconcile;
    reconcile.run_tag = run_tag_;
    // The one reject the daemon forgives, and only because the instrument's
    // own name says the window had ended before the order reached it.
    reconcile.window_has_ended = [](const std::string& symbol) {
        const auto parsed = cpm::md::parse_ticker(symbol);
        return parsed && Clock::now() >= parsed->window_end;
    };
    reconciler_ = std::make_unique<cpm::oms::Reconciler>(
        reconcile, oe_, orders_, cpm::oms::BookmarkFile(options_.bookmark),
        logger, [this](const cpm::oms::Halt& halt) {
            request_halt(std::string("order entry: ") + to_string(halt.reason) + " - " + halt.detail);
        });
    orders_.on_anomaly(reconciler_->anomaly_handler());
    orders_.on_applied([this](const cpm::oms::AppliedReport& applied) { record(applied); });
    oe_.on_session_event(reconciler_->session_callback());

    cpm::oms::OeCallbacks to_strategies;
    to_strategies.report = [this](const cpm::oms::ExecutionReport& report) {
        // Only queues: the host fans it out on the next tick, so this QuickFIX
        // callback never runs strategy code.
        host_.on_report(report);
    };
    oe_.on_events(cpm::oms::combine_oe(
        cpm::oms::combine_oe(orders_.callbacks(), reconciler_->callbacks()), to_strategies));

    auto limits = options_.limits;
    limits.allow_orders = options_.allow_orders;
    oe_.set_limits(limits);

    // --- the operator's surface ---------------------------------------
    ControlApi::Env env;
    env.market_data = [this] {
        return SessionView{md_.logged_on(), md_.last_logout_reason()};
    };
    env.order_entry = [this] {
        return SessionView{oe_.logged_on(), oe_.last_logout_reason()};
    };
    env.recovery = [this] {
        return reconciler_ ? std::string(to_string(reconciler_->state())) : std::string("unknown");
    };
    env.reference = [this]() -> std::string {
        if (!reference_) return "off";
        if (!reference_->connected()) return "down";
        // What it is actually carrying, not what was asked for on the
        // command line: streams come and go with the strategies (B-Q14),
        // and an idle connection with nothing subscribed is up and fine.
        const auto now = cpm::binance::Clock::now();
        for (const auto& symbol : reference_->subscribed()) {
            if (!reference_->latest(symbol, now).usable()) return "stale";
        }
        return "up";
    };
    env.recover_fills = [this](const std::string& symbol) { return recover_cycle_fills(symbol); };
    env.fills = [this] {
        auto fills = orders_.fills();
        const auto now = Clock::now();
        std::erase_if(fills, [now](const auto& fill) {
            const auto market = cpm::md::parse_ticker(fill.symbol);
            return market && now >= market->window_end;
        });
        std::stable_sort(fills.begin(), fills.end(), [](const auto& left, const auto& right) {
            return left.transact_time > right.transact_time;
        });
        return fills;
    };
    env.positions = [this] {
        std::vector<PositionView> out;
        for (const auto& position : orders_.positions()) {
            PositionView view;
            view.symbol = position.symbol;
            view.net = position.net;
            view.bought = position.bought;
            view.sold = position.sold;
            view.cash = position.cash;
            view.bought_value = position.bought_value;
            view.sold_value = position.sold_value;
            view.fees = position.fees;
            view.fills = position.fills;
            view.settlement_payout = position.settlement_payout;
            out.push_back(std::move(view));
        }
        return out;
    };
    env.pnl = [this] {
        std::vector<PnlView> out;
        for (const auto& marked : marked_positions()) {
            PnlView view;
            view.symbol = marked.position.symbol;
            view.mark = marked.mark;
            view.realized = marked.pnl.realized;
            view.unrealized = marked.pnl.unrealized;
            view.total = marked.pnl.total;
            out.push_back(std::move(view));
        }
        return out;
    };
    env.book = [this](const std::string& symbol) -> std::optional<BookView> {
        return view_of(symbol);
    };
    env.resolve = [this](const std::string& series) { return resolve_series(series); };
    env.current_window = [this](const std::string& series) -> std::optional<std::string> {
        if (const auto window = series_.current(series, Clock::now())) return window->symbol;
        return std::nullopt;
    };
    env.axis = [this](const std::string& symbol) {
        AxisView view;
        view.symbol = symbol;
        view.structure = cpm::md::kPriceLevelStructure;
        view.prices = cpm::md::price_grid();
        return view;
    };
    env.limits = [this] {
        LimitsView view;
        view.allow_orders = options_.allow_orders;
        view.max_quantity = options_.limits.max_quantity;
        view.max_notional = options_.limits.max_notional;
        view.max_orders_per_sec = options_.limits.max_orders_per_sec;
        return view;
    };
    env.subscribe = [this](const std::string& symbol) { return subscribe(symbol); };
    env.unsubscribe = [this](const std::string& symbol) { unsubscribe(symbol); };
    env.watched = [this] {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> out;
        out.reserve(watching_.size());
        for (const auto& [series, symbol] : watching_) out.push_back(symbol);
        return out;
    };
    // The operator's own order path (G13). It is the strategies' path with
    // the strategies taken out: the same gate, the same counter, the same
    // record of what was sent, and no arming, because arming is a standing
    // permission to quote and this is one order the operator asked for.
    env.can_submit = [this] {
        return options_.allow_orders && reconciler_ && reconciler_->submission_allowed();
    };
    env.resting_orders = [this](const std::string& symbol) { return resting_on(symbol); };
    env.next_cl_ord_id = [this] {
        // "ex-" rather than "mm-": the log, the CSVs and the wire should all
        // say that this one was not a quote. The counter is shared with the
        // strategies', so an operator's exit never collides with a quote.
        return cpm::oms::make_cl_ord_id(ex_prefix_, ++cl_ord_counter_);
    };
    env.send_order = [this](const cpm::oms::NewOrder& order) -> std::string {
        // Attributed and recorded before it goes, because the ack can reach
        // a QuickFIX thread before send() has returned here.
        {
            std::lock_guard lock(attribution_mutex_);
            attribution_[order.cl_ord_id] = kOperatorId;
        }
        orders_.record_sent(order);
        try {
            oe_.send(order);
        } catch (const std::exception& e) {
            // The client refused it - a limit, or a rule the gateway would
            // enforce on receipt. Nothing went to the wire, so this is a
            // refusal to show the operator and not a halt.
            return e.what();
        }
        logger_.warn(std::string("exit ") + order.symbol + ": " + to_string(order.side) + " " +
                     cpm::format_qty(order.quantity) + " @" + cpm::format_price(order.price) +
                     " IOC as " + order.cl_ord_id);
        return {};
    };
    api_ = std::make_unique<ControlApi>(host_, registry, std::move(env), options_.allow_orders);
}

Trader::~Trader() {
    stop();
    if (reference_) reference_->stop();
    if (wake_read_ >= 0) ::close(wake_read_);
    if (wake_write_ >= 0) ::close(wake_write_);
}

int Trader::wake_fd() const { return wake_read_; }

void Trader::drain_wake() {
    if (wake_read_ < 0) return;
    // A flag, not a queue: however many updates arrived, this pass sees the
    // newest price and that is the whole of it (B-Q5).
    char scratch[256];
    while (::read(wake_read_, scratch, sizeof(scratch)) > 0) {
    }
}

void Trader::follow_reference_demand() {
    if (!reference_) return;
    // The reference venue carries exactly the streams the running
    // strategies need, and nothing else: start a strategy on BTCD15M and
    // BTCUSDT is subscribed on the one connection; stop it and the stream
    // goes (B-Q14). Stopped, stopping and halted strategies do not count -
    // none of them is about to quote.
    //
    // Declared rather than counted: the set is recomputed from the host
    // each pass, so there is no reference count to get wrong and a
    // strategy that vanishes however it vanishes takes its stream with it.
    std::vector<std::string> wanted;
    // The watched series first: their streams are up for the life of the
    // daemon, so the guard has a price the instant a strategy is armed
    // rather than one pass later.
    std::map<std::string, std::string> watched;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        watched = watching_;
    }
    for (const auto& [series, symbol] : watched) {
        if (const auto mapped = reference_symbol_for(symbol); !mapped.empty()) {
            wanted.push_back(mapped);
        }
    }
    for (const auto& strategy : host_.list()) {
        if (strategy.state != cpm::strategy::RunState::Running) continue;
        for (const auto& symbol : strategy.symbols) {
            if (const auto mapped = reference_symbol_for(symbol); !mapped.empty()) {
                wanted.push_back(mapped);
            }
        }
    }
    reference_->want(wanted);
}

std::string Trader::reference_symbol_for(const std::string& symbol) const {
    // The asset if the ticker parses - BTCD15M-BTC26OCT290430-0 is BTC -
    // and otherwise the series, which is the first field of any instrument
    // name on this venue. The second is what makes a simulator symbol
    // mappable at all: SIMGAME-26NOV01HOMAWY-HOM has no asset to read.
    std::vector<std::string> keys;
    if (const auto parsed = cpm::md::parse_ticker(symbol)) keys.push_back(parsed->asset);
    if (const auto dash = symbol.find('-'); dash != std::string::npos) {
        keys.push_back(symbol.substr(0, dash));
    }
    for (const auto& key : keys) {
        const auto mapped = options_.reference_symbols.find(key);
        if (mapped != options_.reference_symbols.end()) return mapped->second.symbol;
    }
    return {};
}

cpm::binance::ReferenceView Trader::reference_for(const std::string& symbol) const {
    if (!reference_) return {};
    const auto mapped = reference_symbol_for(symbol);
    if (mapped.empty()) return {};
    return reference_->latest(mapped, cpm::binance::Clock::now());
}

std::vector<cpm::strategy::LiveLeg> Trader::resting_on(const std::string& symbol) const {
    // From the order manager rather than any strategy's own two slots:
    // anything of ours left on this instrument counts, including what an
    // earlier state left behind.
    std::vector<cpm::strategy::LiveLeg> out;
    for (const auto& order : orders_.open_orders()) {
        if (order.symbol != symbol || order.leaves_quantity <= 0) continue;
        cpm::strategy::LiveLeg leg;
        leg.side = order.side;
        leg.cl_ord_id = order.cl_ord_id;
        // Where it actually rests, which a post-only slide moves away from
        // what we asked for.
        leg.price = order.price;
        leg.quantity = order.leaves_quantity;
        leg.cancel_pending = order.cancel_pending;
        out.push_back(std::move(leg));
    }
    return out;
}

BookView Trader::view_of(const std::string& symbol) const {
    BookView view;
    view.symbol = symbol;
    const auto instrument = book_.instrument(symbol);
    if (!instrument) return view;
    view.valid = instrument->book_valid;
    view.status = to_string(instrument->status);
    view.crossed_or_locked = instrument->crossed_or_locked;
    view.volume = instrument->volume;
    view.open_interest = instrument->open_interest;
    if (instrument->last_trade) view.last_trade = instrument->last_trade->yes_price;
    if (!instrument->yes.empty()) {
        view.best_yes_bid = instrument->yes.front().price;
        view.best_yes_bid_qty = instrument->yes.front().qty;
    }
    if (!instrument->no.empty()) {
        // Both sides of this venue are bids (Q9), so the YES ask is implied.
        view.implied_yes_ask = kPriceOne - instrument->no.front().price;
        view.best_no_bid_qty = instrument->no.front().qty;
    }
    // The full ladder, both sides on one price axis. The YES stack is already
    // best first; the NO stack is too, and subtracting each from one turns
    // "the best NO bid" into "the cheapest YES ask" without reordering it.
    view.bids.reserve(instrument->yes.size());
    for (const auto& level : instrument->yes) {
        view.bids.push_back(LadderLevel{level.price, level.qty, level.price});
    }
    view.asks.reserve(instrument->no.size());
    for (const auto& level : instrument->no) {
        view.asks.push_back(LadderLevel{kPriceOne - level.price, level.qty, level.price});
    }
    // Only quotable books have a mid, which is the same rule the strategy
    // applies: one side missing and there is nothing to quote around.
    if (view.best_yes_bid && view.implied_yes_ask && *view.implied_yes_ask > *view.best_yes_bid) {
        view.mid = (*view.best_yes_bid + *view.implied_yes_ask) / 2;
        view.spread = *view.implied_yes_ask - *view.best_yes_bid;
    }
    return view;
}

std::string Trader::subscribe(const std::string& symbol) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (subscriptions_.contains(symbol)) return {};
    if (!md_.logged_on()) {
        return "the market data session is not logged on, so " + symbol +
               " would never get a book";
    }
    std::vector<std::string> ids;
    try {
        // The book, and the lifecycle so a closed window withdraws the quotes.
        const cpm::md::ChannelRequest book{cpm::md::Channel::Book, {symbol}, options_.book_depth};
        const cpm::md::ChannelRequest lifecycle{cpm::md::Channel::Lifecycle, {symbol}, 0};
        ids.push_back(md_.subscribe(book));
        feed_->track(ids.back(), book);

        ids.push_back(md_.subscribe(lifecycle));
    } catch (const std::exception& e) {
        for (const auto& id : ids) {
            if (feed_) feed_->forget(id);
            md_.unsubscribe(id);
        }
        return std::string("could not subscribe to ") + symbol + ": " + e.what();
    }
    subscriptions_[symbol] = Subscription{std::move(ids)};

    return {};
}

void Trader::unsubscribe(const std::string& symbol) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = subscriptions_.find(symbol);
    if (it == subscriptions_.end()) return;
    // A watched symbol keeps its book: the daemon holds this subscription for
    // its own sake, and dropping it because a strategy let go would put the
    // page and every position's price back at the mercy of a row.
    const bool watched = std::any_of(watching_.begin(), watching_.end(),
                                     [&](const auto& entry) { return entry.second == symbol; });
    if (watched) return;
    for (const auto& id : it->second.ids) {
        if (feed_) feed_->forget(id);
        md_.unsubscribe(id);
    }
    subscriptions_.erase(it);
    // The book goes with the subscription that built it. Leaving it bound to
    // an MDReqID nobody is subscribed with means the next subscription's
    // snapshot is ignored as a second subscriber's (O7) and so is every
    // update after it: the book then stays frozen at this instant while
    // looking live, and everything that reads it - the page, the mark, a
    // strategy's quoting, an exit's price - is reading a market that has
    // stopped existing (GUI-N5).
    book_.invalidate_book(symbol);
}

std::string Trader::strategy_of(const std::string& cl_ord_id) const {
    std::lock_guard lock(attribution_mutex_);
    const auto it = attribution_.find(cl_ord_id);
    return it == attribution_.end() ? std::string() : it->second;
}

std::vector<Trader::MarkedPosition> Trader::marked_positions() const {
    std::vector<MarkedPosition> out;
    for (const auto& position : orders_.positions()) {
        // The mid of the instrument's own book, which is the only honest
        // mark here - and it is absent exactly when the book is one-sided,
        // invalid, or gone because the window has closed and the instrument
        // has been evicted. pnl_of() leaves the unrealized half unset in
        // that case rather than calling it zero.
        const auto mark = view_of(position.symbol).mid;
        out.push_back(MarkedPosition{position, mark, cpm::oms::pnl_of(position, mark)});
    }
    return out;
}

void Trader::record_pnl(Clock::time_point now) {
    if (!records_ || now - last_pnl_ < options_.pnl_every) return;
    last_pnl_ = now;
    for (const auto& marked : marked_positions()) {
        records_->pnl_row(marked.position, marked.mark, marked.pnl);
    }
}

void Trader::record_quotes() {
    if (!records_ || !records_->quotes_enabled()) return;
    std::map<std::string, std::string> seen;
    for (const auto& strategy : host_.list()) {
        cpm::oms::RecordLog::Quote quote;
        quote.strategy_id = strategy.id;
        quote.symbol = strategy.symbols.empty() ? strategy.target : strategy.symbols.front();
        quote.state = to_string(strategy.state);
        quote.armed = strategy.armed;
        quote.reason = strategy.status.reason.empty() ? strategy.note : strategy.status.reason;
        for (const auto& leg : strategy.status.legs) {
            // What is actually resting, which a post-only slide moves away
            // from what the strategy asked for.
            if (leg.side == cpm::oms::Side::Buy) {
                quote.bid = leg.price;
                quote.bid_qty = leg.quantity;
            } else {
                quote.ask = leg.price;
                quote.ask_qty = leg.quantity;
            }
        }
        const auto book = view_of(quote.symbol);
        quote.mid = book.mid;
        if (const auto reference = reference_for(quote.symbol); reference.usable()) {
            quote.reference_mid = cpm::format_scaled(reference.mid, cpm::binance::kScale);
        }

        // The decision, and not the context around it: the book's mid and
        // the reference move constantly, and a row every time either ticked
        // would be a row every pass saying the same thing about our quotes.
        std::string decision = quote.state + (quote.armed ? "|armed|" : "|disarmed|") +
                               (quote.bid ? cpm::format_price(*quote.bid) : "-") + "|" +
                               (quote.bid_qty ? cpm::format_qty(*quote.bid_qty) : "-") + "|" +
                               (quote.ask ? cpm::format_price(*quote.ask) : "-") + "|" +
                               (quote.ask_qty ? cpm::format_qty(*quote.ask_qty) : "-") + "|" +
                               quote.symbol + "|" + quote.reason;
        const auto last = last_quote_.find(strategy.id);
        if (last == last_quote_.end() || last->second != decision) records_->quote_row(quote);
        seen.emplace(strategy.id, std::move(decision));
    }
    // A strategy that has been reaped takes its last decision with it, so
    // the map is the size of what is running rather than of the day.
    last_quote_.swap(seen);
}

void Trader::record(const cpm::oms::AppliedReport& applied) {
    const auto& order_id = applied.kind == cpm::oms::AppliedReport::Kind::CancelReject
                               ? applied.reject.orig_cl_ord_id
                               : applied.order.cl_ord_id;
    // The order's own id, not the cancel's: a cancel's report names the
    // cancel in ClOrdID (OE-O2), and it is the order that belongs to a
    // strategy.
    std::string strategy = strategy_of(order_id);
    if (strategy.empty() && applied.kind == cpm::oms::AppliedReport::Kind::CancelReject) {
        strategy = strategy_of(applied.reject.cl_ord_id);
    }

    // The order's life in strategy.log, beside the reasoning that produced
    // it. Logged whether or not the record files were opened - this is the
    // narrative half, and a run writing to stdout only still has one - and
    // from here rather than from the strategies, because this is where a
    // report is attributed, where the latencies are measured and where the
    // position a fill left behind is known. A strategy is told about its own
    // reports on the next tick and could log them itself, but then every
    // kind would have to remember to, each would say it differently, none
    // would have the latencies, and the operator's own exit (G13) would say
    // nothing at all.
    //
    // LogSource::Strategy, so it files beside the commentary it belongs
    // with and stays off the console with the rest of it; --console-strategy
    // puts the whole source back. A Warn is never silenced, so a reject is
    // on screen and in the narrative whatever this says.
    if (const auto narrated = narrate(applied, strategy); !narrated.text.empty()) {
        logger_.log(narrated.warn ? cpm::LogLevel::Warn : cpm::LogLevel::Info, narrated.text,
                    cpm::LogSource::Strategy);
    }

    if (records_) records_->write(applied, strategy);

    // Forget what is finished with, so the map stays the size of what is
    // live rather than of the day. Whether or not anything was written: the
    // map is filled on every send now, so it has to be drained on every
    // report, not only on the ones a record file saw.
    std::lock_guard lock(attribution_mutex_);
    if (applied.kind == cpm::oms::AppliedReport::Kind::Report) {
        if (applied.order.terminal) attribution_.erase(applied.order.cl_ord_id);
        // A cancel is answered once, by this report.
        if (!applied.report.orig_cl_ord_id.empty()) attribution_.erase(applied.report.cl_ord_id);
    } else {
        attribution_.erase(applied.reject.cl_ord_id);
    }
}

std::string Trader::start(std::chrono::milliseconds timeout) {
    // Before the sessions: a report that arrives in the first millisecond
    // belongs in the records like any other.
    if (!options_.log_dir.empty()) {
        records_ = std::make_unique<cpm::oms::RecordLog>(options_.log_dir, options_.quote_records,
                                                         options_.log_run);
        if (const auto failed = records_->open_now(cpm::utc_stamp()); !failed.empty()) {
            logger_.warn("no record files: " + failed);
            records_.reset();
        } else {
            logger_.info("records " + records_->fills_path() + ", " + records_->orders_path() +
                         " and " + records_->pnl_path() +
                         (records_->quotes_enabled() ? ", quotes as well" : ""));
        }
    }

    // The reference feed first, so a strategy created a second later has a
    // price rather than a gap. It is optional: no symbols, no feed, and the
    // guard is simply off for every strategy (B-Q4 is about a feed that was
    // configured and went away, which is a different thing).
    if (options_.reference_enabled) {
        int pipes[2] = {-1, -1};
        if (::pipe(pipes) != 0) return "could not create the reference feed's wake pipe";
        wake_read_ = pipes[0];
        wake_write_ = pipes[1];
        // Non-blocking on both ends: the reader must never stall the feed's
        // thread on a full pipe, and draining must never block the loop.
        ::fcntl(wake_read_, F_SETFL, O_NONBLOCK);
        ::fcntl(wake_write_, F_SETFL, O_NONBLOCK);

        auto reference_options = options_.reference;
        if (reference_options.history_file.empty() && !options_.bookmark.empty()) {
            reference_options.history_file = options_.bookmark + ".reference-history";
        }
        reference_ = std::make_unique<cpm::binance::ReferenceFeed>(std::move(reference_options), logger_);
        const int fd = wake_write_;
        reference_->on_update([fd] {
            // Everything this thread is allowed to do to the daemon. One
            // byte, non-blocking, and a full pipe means a wake is already
            // pending - which is the same answer.
            const char one = 'r';
            [[maybe_unused]] const auto wrote = ::write(fd, &one, 1);
        });
        if (const auto failed = reference_->start(); !failed.empty()) {
            return "the reference feed could not start: " + failed;
        }
    }

    reconciler_->start();
    oe_.start();
    md_.start();
    feed_->start();

    if (!md_.wait_logged_on(timeout)) {
        return "market data logon failed: " + md_.last_logout_reason();
    }
    // Without --allow-orders the daemon is an observer, so an order entry
    // session it cannot use is not worth failing over.
    const auto oe_timeout = options_.allow_orders ? timeout : 3s;
    if (!oe_.wait_logged_on(oe_timeout)) {
        const auto reason = oe_.last_logout_reason();
        if (options_.allow_orders) {
            return "order entry logon failed: " + reason;
        }
        logger_.warn("order entry is not available (" + (reason.empty() ? "no logon" : reason) +
                     "); strategies can run but nothing could be armed anyway");
    }
    // One all-instrument lifecycle subscription (269=L, 146=0) for the whole
    // run: it announces every window of every series as it is Listed and
    // Opened, which is how a rolling strategy learns where to go next
    // ([MD] O6). Per-symbol lifecycle subscriptions are redundant beside it
    // and only duplicate messages (R-Q8), but they stay for now because the
    // book subscription path opens them together.
    try {
        const cpm::md::ChannelRequest all_lifecycle{cpm::md::Channel::Lifecycle, {}, 0};
        series_md_req_id_ = md_.subscribe(all_lifecycle);
        // Deliberately not tracked by FeedMonitor: a lifecycle feed is silent
        // between rolls by nature, and the monitor only watches books anyway.
        logger_.info("watching every instrument's lifecycle for series rolls");
    } catch (const std::exception& e) {
        // Not fatal: without it a series cannot roll, but a daemon quoting
        // fixed tickers is unaffected.
        logger_.warn(std::string("could not watch the lifecycle feed, so no strategy can roll: ") +
                     e.what());
    }

    // The watched series, before anything is created: the page has a market
    // to show the moment it opens, and a position has a price whatever
    // happens to the row that opened it. Not fatal - a series that cannot be
    // resolved yet is picked up by follow_watch_list() at the next roll.
    for (const auto& wanted : options_.watch) {
        // A ticker is watched as it is; a series is resolved to the window
        // trading now, the same choice create() offers an operator.
        std::string symbol = wanted;
        if (!cpm::md::looks_like_instrument(wanted)) {
            const auto found = resolve_series(wanted);
            if (!found.error.empty()) {
                logger_.warn("not watching " + wanted + ": " + found.error);
                continue;
            }
            symbol = found.symbol;
        }
        if (const auto error = subscribe(symbol); !error.empty()) {
            logger_.warn("not watching " + wanted + ": " + error);
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            watching_[wanted] = symbol;
        }
        logger_.info("watching " + wanted + (symbol == wanted ? "" : " on " + symbol));
    }

    last_evict_ = Clock::now();
    return {};
}

void Trader::request_halt(std::string reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (halt_requested_) return;  // the first reason is the real one
    halt_requested_ = true;
    pending_halt_ = std::move(reason);
}

void Trader::apply_halt(Clock::time_point now) {
    std::string reason;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!halt_requested_ || halt_applied_) return;
        halt_applied_ = true;
        halt_applied_at_ = now;
        reason = pending_halt_;
    }
    // Withdraws every strategy and refuses every start and arm from here on.
    host_.halt(reason);
    // The feed is what was lost or is no longer trusted, so it goes first.
    // The order session stays up until the cancels are accounted for.
    feed_->stop();
    md_.stop();
}

void Trader::tick(Clock::time_point now) {
    apply_halt(now);
    follow_reference_demand();
    host_.tick(now);

    bool finish_halt = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (halt_applied_ && !sessions_stopped_) {
            // Give the cancels a moment to come back, then stop regardless:
            // a venue that is not answering is the reason we are halting.
            finish_halt = host_.quiescent() || now - halt_applied_at_ > 5s;
            if (finish_halt) sessions_stopped_ = true;
        }
    }
    if (finish_halt) {
        reconciler_->stop();
        reconciler_->save_bookmark();
        oe_.stop();
        logger_.error("halted: both sessions are down and nothing can restart. "
                      "The daemon stays up so the reason can be read; restart it to trade again.");
        return;
    }

    follow_watch_list(now);
    roll_series(now);
    watch_for_an_orphaned_book();
    // After the strategies have run and before the evictions: what is
    // written is what this pass left behind.
    record_quotes();
    record_pnl(now);

    if (now - last_evict_ >= options_.evict_every) {
        last_evict_ = now;
        if (const auto dropped = book_.evict_finished(options_.evict_finished_after, now);
            dropped > 0) {
            logger_.debug("evicted " + std::to_string(dropped) + " finished instrument(s)");
        }
        if (const auto forgotten = series_.evict(now); forgotten > 0) {
            logger_.debug("forgot " + std::to_string(forgotten) + " finished window(s)");
        }
    }
}

std::string Trader::recover_cycle_fills(const std::string& symbol) {
    if (!options_.allow_orders || options_.oe.endpoint.host != cpm::kOeSandboxHost) {
        return {};
    }
    std::lock_guard lock(fill_recovery_mutex_);
    if (recovered_markets_.contains(symbol)) return {};

    const auto failure = [&](const std::string& reason) {
        return "fill recovery for " + symbol + ": " + reason;
    };
    if (!reconciler_->submission_allowed()) return failure("wait for FIX recovery to finish");
    if (const auto window = cpm::md::parse_ticker(symbol);
        window && Clock::now() >= window->window_end) {
        return failure("the market window has ended");
    }
    const auto fetch = options_.rest_fetch ? options_.rest_fetch : cpm::md::HttpsFetch(cpm::https_get);
    const auto recovered = fetch_fill_history(options_.rest, fetch, symbol, options_.oe.subaccount);
    if (!recovered.error.empty()) return failure(recovered.error);
    if (!reconciler_->submission_allowed()) return failure("FIX recovery changed during the request");
    if (const auto window = cpm::md::parse_ticker(symbol);
        window && Clock::now() >= window->window_end) {
        return failure("the market window ended during the request");
    }
    if (const auto error = orders_.recover_fills(symbol, recovered.fills, recovered.net);
        !error.empty()) {
        return failure(error);
    }
    recovered_markets_.insert(symbol);
    logger_.info("recovered " + std::to_string(recovered.fills.size()) + " fills for " + symbol);
    return {};
}

ControlApi::Env::Resolved Trader::resolve_series(const std::string& series) {
        ControlApi::Env::Resolved out;
        // The feed first: if this daemon has already seen the series roll, it
        // knows the answer without asking anyone.
        if (const auto known = series_.current(series, Clock::now())) {
            out.symbol = known->symbol;
            return out;
        }
        if (!options_.rest.configured()) {
            out.error = series +
                        " cannot be resolved: REST is not configured (the CPM_REST_* set in "
                        "config/creds.env), and the "
                        "lifecycle feed has not seen this series roll yet - it announces the "
                        "next window, not the present one. Source config/creds.env and restart "
                        "the daemon, or name a full ticker";
            return out;
        }
        cpm::md::HttpsFetch fetch = options_.rest_fetch;
        if (!fetch) fetch = cpm::https_get;
        const auto found = cpm::md::open_instruments(options_.rest, series, fetch);
        if (!found.ok()) {
            out.error = found.error;
            return out;
        }
        if (found.instruments.empty()) {
            out.error = "no window of " + series +
                        " is open right now; the next one opens within a second of the last one "
                        "closing";
            return out;
        }
        const auto& instrument = found.instruments.front();
        logger_.info("resolved " + series + " to " + instrument.ticker + " (ends " +
                     cpm::md::to_utc_string(instrument.expected_close) + ")");
        // This build prices on one grid, the venue's TAPERED_DECI_CENT. An
        // instrument the catalog says trades on another one cannot be quoted
        // here at all: every price would be off its grid, and an order at a
        // price the venue does not have is a reject, which halts the daemon
        // (OE-Q6). So it is said loudly rather than discovered that way.
        if (!instrument.price_level_structure.empty() &&
            instrument.price_level_structure != cpm::md::kPriceLevelStructure) {
            logger_.warn("the catalog reports " + instrument.ticker + " on " +
                         instrument.price_level_structure + ", and this build prices only on " +
                         cpm::md::kPriceLevelStructure + ": do not quote it");
        }
        out.symbol = instrument.ticker;
        return out;
}


void Trader::follow_watch_list(Clock::time_point now) {
    if (options_.watch.empty()) return;
    for (const auto& series : options_.watch) {
        if (cpm::md::looks_like_instrument(series)) continue;  // a ticker never rolls
        const auto window = series_.current(series, now);
        if (!window) continue;  // between windows, or the feed has not said yet
        std::string previous;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto held = watching_.find(series);
            if (held != watching_.end()) previous = held->second;
        }
        if (previous == window->symbol) continue;

        // The new window before the old one goes, so there is no instant
        // with nothing watched.
        if (const auto error = subscribe(window->symbol); !error.empty()) {
            logger_.warn("could not watch " + window->symbol + ": " + error);
            continue;
        }
        {
            // The GUI's watched-book snapshot and unsubscribe() read this map
            // under the same lock while the tick thread advances windows.
            std::lock_guard<std::mutex> lock(mutex_);
            watching_[series] = window->symbol;
        }
        logger_.info("watching " + series + " on " + window->symbol +
                     (previous.empty() ? "" : " (was " + previous + ")"));
        if (previous.empty() || previous == window->symbol) continue;
        // The window that just closed: let it go unless a strategy is still
        // on it, in which case it is that strategy's to drop.
        // list() returns a vector by value. Both iterators must belong to
        // this same snapshot, not to two separate temporary vectors.
        const auto strategies = host_.list();
        const bool claimed = std::any_of(
            strategies.begin(), strategies.end(), [&](const cpm::strategy::StrategyInfo& row) {
                return std::find(row.symbols.begin(), row.symbols.end(), previous) !=
                       row.symbols.end();
            });
        if (!claimed) unsubscribe(previous);
    }
}

void Trader::watch_for_an_orphaned_book() {
    // MarketState ignores a snapshot or an update whose MDReqID is not the
    // one that bound the book, because two deliberate subscriptions on one
    // instrument duplicate every message (O7). This daemon never makes that
    // overlap - subscribe() is a no-op for a symbol it already holds - so
    // anything counted here means a book is bound to a subscription that no
    // longer exists. Such a book is frozen at the moment its feed went and
    // still reads as live, which is worth saying out loud exactly once: it
    // silently cost a live run its prices on 2026-11-04.
    const auto ignored = book_.ignored_foreign_snapshots() + book_.ignored_foreign_updates();
    if (ignored <= orphan_warned_at_) return;
    orphan_warned_at_ = ignored;
    if (orphan_warned_) return;
    orphan_warned_ = true;
    logger_.error("a book is bound to a subscription this daemon no longer holds, so it has "
                  "stopped being rebuilt and is showing a market that has moved on. Nothing it "
                  "prices can be trusted: restart the daemon");
}

void Trader::roll_series(Clock::time_point now) {
    if (host_.halted()) return;
    for (const auto& strategy : host_.list()) {
        if (strategy.target.empty()) continue;  // pinned to one instrument
        // A row whose feed is stopped is watching nothing, and rolling it
        // would subscribe the feed the operator has just stopped. It picks
        // up whatever window is open when Start Feed is pressed.
        if (!strategy.feed) continue;
        const auto window = series_.current(strategy.target, now);
        if (!window) continue;  // between windows, or the series has gone quiet
        if (!strategy.symbols.empty() && strategy.symbols.front() == window->symbol) continue;

        const auto rolled = api_->roll(strategy.id, window->symbol);
        if (!rolled.ok()) {
            // Not a halt: the strategy stays on a window that has closed,
            // which quotes nothing, and the next tick tries again.
            logger_.warn("could not roll " + strategy.id + " onto " + window->symbol + ": " +
                         rolled.error);
        }
    }
}

void Trader::stop() {
    if (stopped_) return;
    stopped_ = true;

    host_.stop_all("shutting down");
    // Keep ticking so the cancels' reports are delivered; a stopped strategy
    // is never driven again, so nothing re-quotes on the way out.
    cpm::oms::wait_for([this] { host_.tick(Clock::now()); return host_.quiescent(); }, 3s);

    feed_->stop();
    reconciler_->stop();
    reconciler_->save_bookmark();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [symbol, subscription] : subscriptions_) {
            (void)symbol;
            for (const auto& id : subscription.ids) md_.unsubscribe(id);
        }
        subscriptions_.clear();
    }
    md_.stop();
    oe_.stop();
}

}  // namespace cpm::trader
