#pragma once

#include "cpm/md/ticker.hpp"
#include "cpm/md/ticks.hpp"
#include "cpm/strategy/registry.hpp"
#include "cpm/strategy/quote_reconciler.hpp"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// CryptoMM: a two-sided quoting strategy. It quotes a bid and an ask a fixed
// number of ticks apart around the mid, each for a fixed quantity, and
// re-quotes when the target moves. Which of the two the mid's rounding
// favours is the lean: the resting sizes, or the position once there is one.
// See desired_quotes() below.
//
// It depends on both containers - the book from market_data, the order path
// from order_management - and on neither of them depending on it.
namespace cpm::strategy {

// InstrumentView, Position, ExecutionReport, NewOrder, CancelOrder and Clock
// all come from cpm/strategy/strategy.hpp, via registry.hpp.

// The tick grid, from the REST PriceLevelStructure values (docs/design.md O5).
// TAPERED_DECI_CENT is what the crypto series use.
//
// It now belongs to the instrument rather than to this strategy - a grid
// decides what any order may be priced at, and what rungs a ladder has - so
// the definition lives in cpm/md/ticks.hpp and these names stay as they were.
using cpm::md::tick_at;

struct Params {
    std::string symbol;
    // The whole width of the market we make, in ticks: each side is quoted
    // half of this away from the mid, so 2 is a two-tick-wide market. An odd
    // width puts the pair on a half tick, which is what the rounding tilt
    // below resolves - and it resolves it without ever widening us.
    int spread_ticks = 4;
    // How much to show on each side (the "y"), in hundredths of a contract.
    Qty quantity = 100;
    // Stop the strategy once the net position reaches this, long or short,
    // so it cannot accumulate without limit. 0 disables the check.
    //
    // It stops rather than quoting only the reducing side: a position at the
    // limit is the operator's to decide about, and a strategy that went on
    // working it off by itself would be making that decision for them. What
    // it leaves behind is the position - nothing flattens it, and the row
    // stays on the page with its feed up, saying why it stopped.
    Qty max_inventory = 500;
    // Re-quote only when the target has moved at least this many ticks, so a
    // one-tick flicker does not cause a cancel and replace on every update.
    int requote_threshold_ticks = 1;
    // Stop quoting this many seconds before the window ends, cancelling what
    // is resting while the market is still open.
    //
    // Zero - the default - means quote to the bell and let the venue cancel
    // at the close, which it does (ExecRestatementReason=2). That is safe
    // mechanically: what it accepts is the trading risk of resting through
    // the last seconds of a binary, where the informed flow is.
    //
    // It needs a window end to act on, which comes from the instrument's own
    // name ([MD] O2). On an instrument whose name carries no end - a sports
    // market, a simulator symbol - there is nothing to count back from and
    // this does nothing.
    std::chrono::seconds withdraw_before{0};
    // The speed bump (GUI column PricingMidMoveThreshold, B-Q2). If the
    // reference venue's mid moves this many of ITS ticks between one update
    // and the next, the quotes are stale: cancel them, and quote again on the
    // next update of this market's own book.
    //
    // 0 - the default - is off, and the strategy is exactly what it was.
    std::int64_t mid_move_threshold_ticks = 0;
    // BADQ (the GUI column): the widest market, in grid rungs, that this
    // strategy will lean on. While the book EXCLUDING OUR OWN ORDERS is wider
    // than this, both quotes come off and nothing is placed; when it narrows
    // back inside, quoting resumes by itself.
    //
    // Measured on the book without our orders because that is the book the
    // mid is taken from, and because the raw book is the wrong answer
    // exactly when this guard is needed: when our own pair is the only thing
    // at the touch, the raw spread is our own width and looks tight. On
    // 2026-11-19 at 06:13 the raw book read 0.70/0.72 while the market
    // behind it was 0.44/0.98; the mid of that vacuum is 0.71, so the
    // strategy bid 0.70 in a market that had been trading at 0.50 ten
    // seconds earlier and was taken twice inside eleven seconds.
    //
    // In grid rungs and not in price, because the grid is tapered: 5 rungs is
    // 0.05 between 0.100 and 0.900 and 0.005 outside it (md/ticks.hpp). A
    // difference in price is therefore not a number of ticks, which is why
    // this is counted by walking the grid rather than by subtracting.
    //
    // 0 - the default - is off, and the strategy is exactly what it was.
    int max_quote_spread_ticks = 0;
};

// The whole decision, as a pure function of the book, the position and the
// parameters: no orders, no clock, no state. Everything the strategy decides
// is decided here, so it can be tested by inspection.
//
// The mid comes from both sides of the YES book: the best YES bid and the YES
// ask implied by the best NO bid (1.000 - best NO bid). Without both there is
// no mid and nothing is quoted.
//
// Both quotes are mid +/- half the spread, and both round the same way onto
// the grid. Which way is the whole of the lean:
//
//   * past a third of the maximum position, the position decides - round down
//     when long, so the ask lands on the offer and sells first; round up when
//     short, so the bid lands on the bid and buys first;
//   * otherwise the resting sizes decide - round up towards the heavier bid,
//     down towards the heavier ask, and down on a tie.
//
// Rounding both prices the same way is what keeps the width exactly as
// configured: rounding each away from the touch instead would widen us by a
// tick whenever the mid fell between two of them.
// `ours` is what this daemon has resting on the instrument. Those quantities
// come off the book before the mid and the sizes are read: the touch we lean
// on is the one other people are making. A level whose whole size is ours
// stops being the touch, and if nothing of anyone else's is left on a side
// there is no mid and nothing is quoted.
//
// The raw best bid and ask keep their own jobs - the crossed-book check and
// the guard that never quotes through what is resting - because a post-only
// order crosses our own orders exactly as it crosses anyone's.
DesiredQuotes desired_quotes(const InstrumentView& book, const Position& position,
                             const Params& params, const std::vector<LiveLeg>& ours = {});

// Drives the desired quotes onto the venue: places what is missing, cancels
// what has moved, and leaves alone what is still right. The venue has no
// amend, so a move is a cancel and a re-submit (OE "Not supported").
class QuoteManager : public QuoteReconciler {
public:
    QuoteManager(Params params, Hooks hooks);
    void on_market(const InstrumentView& book, const Position& position, Clock::time_point now);
    void on_reference(const cpm::binance::ReferenceView& reference, Clock::time_point now);

private:
    void announce(const InstrumentView& book, const DesiredQuotes& desired, bool may_send);
    Params params_;
    std::string last_announcement_;

    // --- the speed bump ---------------------------------------------------
    // The reference mid at the previous update. A breach is one update
    // against the one before it, not a drift summed since we priced.
    std::optional<Price> previous_reference_;
    // The latest the reference feed has said, and whether it can be trusted.
    cpm::binance::ReferenceView reference_;
    // Sitting out after a breach, until this market's book updates. The host
    // calls on_market on every pass, reference wakes included, so "an update"
    // is the book's own timestamp moving, not merely being shown the book.
    bool held_ = false;
    std::optional<decltype(InstrumentView::book_time)> book_time_at_withdraw_;
    // The timestamp of the last book we were shown, which the release above
    // is measured against.
    std::optional<decltype(InstrumentView::book_time)> last_book_time_;
};

// CryptoMM as the host runs it: a QuoteManager, its parameters, and nothing
// else. The decision still lives in desired_quotes() and the driving still
// lives in QuoteManager; this only adapts them to IStrategy.
class CryptoMm final : public IStrategy {
public:
    CryptoMm(Params params, StrategyHooks hooks);

    void on_market(const InstrumentView& book, const Position& position,
                   Clock::time_point now) override;
    void on_reference(const cpm::binance::ReferenceView& reference,
                      Clock::time_point now) override;
    void on_report(const ExecutionReport& report) override;
    void withdraw(const std::string& why) override;
    void abandon(const std::string& why) override;
    bool quiescent() const override;
    std::vector<std::string> symbols() const override;
    StrategyStatus status() const override;

    const Params& params() const { return params_; }

private:
    Params params_;
    QuoteManager manager_;
    // The instrument's own window end, parsed once. The outer optional is
    // "not looked at yet"; the inner one is "this name has no end", which is
    // the answer for a sports market or a simulator symbol.
    std::optional<std::optional<cpm::md::Ticker>> window_end_;
    bool withdrawn_for_close_ = false;
};

// The parameters CryptoMM takes, described once. The GUI renders a form from
// this and crypto_mm validates its flags against it, so the two cannot
// disagree about what --width-ticks 0 means.
ParamSchema crypto_mm_schema();
// Turns an already validated value set into Params.
Params crypto_mm_params(const ParamValues& values);
void register_crypto_mm(StrategyRegistry& registry);

}  // namespace cpm::strategy
