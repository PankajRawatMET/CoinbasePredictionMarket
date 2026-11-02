#pragma once

#include "cpm/md/ticker.hpp"
#include "cpm/md/ticks.hpp"
#include "cpm/strategy/registry.hpp"

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
using TickStructure = cpm::md::TickStructure;
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
    TickStructure ticks = TickStructure::TaperedDeciCent;
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
    // reference venue's mid moves this many of ITS ticks away from where it
    // was when these quotes were placed, the quotes are stale: cancel them
    // and do not come back until this market has repriced.
    //
    // 0 - the default - is off, and the strategy is exactly what it was.
    std::int64_t mid_move_threshold_ticks = 0;
};

// One side's target, or nothing when that side should not be quoted.
struct Quote {
    Price price = 0;
    Qty quantity = 0;
};

struct DesiredQuotes {
    std::optional<Quote> bid;
    std::optional<Quote> ask;
    // The inventory limit has been reached, so this strategy is done: both
    // sides come off and it stops itself, rather than quoting the one side
    // that would reduce the position. Decided here with everything else, and
    // acted on by on_market, which is the only thing that may touch a hook.
    bool stop = false;
    // Why a side is absent, for the log. Empty when both sides are quoted.
    std::string reason;
    // Which way the pair was rounded onto the grid, and what decided it
    // ("position" or "qty imbalance"), for the log. Both quotes always round
    // the same way, so this is one answer and not one per side.
    bool rounded_up = false;
    std::string round_reason;
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
class QuoteManager {
public:
    // Everything it needs from the outside, so it can be driven by a test.
    // The host composes these - arming and the halt are folded into
    // can_submit before they get here - so this is the host's StrategyHooks.
    using Hooks = StrategyHooks;

    QuoteManager(Params params, Hooks hooks);

    // One pass: compare the desired quotes with the live ones and act.
    void on_market(const InstrumentView& book, const Position& position, Clock::time_point now);
    // The reference venue moved. Called on every update from it, which is
    // many times a second and not on this market's cadence, so it does as
    // little as it can: compare, and cancel if the comparison says to.
    void on_reference(const cpm::binance::ReferenceView& reference, Clock::time_point now);
    // Keeps track of which quotes are live. Must be fed every report for this
    // instrument, including the unsolicited ones.
    void on_report(const ExecutionReport& report);
    // Cancels both quotes and stops quoting until the next on_market().
    void withdraw(const std::string& why);
    // Drop the quotes without sending anything: the window has closed and the
    // venue has already cancelled them (ExecRestatementReason=2).
    void abandon(const std::string& why);

    std::optional<Quote> live_bid() const;
    std::optional<Quote> live_ask() const;
    // Both quotes as the host reports them, with the ids and the pending
    // cancels that live_bid()/live_ask() leave out.
    std::vector<LiveLeg> legs() const;
    // Nothing of ours is left at the venue.
    bool quiescent() const { return !bid_ && !ask_; }
    std::size_t quotes_placed() const { return placed_; }
    std::size_t quotes_cancelled() const { return cancelled_; }
    std::size_t fills() const { return fills_; }

    // Why it is not quoting, so a run that places nothing explains itself
    // rather than going quiet. Empty while both sides are quoted.
    const std::string& last_reason() const { return last_reason_; }
    std::size_t passes() const { return passes_; }
    std::size_t passes_without_quotes() const { return passes_without_quotes_; }

private:
    struct LiveQuote {
        std::string cl_ord_id;
        // What was asked for, and where the venue actually rested it. A
        // post-only slide makes these differ.
        Price requested = 0;
        Price price = 0;
        Qty quantity = 0;
        bool cancel_pending = false;
        bool acknowledged = false;  // a New or a fill has been seen for it
        // True when the venue slid this quote. The target that was slid is
        // remembered so it is not offered again unchanged, while any other
        // target still re-quotes: the quotes follow the mid, they just do not
        // retry the one price the venue refused to rest at.
        bool slid = false;
    };

    void reconcile_side(cpm::oms::Side side, const std::optional<Quote>& desired,
                        std::optional<LiveQuote>& live, const std::string& symbol);
    bool worth_replacing(const LiveQuote& live, const Quote& desired) const;
    // Logs what it sees, but only when that changes: on a 250 ms loop a line
    // per pass would bury everything else.
    void announce(const InstrumentView& book, const DesiredQuotes& desired, bool may_send);

    Params params_;
    Hooks hooks_;
    std::optional<LiveQuote> bid_;
    std::optional<LiveQuote> ask_;
    std::size_t placed_ = 0;
    std::size_t cancelled_ = 0;
    std::size_t fills_ = 0;
    std::size_t passes_ = 0;
    std::size_t passes_without_quotes_ = 0;
    std::string last_reason_;
    std::string last_announcement_;

    // --- the speed bump ---------------------------------------------------
    // Where the reference was when these quotes were placed. The measure is
    // always "since we priced", never a drift summed across re-quotes
    // (B-Q9), so this is refreshed on every place and replace.
    std::optional<Price> reference_at_quote_;
    // The latest the reference feed has said, and whether it can be trusted.
    cpm::binance::ReferenceView reference_;
    // Sitting out after a breach, until this market moves a tick from where
    // it was when we left (B-Q3).
    bool held_ = false;
    std::optional<Price> mid_at_withdraw_;
    // The mid of the last book we were shown, which is what the release
    // above is measured against.
    std::optional<Price> last_market_mid_;
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
// The "tapered" / "cent" / "deci" spelling the schema, the CLI and the GUI
// share, from cpm/md/ticks.hpp.
using cpm::md::tick_structure_from;
using cpm::md::to_string;

void register_crypto_mm(StrategyRegistry& registry);

}  // namespace cpm::strategy
