#include "cpm/strategy/crypto_mm.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <utility>

namespace cpm::strategy {
namespace {

using cpm::oms::Side;

constexpr Price kMinPrice = 1;              // 0.001
constexpr Price kMaxPrice = kPriceOne - 1;  // 0.999

// Is `ask` more than `limit` grid rungs above `bid`? That is the BADQ test.
//
// Walked rather than subtracted, because the grid is tapered - 0.001 below
// 0.100, 0.01 between, 0.001 above 0.900 - so a difference in price is not a
// number of ticks, and the same 5 ticks is 0.05 at 0.50 and 0.005 at 0.95.
// price_above() is the venue's own stepping rule and the only thing that gets
// the boundaries right: a step up from 0.900 lands on 0.910, not 0.901.
//
// At most `limit` steps, so the cost is the limit and not the width of the
// book: an empty book is 126 rungs wide and we only need to know that it is
// more than five. Exactly `limit` rungs is inside the limit, because the
// operator's rule is "wider than BADQ", not "at least BADQ".
bool wider_than(Price bid, Price ask, int limit) {
    Price at = bid;
    for (int step = 0; step < limit; ++step) {
        if (at >= ask) return false;
        at = cpm::md::price_above(at);
    }
    return at < ask;
}

// floor() and ceil() of v/2, for prices carried at twice their scale. The mid
// of a one-tick market sits on a half, and so does either side of an odd
// spread, so the halves are kept until the rounding below consumes them:
// truncating earlier would quietly turn a one-tick spread into none at all.
Price floor_half(Price v) { return v >= 0 ? v / 2 : -((-v + 1) / 2); }
Price ceil_half(Price v) { return v >= 0 ? (v + 1) / 2 : -((-v) / 2); }

// One side of the book with our own orders taken out of it: the best level
// that still has somebody else's size on it, and how much of it is theirs.
//
// `mine` is the price each of our orders on this side rests at, which is not
// always the price we asked for - a post-only slide moves it. Our sells rest
// on the NO book at 1.000 minus their YES price, which is how the feed
// carries them, so the caller converts before it gets here.
struct TopOfBook {
    bool present = false;
    Price price = 0;
    Qty qty = 0;
};

TopOfBook top_excluding_ours(const std::vector<cpm::md::BookLevel>& levels,
                             const std::map<Price, Qty>& mine) {
    for (const auto& level : levels) {
        const auto found = mine.find(level.price);
        const Qty ours = found == mine.end() ? 0 : found->second;
        // Never below zero: our record of what is resting and the book's can
        // disagree for an instant either way, and a negative here would turn
        // a level we are not even on into the touch.
        const Qty theirs = level.qty > ours ? level.qty - ours : 0;
        if (theirs > 0) return {true, level.price, theirs};
    }
    return {};
}

}  // namespace

DesiredQuotes desired_quotes(const InstrumentView& book, const Position& position,
                            const Params& params, const std::vector<LiveLeg>& ours) {
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
    // What is resting that is not ours. A buy of ours sits on the YES book at
    // its own price; a sell sits on the NO book at 1.000 minus it, which is
    // how this venue carries the other side.
    std::map<Price, Qty> our_bids;
    std::map<Price, Qty> our_asks;
    for (const auto& leg : ours) {
        if (leg.quantity <= 0) continue;
        if (leg.side == cpm::oms::Side::Buy) {
            our_bids[leg.price] += leg.quantity;
        } else {
            our_asks[kPriceOne - leg.price] += leg.quantity;
        }
    }
    const auto bid_ex_ours = top_excluding_ours(book.yes, our_bids);
    const auto no_bid_ex_ours = top_excluding_ours(book.no, our_asks);
    if (!bid_ex_ours.present || !no_bid_ex_ours.present) {
        // Nobody else is showing on one of the sides. There is no market to
        // lean on, and leaning on our own quote would only be this strategy
        // reading itself back.
        out.reason = "one-sided book once our own orders are taken out";
        return out;
    }
    const Price ask_ex_ours = kPriceOne - no_bid_ex_ours.price;
    if (ask_ex_ours <= bid_ex_ours.price) {
        out.reason = "crossed or locked book once our own orders are taken out";
        return out;
    }

    // BADQ: a market this wide is not a price. Before the mid is taken,
    // because the midpoint of a vacuum is arithmetic and not information -
    // the mid of 0.44/0.98 is 0.71, and quoting around it is how this
    // strategy came to bid 0.70 for something worth 0.47 (2026-11-19 06:13).
    //
    // Both quotes come off, which is what returning with neither side set
    // means: QuoteReconciler cancels whatever is live and places nothing.
    // Nothing latches, so the next pass that sees a market inside the limit
    // quotes again - the condition is a property of the book in front of us
    // and not a memory of the past, which is what makes this simpler than
    // the reference speed bump above.
    if (params.max_quote_spread_ticks > 0 &&
        wider_than(bid_ex_ours.price, ask_ex_ours, params.max_quote_spread_ticks)) {
        out.reason = "the market is " + format_price(bid_ex_ours.price) + " / " +
                     format_price(ask_ex_ours) + " once our own orders are taken out, wider "
                     "than the BADQ limit of " +
                     std::to_string(params.max_quote_spread_ticks) + " ticks";
        return out;
    }

    // Twice the mid, which is exact. The mid itself is only wanted for the
    // tick band, where half a unit cannot change the answer.
    const Price mid2 = bid_ex_ours.price + ask_ex_ours;
    const Price mid = mid2 / 2;

    const Price tick = tick_at(mid);
    // The whole width, so each side is half of it away from the mid.
    const Price span = tick * params.spread_ticks;

    // Which way the pair leans. Past a third of the maximum position the
    // position decides and nothing else does: it is the inventory that has to
    // come off. Below that, the resting sizes decide.
    //
    // 3 * |net| against the limit rather than the limit divided by 3, because
    // the division would have to round and this way nothing does. A zero
    // limit disables this as it disables the cap below.
    const Qty net = position.net;
    const Qty magnitude = net < 0 ? -net : net;
    const bool position_decides = params.max_inventory > 0 && 3 * magnitude > params.max_inventory;
    const Qty bid_qty = bid_ex_ours.qty;
    // The best NO bid is the best YES offer, one contract for one, so the two
    // sizes compare directly.
    const Qty ask_qty = no_bid_ex_ours.qty;
    // Long rounds down, so the ask lands on the offer and sells before it
    // buys; short rounds up, so the bid lands on the bid. Flat enough, and we
    // lean towards whichever side is showing more.
    const bool round_up = position_decides ? net < 0 : bid_qty > ask_qty;
    out.rounded_up = round_up;
    out.round_reason = position_decides ? "position" : "qty imbalance";

    // Onto the grid, both sides the same way. That is what holds the width at
    // exactly span: rounding each side away from the touch instead would add a
    // tick whenever the mid fell between two of them.
    Price bid = round_up ? cpm::md::snap_up(ceil_half(mid2 - span))
                         : cpm::md::snap_down(floor_half(mid2 - span));
    Price ask = round_up ? cpm::md::snap_up(ceil_half(mid2 + span))
                         : cpm::md::snap_down(floor_half(mid2 + span));
    bid = std::clamp(bid, kMinPrice, kMaxPrice);
    ask = std::clamp(ask, kMinPrice, kMaxPrice);

    // Never quote through the resting book: a post-only order that would cross
    // is either slid or rejected, and a reject would halt the run (OE-Q6).
    //
    // One price below the touch, not one tick below it. The touch is the
    // venue's price and need not sit on our grid, and a tick taken off it
    // lands between rungs: one below 99% is 98%, not 98.9%, and an order at
    // 98.9% is the reject this line exists to avoid.
    if (bid >= best_ask) bid = cpm::md::price_below(best_ask);
    if (ask <= best_bid) ask = cpm::md::price_above(best_bid);
    if (bid < kMinPrice || ask > kMaxPrice || bid >= ask) {
        out.reason = "no room between the touch and the price bounds";
        return out;
    }

    // Inventory: at the limit the strategy is done. Both sides come off and
    // it stops itself - it does not go on quoting the reducing side, because
    // a position at the limit is the operator's to decide about. The
    // position is signed in YES contracts.
    const bool too_long = params.max_inventory > 0 && position.net >= params.max_inventory;
    const bool too_short = params.max_inventory > 0 && position.net <= -params.max_inventory;
    if (too_long || too_short) {
        out.stop = true;
        out.reason = std::string(too_long ? "long" : "short") + " inventory limit reached: net " +
                     cpm::format_signed(position.net, cpm::kQtyScale) + " of " +
                     cpm::format_qty(params.max_inventory);
        return out;
    }
    out.bid = Quote{bid, params.quantity};
    out.ask = Quote{ask, params.quantity};
    return out;
}

QuoteManager::QuoteManager(Params params, Hooks hooks)
    : QuoteReconciler({params.symbol, params.requote_threshold_ticks}, std::move(hooks)),
      params_(std::move(params)) {}

void QuoteManager::announce(const InstrumentView& book, const DesiredQuotes& desired, bool may_send) {
    std::string line;
    if (desired.bid || desired.ask) {
        const Price best_bid = book.yes.empty() ? 0 : book.yes.front().price;
        const Price best_ask = book.no.empty() ? 0 : kPriceOne - book.no.front().price;
        line = "market " + format_price(best_bid) + " / " + format_price(best_ask) + ", mid " +
               format_price((best_bid + best_ask) / 2) + " -> quoting " +
               (desired.bid ? format_price(desired.bid->price) : std::string("-")) + " / " +
               (desired.ask ? format_price(desired.ask->price) : std::string("-"));
        if (!desired.round_reason.empty()) {
            line += ", rounded " + std::string(desired.rounded_up ? "up" : "down") + " on " +
                    desired.round_reason;
        }
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
    if (!reference.usable()) {
        // A gap in the feed is not one update: the stale path withdraws on
        // its own, and the first price after it starts a fresh comparison.
        previous_reference_.reset();
        return;
    }
    const auto previous = previous_reference_;
    previous_reference_ = reference.mid;
    if (!previous) return;

    // One update against the one before it (B-Q2), not a drift summed
    // since we priced.
    const Price moved = reference.mid > *previous ? reference.mid - *previous
                                                  : *previous - reference.mid;
    const Price allowed = params_.mid_move_threshold_ticks * reference.tick_size;
    if (moved < allowed) return;
    if (held_) return;  // already out; nothing to do until this market updates

    held_ = true;
    book_time_at_withdraw_ = last_book_time_;
    const std::string why = "the reference moved " +
                            std::to_string(moved / reference.tick_size) +
                            " ticks in one update; waiting for this market";
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
    last_book_time_ = book.book_time;

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
            // Out until this market's book updates. Any update will do; the
            // same book shown again on a reference wake is not one.
            const bool updated = !book_time_at_withdraw_ ||
                                 *last_book_time_ != *book_time_at_withdraw_;
            if (!updated) {
                last_reason_ = "waiting for this market to update after the reference moved";
                ++passes_without_quotes_;
                return;
            }
            held_ = false;
            book_time_at_withdraw_.reset();
            if (hooks_.log) hooks_.log("this market updated; quoting again");
        }
    }

    // Computed first, and always: a run that sends nothing should still be
    // able to say what it would have done and why.
    const auto desired = desired_quotes(book, position, params_,
                                        hooks_.resting_orders
                                            ? hooks_.resting_orders(params_.symbol)
                                            : std::vector<LiveLeg>{});
    last_reason_ = desired.reason;
    log_quote_availability(desired.bid.has_value(), desired.ask.has_value(), desired.reason);
    if (!desired.bid && !desired.ask) ++passes_without_quotes_;

    const bool may_send = !hooks_.can_submit || hooks_.can_submit();
    announce(book, desired, may_send);

    // The inventory limit ends the run. The host takes the quotes off and
    // moves the row to Stopping on its way out of this pass, so nothing is
    // placed here and nothing is cancelled twice. With no hook to ask - the
    // one-shot crypto_mm binary has none - the quotes still come off, which
    // is as far as a strategy with no owner can go.
    if (desired.stop) {
        if (hooks_.stop_me) {
            hooks_.stop_me(desired.reason);
        } else {
            withdraw(desired.reason);
        }
        return;
    }
    if (!may_send) {
        // Reconciling, halted, or a dry run. Quotes already live are left to
        // the reconciler's mass cancel rather than fought over from here.
        return;
    }
    reconcile_side(Side::Buy, desired.bid, bid_, params_.symbol);
    reconcile_side(Side::Sell, desired.ask, ask_, params_.symbol);
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
    width.default_value = "4";
    width.min = "1";
    width.max = "500";
    width.help = "the whole width of the market to make, in ticks: 2 quotes a two-tick "
                 "market, one tick either side of the mid";
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
    inventory.help =
        "stop the strategy at this net position, long or short; 0 disables the check";
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
                      "of its own ticks in one update, and quote again on this market's next "
                      "book update; 0 is off. BTCUSDT ticks at 0.1, so 10 is a $1 move";
    schema.push_back(std::move(speed_bump));

    ParamSpec badq;
    badq.name = "max-quote-spread";
    // The operator's own spelling for the column, as PricingMidMoveThreshold is.
    badq.label = "BADQ";
    badq.type = ParamSpec::Type::Int;
    badq.default_value = "0";
    badq.min = "0";
    badq.max = "500";
    badq.help = "pull both quotes while the market excluding our own orders is wider than "
                "this many ticks, and quote again once it narrows back inside; 0 is off. "
                "Ticks are grid rungs, so 5 is 0.05 between 0.100 and 0.900 and 0.005 "
                "outside it";
    schema.push_back(std::move(badq));

    return schema;
}

Params crypto_mm_params(const ParamValues& values) {
    Params params;
    params.symbol = param_text(values, "symbol");
    params.spread_ticks = static_cast<int>(param_int(values, "width-ticks"));
    params.quantity = param_decimal(values, "qty", cpm::kQtyScale);
    params.max_inventory = param_decimal(values, "max-inventory", cpm::kQtyScale);
    params.requote_threshold_ticks = static_cast<int>(param_int(values, "requote-threshold"));
    // The schema has already refused anything else.
    params.withdraw_before = std::chrono::seconds(param_int(values, "withdraw-before"));
    params.mid_move_threshold_ticks = param_int(values, "pricing-mid-move-threshold");
    params.max_quote_spread_ticks = static_cast<int>(param_int(values, "max-quote-spread"));
    return params;
}

void register_crypto_mm(StrategyRegistry& registry) {
    registry.add("CryptoMM",
                 "quotes a market x ticks wide around the mid, y on each side",
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
