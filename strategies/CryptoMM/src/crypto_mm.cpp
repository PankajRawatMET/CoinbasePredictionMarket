#include "cpm/strategy/crypto_mm.hpp"

#include <algorithm>

namespace cpm::strategy {
namespace {

using cpm::oms::Side;

constexpr Price kMinPrice = 1;              // 0.001
constexpr Price kMaxPrice = kPriceOne - 1;  // 0.999

// Rounds down to the tick grid for a bid and up for an ask, so rounding never
// moves a quote towards the other side of the book.
Price round_to_tick(Price price, Price tick, bool downwards) {
    if (tick <= 1) return price;
    const Price remainder = price % tick;
    if (remainder == 0) return price;
    return downwards ? price - remainder : price + (tick - remainder);
}

}  // namespace

Price tick_at(Price price, TickStructure structure) {
    switch (structure) {
        case TickStructure::LinearCent: return 10;  // 0.01
        case TickStructure::DeciCent: return 1;     // 0.001
        case TickStructure::TaperedDeciCent:
            // 0.001 ticks below 0.100 and above 0.900, 0.01 between, with
            // 0.100 and 0.900 themselves on the coarse grid (O5).
            return (price < 100 || price > 900) ? 1 : 10;
    }
    return 1;
}

DesiredQuotes desired_quotes(const InstrumentView& book, const Position& position,
                            const Params& params) {
    DesiredQuotes out;

    // The mid needs both sides: the best YES bid, and the YES ask implied by
    // the best NO bid. Either missing and there is no mid to quote around.
    if (!book.book_valid) {
        out.reason = "no book";
        return out;
    }
    if (book.yes.empty() || book.no.empty()) {
        out.reason = "one-sided book";
        return out;
    }
    const Price best_bid = book.yes.front().price;
    const Price best_ask = kPriceOne - book.no.front().price;
    if (best_ask <= best_bid) {
        // Crossed or locked: the book is not quotable and Stage 4 has already
        // flagged it.
        out.reason = "crossed or locked book";
        return out;
    }
    const Price mid = (best_bid + best_ask) / 2;

    const Price tick = tick_at(mid, params.ticks);
    const Price offset = tick * params.half_width_ticks;

    Price bid = round_to_tick(mid - offset, tick_at(mid - offset, params.ticks), true);
    Price ask = round_to_tick(mid + offset, tick_at(mid + offset, params.ticks), false);
    bid = std::clamp(bid, kMinPrice, kMaxPrice);
    ask = std::clamp(ask, kMinPrice, kMaxPrice);

    // Never quote through the resting book: a post-only order that would cross
    // is either slid or rejected, and a reject would halt the run (OE-Q6).
    if (bid >= best_ask) bid = best_ask - tick_at(best_ask, params.ticks);
    if (ask <= best_bid) ask = best_bid + tick_at(best_bid, params.ticks);
    if (bid < kMinPrice || ask > kMaxPrice || bid >= ask) {
        out.reason = "no room between the touch and the price bounds";
        return out;
    }

    // Inventory: stop adding to the side that would make the position longer
    // or shorter still. The position is signed in YES contracts.
    const bool too_long = params.max_inventory > 0 && position.net >= params.max_inventory;
    const bool too_short = params.max_inventory > 0 && position.net <= -params.max_inventory;
    if (!too_long) {
        out.bid = Quote{bid, params.quantity};
    } else {
        out.reason = "long inventory limit reached";
    }
    if (!too_short) {
        out.ask = Quote{ask, params.quantity};
    } else {
        out.reason = out.reason.empty() ? "short inventory limit reached" : out.reason;
    }
    return out;
}

QuoteManager::QuoteManager(Params params, Hooks hooks)
    : params_(std::move(params)), hooks_(std::move(hooks)) {}

bool QuoteManager::worth_replacing(const LiveQuote& live, const Quote& desired) const {
    if (live.quantity != desired.quantity) return true;
    // Offering the exact target the venue just slid would only be slid again,
    // so that one price is not retried. Any other target is, which is what
    // keeps the quotes following the mid.
    if (live.slid && desired.price == live.requested) return false;

    // Otherwise: how far is the target from where the quote actually rests?
    // Measuring against the resting price is what makes the quotes track the
    // mid rather than the intention behind them.
    const Price tick = tick_at(live.price, params_.ticks);
    const Price moved = live.price > desired.price ? live.price - desired.price
                                                   : desired.price - live.price;
    // A flicker of less than the threshold is left alone, so the strategy does
    // not cancel and replace on every update.
    return moved >= tick * std::max(1, params_.requote_threshold_ticks);
}

void QuoteManager::reconcile_side(Side side, const std::optional<Quote>& desired,
                                  std::optional<LiveQuote>& live, const std::string& symbol) {
    const auto log = [&](const std::string& message) {
        if (hooks_.log) hooks_.log(message);
    };

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

void QuoteManager::announce(const InstrumentView& book, const DesiredQuotes& desired, bool may_send) {
    std::string line;
    if (desired.bid || desired.ask) {
        const Price best_bid = book.yes.empty() ? 0 : book.yes.front().price;
        const Price best_ask = book.no.empty() ? 0 : kPriceOne - book.no.front().price;
        line = "market " + format_price(best_bid) + " / " + format_price(best_ask) + ", mid " +
               format_price((best_bid + best_ask) / 2) + " -> quoting " +
               (desired.bid ? format_price(desired.bid->price) : std::string("-")) + " / " +
               (desired.ask ? format_price(desired.ask->price) : std::string("-"));
        if (!desired.reason.empty()) line += " (" + desired.reason + ")";
    } else {
        line = "not quoting: " + (desired.reason.empty() ? std::string("no target") : desired.reason);
    }
    if (!may_send) line += " [not sending]";
    // Only when it changes: this runs several times a second.
    if (line == last_announcement_) return;
    last_announcement_ = line;
    if (hooks_.log) hooks_.log(line);
}

void QuoteManager::on_market(const InstrumentView& book, const Position& position,
                             Clock::time_point now) {
    (void)now;
    ++passes_;
    // Computed first, and always: a run that sends nothing should still be
    // able to say what it would have done and why.
    const auto desired = desired_quotes(book, position, params_);
    last_reason_ = desired.reason;
    if (!desired.bid && !desired.ask) ++passes_without_quotes_;

    const bool may_send = !hooks_.can_submit || hooks_.can_submit();
    announce(book, desired, may_send);
    if (!may_send) {
        // Reconciling, halted, or a dry run. Quotes already live are left to
        // the reconciler's mass cancel rather than fought over from here.
        return;
    }
    reconcile_side(Side::Buy, desired.bid, bid_, params_.symbol);
    reconcile_side(Side::Sell, desired.ask, ask_, params_.symbol);
}

void QuoteManager::on_report(const ExecutionReport& report) {
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

void QuoteManager::withdraw(const std::string& why) {
    if (hooks_.log) hooks_.log("withdrawing both quotes: " + why);
    for (auto* quote : {&bid_, &ask_}) {
        if (!*quote || (*quote)->cancel_pending) continue;
        CancelOrder request;
        request.cl_ord_id = hooks_.next_cl_ord_id();
        request.orig_cl_ord_id = (*quote)->cl_ord_id;
        request.symbol = params_.symbol;
        request.side = quote == &bid_ ? Side::Buy : Side::Sell;
        if (hooks_.cancel(request)) {
            (*quote)->cancel_pending = true;
            ++cancelled_;
        }
    }
}

std::optional<Quote> QuoteManager::live_bid() const {
    if (!bid_) return std::nullopt;
    return Quote{bid_->price, bid_->quantity};
}

std::optional<Quote> QuoteManager::live_ask() const {
    if (!ask_) return std::nullopt;
    return Quote{ask_->price, ask_->quantity};
}

}  // namespace cpm::strategy
