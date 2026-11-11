#include "cpm/strategy/quote_reconciler.hpp"

#include <algorithm>
#include <utility>

namespace cpm::strategy {
using cpm::oms::Side;
using cpm::md::tick_at;

QuoteReconciler::QuoteReconciler(QuoteSettings settings, Hooks hooks)
    : settings_(std::move(settings)), hooks_(std::move(hooks)) {}

void QuoteReconciler::update(const DesiredQuotes& desired) {
    ++passes_;
    last_reason_ = desired.reason;
    if (!desired.bid && !desired.ask) ++passes_without_quotes_;
    if (desired.stop) {
        if (hooks_.stop_me) hooks_.stop_me(desired.reason);
        else withdraw(desired.reason);
        return;
    }
    if (hooks_.can_submit && !hooks_.can_submit()) return;
    reconcile_side(Side::Buy, desired.bid, bid_, settings_.symbol);
    reconcile_side(Side::Sell, desired.ask, ask_, settings_.symbol);
}

StrategyStatus QuoteReconciler::status() const {
    StrategyStatus out;
    out.legs = legs();
    out.reason = last_reason_;
    out.passes = passes_;
    out.passes_without_target = passes_without_quotes_;
    out.placed = placed_;
    out.cancelled = cancelled_;
    out.fills = fills_;
    return out;
}

bool QuoteReconciler::worth_replacing(const LiveQuote& live, const Quote& desired) const {
    if (live.quantity != desired.quantity) return true;
    // Offering the exact target the venue just slid would only be slid again,
    // so that one price is not retried. Any other target is, which is what
    // keeps the quotes following the mid.
    if (live.slid && desired.price == live.requested) return false;

    // Otherwise: how far is the target from where the quote actually rests?
    // Measuring against the resting price is what makes the quotes track the
    // mid rather than the intention behind them.
    const Price tick = tick_at(live.price, settings_.ticks);
    const Price moved = live.price > desired.price ? live.price - desired.price
                                                   : desired.price - live.price;
    // A flicker of less than the threshold is left alone, so the strategy does
    // not cancel and replace on every update.
    return moved >= tick * std::max(1, settings_.requote_threshold_ticks);
}

void QuoteReconciler::reconcile_side(Side side, const std::optional<Quote>& desired,
                                  std::optional<LiveQuote>& live, const std::string& symbol) {
    const auto log = [&](const std::string& message) {
        if (hooks_.log) hooks_.log(message);
    };

    // A quote that has already finished at the venue is not ours to cancel.
    // The fill that ended it updates the position on the session's thread,
    // while this quote is only cleared when the report reaches the next tick
    // - so for the width of that gap the strategy prices against a fill it
    // has not yet applied, and would cancel the order that caused it.
    // UNKNOWN_ORDER comes back, and a cancel reject halts the run.
    if (live && hooks_.order_finished && hooks_.order_finished(live->cl_ord_id)) {
        log("the " + std::string(to_string(side)) + " quote " + live->cl_ord_id +
            " has already finished at the venue; dropping it rather than cancelling it");
        live.reset();
        // And on, with no live quote: the desired side is placed afresh below,
        // which is what the report would have led to a moment later anyway.
    }

    if (!desired) {
        if (live && !live->cancel_pending) {
            CancelOrder request;
            request.cl_ord_id = hooks_.next_cl_ord_id();
            request.orig_cl_ord_id = live->cl_ord_id;
            request.symbol = symbol;
            request.side = side;
            if (hooks_.cancel(request)) {
                live->cancel_pending = true;
                ++cancelled_;
                log("withdrawing the " + std::string(to_string(side)) + " quote " + live->cl_ord_id);
            }
        }
        return;
    }

    if (live) {
        // A cancel already out: wait for its report rather than stacking
        // another, which the gateway answers with ORDER_ALREADY_PENDING_CANCEL.
        if (live->cancel_pending) return;
        if (!worth_replacing(*live, *desired)) return;
        CancelOrder request;
        request.cl_ord_id = hooks_.next_cl_ord_id();
        request.orig_cl_ord_id = live->cl_ord_id;
        request.symbol = symbol;
        request.side = side;
        if (hooks_.cancel(request)) {
            live->cancel_pending = true;
            ++cancelled_;
            log("re-quoting " + std::string(to_string(side)) + ": " + format_price(live->requested) +
                " -> " + format_price(desired->price) +
                (live->price == live->requested
                     ? ""
                     : " (resting at " + format_price(live->price) + " after a slide)"));
        }
        // The replacement goes out once the cancel is confirmed: the venue has
        // no amend, and a ClOrdID must be unique among live orders.
        return;
    }

    NewOrder order;
    order.cl_ord_id = hooks_.next_cl_ord_id();
    order.symbol = symbol;
    order.side = side;
    order.quantity = desired->quantity;
    order.type = cpm::oms::OrderType::Limit;
    order.price = desired->price;
    order.time_in_force = cpm::oms::TimeInForce::Gtc;
    // Post-only slide, not reject: a quote that would cross is adjusted rather
    // than refused, and a refusal would halt the run (OE-Q6).
    order.post_only = cpm::oms::PostOnly::Slide;
    try {
        const auto id = hooks_.send(order);
        live = LiveQuote{id, desired->price, desired->price, desired->quantity, false, false, false};
        ++placed_;
        log("quoting " + std::string(to_string(side)) + " " + format_qty(desired->quantity) + " @" +
            format_price(desired->price) + " as " + id);
    } catch (const std::exception& e) {
        // A refusal before the wire - a limit, the submission gate, a rule -
        // leaves the side unquoted and is tried again on the next update.
        log(std::string("could not quote ") + to_string(side) + ": " + e.what());
    }
}


void QuoteReconciler::on_report(const ExecutionReport& report) {
    // A report belongs to a quote by its ClOrdID, or by the OrigClOrdID a
    // cancel's report carries (OE-O2).
    const auto matches = [&](const std::optional<LiveQuote>& quote) {
        if (!quote) return false;
        return quote->cl_ord_id == report.cl_ord_id || quote->cl_ord_id == report.orig_cl_ord_id;
    };
    std::optional<LiveQuote>* quote = nullptr;
    if (matches(bid_)) {
        quote = &bid_;
    } else if (matches(ask_)) {
        quote = &ask_;
    } else {
        return;
    }

    switch (report.exec_type) {
        case cpm::oms::ExecType::New:
        case cpm::oms::ExecType::PendingNew:
            (*quote)->acknowledged = true;
            // A slide moves the price, so the live price is the report's - and
            // the fact that it was slid is remembered.
            if (report.price) {
                (*quote)->price = *report.price;
                (*quote)->slid = *report.price != (*quote)->requested;
            }
            break;
        case cpm::oms::ExecType::Trade:
            ++fills_;
            (*quote)->quantity = report.leaves_quantity;
            // Fully filled: the quote is gone and will be replaced on the next
            // update, at whatever the mid is by then.
            if (report.leaves_quantity == 0) quote->reset();
            break;
        case cpm::oms::ExecType::Canceled:
        case cpm::oms::ExecType::Expired:
        case cpm::oms::ExecType::Rejected:
            quote->reset();
            break;
        default: break;
    }
}

void QuoteReconciler::withdraw(const std::string& why) {
    if (hooks_.log) hooks_.log("withdrawing both quotes: " + why);
    for (auto* quote : {&bid_, &ask_}) {
        if (!*quote || (*quote)->cancel_pending) continue;
        // The same guard reconcile_side keeps: an order the venue has already
        // finished is not ours to cancel. A halt withdraws right after its
        // own mass cancel, which is exactly when this is true.
        if (hooks_.order_finished && hooks_.order_finished((*quote)->cl_ord_id)) {
            if (hooks_.log) {
                hooks_.log("the quote " + (*quote)->cl_ord_id +
                           " has already finished at the venue; dropping it rather than "
                           "cancelling it");
            }
            quote->reset();
            continue;
        }
        CancelOrder request;
        request.cl_ord_id = hooks_.next_cl_ord_id();
        request.orig_cl_ord_id = (*quote)->cl_ord_id;
        request.symbol = settings_.symbol;
        request.side = quote == &bid_ ? Side::Buy : Side::Sell;
        if (hooks_.cancel(request)) {
            (*quote)->cancel_pending = true;
            ++cancelled_;
        }
    }
}

void QuoteReconciler::abandon(const std::string& why) {
    // No cancels. The window has closed and the venue has already terminated
    // whatever was on it; a cancel sent now would be answered with
    // UNKNOWN_ORDER, and a cancel reject halts the daemon (OE-Q6).
    if (!bid_ && !ask_) return;
    if (hooks_.log) hooks_.log("abandoning both quotes to the venue's own cancel: " + why);
    bid_.reset();
    ask_.reset();
}

std::optional<Quote> QuoteReconciler::live_bid() const {
    if (!bid_) return std::nullopt;
    return Quote{bid_->price, bid_->quantity};
}

std::optional<Quote> QuoteReconciler::live_ask() const {
    if (!ask_) return std::nullopt;
    return Quote{ask_->price, ask_->quantity};
}

std::vector<LiveLeg> QuoteReconciler::legs() const {
    std::vector<LiveLeg> out;
    const auto add = [&out](const std::optional<LiveQuote>& quote, Side side) {
        if (!quote) return;
        LiveLeg leg;
        leg.side = side;
        leg.cl_ord_id = quote->cl_ord_id;
        // Where it actually rests, which is not where it was asked for if the
        // venue slid it.
        leg.price = quote->price;
        leg.quantity = quote->quantity;
        leg.cancel_pending = quote->cancel_pending;
        out.push_back(std::move(leg));
    };
    add(bid_, Side::Buy);
    add(ask_, Side::Sell);
    return out;
}


}  // namespace cpm::strategy
