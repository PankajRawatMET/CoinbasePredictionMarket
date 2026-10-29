#include "cpm/strategy/crypto_mm.hpp"

#include <algorithm>
#include <memory>
#include <utility>

namespace cpm::strategy {
namespace {

using cpm::oms::Side;

constexpr Price kMinPrice = 1;              // 0.001
constexpr Price kMaxPrice = kPriceOne - 1;  // 0.999

// The mid of a CPM book, by the same rule desired_quotes uses: both sides,
// with the YES ask implied by the best NO bid. Nullopt when there is not
// one, which the guard reads as "nothing to measure against".
std::optional<Price> market_mid(const InstrumentView& book) {
    if (!book.book_valid || book.yes.empty() || book.no.empty()) return std::nullopt;
    const Price best_bid = book.yes.front().price;
    const Price best_ask = kPriceOne - book.no.front().price;
    if (best_ask <= best_bid) return std::nullopt;
    return (best_bid + best_ask) / 2;
}

}  // namespace

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

    // Onto the grid, away from the other side: rounding must never move a
    // quote towards the touch.
    Price bid = cpm::md::snap_down(mid - offset, params.ticks);
    Price ask = cpm::md::snap_up(mid + offset, params.ticks);
    bid = std::clamp(bid, kMinPrice, kMaxPrice);
    ask = std::clamp(ask, kMinPrice, kMaxPrice);

    // Never quote through the resting book: a post-only order that would cross
    // is either slid or rejected, and a reject would halt the run (OE-Q6).
    //
    // One price below the touch, not one tick below it. The touch is the
    // venue's price and need not sit on our grid, and a tick taken off it
    // lands between rungs: one below 99% is 98%, not 98.9%, and an order at
    // 98.9% is the reject this line exists to avoid.
    if (bid >= best_ask) bid = cpm::md::price_below(best_ask, params.ticks);
    if (ask <= best_bid) ask = cpm::md::price_above(best_bid, params.ticks);
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

void QuoteManager::on_reference(const cpm::binance::ReferenceView& reference,
                                Clock::time_point now) {
    (void)now;
    reference_ = reference;
    if (params_.mid_move_threshold_ticks <= 0) return;  // the guard is off
    if (!reference.usable() || !reference_at_quote_) return;

    const Price moved = reference.mid > *reference_at_quote_
                            ? reference.mid - *reference_at_quote_
                            : *reference_at_quote_ - reference.mid;
    const Price allowed = params_.mid_move_threshold_ticks * reference.tick_size;
    if (moved < allowed) return;
    if (held_) return;  // already out; nothing to do until this market moves

    held_ = true;
    mid_at_withdraw_ = last_market_mid_;
    const std::string why = "the reference moved " +
                            std::to_string(moved / reference.tick_size) +
                            " ticks since we priced; waiting for this market";
    last_reason_ = why;
    if (hooks_.log) hooks_.log(why);
    // Cancel, not abandon: these orders are still live on an open market and
    // nobody else is going to take them off. withdraw() leaves the live
    // quotes to be cancelled and stops quoting until the next on_market().
    withdraw(why);
}

void QuoteManager::on_market(const InstrumentView& book, const Position& position,
                             Clock::time_point now) {
    (void)now;
    ++passes_;
    last_market_mid_ = market_mid(book);

    // The guard, before anything is priced. Both of these end the pass: a
    // strategy with no trustworthy reference must not quote at all, which
    // is the whole of B-Q4.
    if (params_.mid_move_threshold_ticks > 0) {
        if (!reference_.usable()) {
            const std::string why =
                reference_.present
                    ? (reference_.tick_size > 0
                           ? "the reference feed is stale (" +
                                 std::to_string(reference_.age.count()) + "ms)"
                           : "the reference venue's tick size is not known")
                    : "no reference price yet";
            last_reason_ = why;
            ++passes_without_quotes_;
            withdraw(why);
            return;
        }
        if (held_) {
            // Out until this market reprices. Any move of a tick will do:
            // what matters is that CPM has absorbed what the reference
            // already knows (B-Q3).
            const bool moved = last_market_mid_ && mid_at_withdraw_ &&
                               *last_market_mid_ != *mid_at_withdraw_;
            if (!moved) {
                last_reason_ = "waiting for this market to reprice after the reference moved";
                ++passes_without_quotes_;
                return;
            }
            held_ = false;
            mid_at_withdraw_.reset();
            if (hooks_.log) hooks_.log("this market repriced; quoting again");
        }
    }

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

    // Whatever is resting now was priced against this reference, so the next
    // breach is measured from here (B-Q9).
    if ((bid_ || ask_) && reference_.usable()) {
        reference_at_quote_ = reference_.mid;
    } else if (!bid_ && !ask_) {
        reference_at_quote_.reset();
    }
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

void QuoteManager::abandon(const std::string& why) {
    // No cancels. The window has closed and the venue has already terminated
    // whatever was on it; a cancel sent now would be answered with
    // UNKNOWN_ORDER, and a cancel reject halts the daemon (OE-Q6).
    if (!bid_ && !ask_) return;
    if (hooks_.log) hooks_.log("abandoning both quotes to the venue's own cancel: " + why);
    bid_.reset();
    ask_.reset();
}

std::optional<Quote> QuoteManager::live_bid() const {
    if (!bid_) return std::nullopt;
    return Quote{bid_->price, bid_->quantity};
}

std::optional<Quote> QuoteManager::live_ask() const {
    if (!ask_) return std::nullopt;
    return Quote{ask_->price, ask_->quantity};
}

std::vector<LiveLeg> QuoteManager::legs() const {
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

// --- CryptoMM as a hosted strategy ----------------------------------------

ParamSchema crypto_mm_schema() {
    ParamSchema schema;

    ParamSpec symbol;
    symbol.name = "symbol";
    symbol.type = ParamSpec::Type::Text;
    symbol.help = "the instrument to quote; md_client --list-instruments names the live ones";
    schema.push_back(std::move(symbol));

    ParamSpec width;
    width.name = "width-ticks";
    // The operator's word for it. The flag keeps its name.
    width.label = "Spread";
    width.type = ParamSpec::Type::Int;
    width.default_value = "2";
    width.min = "1";
    width.max = "500";
    width.help = "how far either side of the mid to quote, in ticks";
    schema.push_back(std::move(width));

    ParamSpec qty;
    qty.name = "qty";
    qty.type = ParamSpec::Type::Decimal;
    qty.scale = cpm::kQtyScale;
    qty.default_value = "1";
    qty.min = "0.01";
    qty.max = "10000";
    qty.help = "contracts to show on each side";
    schema.push_back(std::move(qty));

    ParamSpec inventory;
    inventory.name = "max-inventory";
    inventory.type = ParamSpec::Type::Decimal;
    inventory.scale = cpm::kQtyScale;
    inventory.default_value = "5";
    inventory.min = "0";
    inventory.max = "1000000";
    inventory.help = "stop adding to a side at this net position; 0 disables the check";
    schema.push_back(std::move(inventory));

    ParamSpec threshold;
    threshold.name = "requote-threshold";
    threshold.type = ParamSpec::Type::Int;
    threshold.default_value = "1";
    threshold.min = "1";
    threshold.max = "500";
    threshold.help = "re-quote only once the target has moved this many ticks";
    schema.push_back(std::move(threshold));

    ParamSpec withdraw;
    withdraw.name = "withdraw-before";
    withdraw.type = ParamSpec::Type::Int;
    withdraw.default_value = "0";
    withdraw.min = "0";
    withdraw.max = "900";
    withdraw.help = "seconds before the window ends to pull the quotes; 0 rests to the "
                    "bell and lets the venue cancel at the close";
    schema.push_back(std::move(withdraw));

    ParamSpec speed_bump;
    speed_bump.name = "pricing-mid-move-threshold";
    // The operator's own spelling for the column (GUI-Q25's label).
    speed_bump.label = "PricingMidMoveThreshold";
    speed_bump.type = ParamSpec::Type::Int;
    speed_bump.default_value = "0";
    speed_bump.min = "0";
    speed_bump.help = "cancel and stand aside when the reference venue's mid moves this many "
                      "of its own ticks from where it was when we priced; 0 is off. BTCUSDT "
                      "ticks at 0.01, so 2500 is a $25 move";
    schema.push_back(std::move(speed_bump));

    ParamSpec ticks;
    ticks.name = "ticks";
    ticks.type = ParamSpec::Type::Enum;
    ticks.default_value = "tapered";
    ticks.choices = {"tapered", "tapered-cent", "cent", "deci"};
    ticks.help = "the instrument's tick grid; tapered is TAPERED_DECI_CENT, which is "
                 "what the venue reports for BTCD15M and ETHD15M over REST";
    schema.push_back(std::move(ticks));

    return schema;
}

Params crypto_mm_params(const ParamValues& values) {
    Params params;
    params.symbol = param_text(values, "symbol");
    params.half_width_ticks = static_cast<int>(param_int(values, "width-ticks"));
    params.quantity = param_decimal(values, "qty", cpm::kQtyScale);
    params.max_inventory = param_decimal(values, "max-inventory", cpm::kQtyScale);
    params.requote_threshold_ticks = static_cast<int>(param_int(values, "requote-threshold"));
    // The schema has already refused anything else.
    params.withdraw_before = std::chrono::seconds(param_int(values, "withdraw-before"));
    params.ticks = tick_structure_from(param_text(values, "ticks"))
                       .value_or(TickStructure::TaperedDeciCent);
    params.mid_move_threshold_ticks = param_int(values, "pricing-mid-move-threshold");
    return params;
}

void register_crypto_mm(StrategyRegistry& registry) {
    registry.add("CryptoMM",
                 "quotes a bid and an ask x ticks either side of the mid, y on each side",
                 crypto_mm_schema(),
                 [](const ParamValues& values, StrategyHooks hooks) -> std::unique_ptr<IStrategy> {
                     return std::make_unique<CryptoMm>(crypto_mm_params(values), std::move(hooks));
                 });
}

CryptoMm::CryptoMm(Params params, StrategyHooks hooks)
    : params_(std::move(params)), manager_(params_, std::move(hooks)) {}

void CryptoMm::on_reference(const cpm::binance::ReferenceView& reference,
                            Clock::time_point now) {
    manager_.on_reference(reference, now);
}

void CryptoMm::on_market(const InstrumentView& book, const Position& position,
                         Clock::time_point now) {
    // Pull out before the window ends, if asked to. The end comes from the
    // instrument's own name ([MD] O2), and the cancels go out while the
    // market is still open - which is the only time cancelling is safe, since
    // one sent into the close races the venue's own cancel and loses (OE-Q6).
    if (params_.withdraw_before > std::chrono::seconds{0}) {
        if (!window_end_) window_end_ = cpm::md::parse_ticker(params_.symbol);
        if (*window_end_ && now >= (*window_end_)->window_end - params_.withdraw_before) {
            if (!withdrawn_for_close_) {
                withdrawn_for_close_ = true;
                manager_.withdraw("within " + std::to_string(params_.withdraw_before.count()) +
                                  "s of the close");
            }
            return;
        }
    }
    withdrawn_for_close_ = false;
    manager_.on_market(book, position, now);
}

void CryptoMm::on_report(const ExecutionReport& report) { manager_.on_report(report); }

void CryptoMm::withdraw(const std::string& why) { manager_.withdraw(why); }

void CryptoMm::abandon(const std::string& why) { manager_.abandon(why); }

bool CryptoMm::quiescent() const { return manager_.quiescent(); }

std::vector<std::string> CryptoMm::symbols() const { return {params_.symbol}; }

StrategyStatus CryptoMm::status() const {
    StrategyStatus status;
    status.legs = manager_.legs();
    status.reason = manager_.last_reason();
    status.passes = manager_.passes();
    status.passes_without_target = manager_.passes_without_quotes();
    status.placed = manager_.quotes_placed();
    status.cancelled = manager_.quotes_cancelled();
    status.fills = manager_.fills();
    return status;
}

}  // namespace cpm::strategy
