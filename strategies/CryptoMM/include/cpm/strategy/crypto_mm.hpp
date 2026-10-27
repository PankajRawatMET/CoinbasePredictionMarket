#pragma once

#include "cpm/md/market_state.hpp"
#include "cpm/oms/order_state.hpp"
#include "cpm/oms/orders.hpp"

#include <functional>
#include <map>
#include <optional>
#include <string>

// CryptoMM: a two-sided quoting strategy. It quotes a bid and an ask a fixed
// number of ticks either side of the mid, each for a fixed quantity, and
// re-quotes when the target moves.
//
// It depends on both containers - the book from market_data, the order path
// from order_management - and on neither of them depending on it.
namespace cpm::strategy {

using cpm::md::InstrumentView;
using cpm::oms::CancelOrder;
using cpm::oms::ExecutionReport;
using cpm::oms::NewOrder;
using cpm::oms::Position;
using Clock = cpm::oms::Clock;

// The tick grid, from the REST PriceLevelStructure values (docs/design.md O5).
// The client cannot read an instrument's structure over FIX, so it is
// configured; TAPERED_DECI_CENT is what the crypto series use.
enum class TickStructure {
    LinearCent,       // 0.01 everywhere
    DeciCent,         // 0.001 everywhere
    TaperedDeciCent,  // 0.001 below 0.100 and above 0.900, 0.01 between
};

// The tick size at a price, in thousandths.
Price tick_at(Price price, TickStructure structure);

struct Params {
    std::string symbol;
    // How far either side of the mid to quote, in ticks (the "x").
    int half_width_ticks = 2;
    // How much to show on each side (the "y"), in hundredths of a contract.
    Qty quantity = 100;
    // Stop adding to a side once the net position reaches this, so the
    // strategy cannot accumulate without limit. 0 disables the check.
    Qty max_inventory = 500;
    TickStructure ticks = TickStructure::TaperedDeciCent;
    // Re-quote only when the target has moved at least this many ticks, so a
    // one-tick flicker does not cause a cancel and replace on every update.
    int requote_threshold_ticks = 1;
};

// One side's target, or nothing when that side should not be quoted.
struct Quote {
    Price price = 0;
    Qty quantity = 0;
};

struct DesiredQuotes {
    std::optional<Quote> bid;
    std::optional<Quote> ask;
    // Why a side is absent, for the log. Empty when both sides are quoted.
    std::string reason;
};

// The whole decision, as a pure function of the book, the position and the
// parameters: no orders, no clock, no state. Everything the strategy decides
// is decided here, so it can be tested by inspection.
//
// The mid comes from both sides of the YES book: the best YES bid and the YES
// ask implied by the best NO bid (1.000 - best NO bid). Without both there is
// no mid and nothing is quoted.
DesiredQuotes desired_quotes(const InstrumentView& book, const Position& position, const Params& params);

// Drives the desired quotes onto the venue: places what is missing, cancels
// what has moved, and leaves alone what is still right. The venue has no
// amend, so a move is a cancel and a re-submit (OE "Not supported").
class QuoteManager {
public:
    // Everything it needs from the outside, so it can be driven by a test.
    struct Hooks {
        // False while reconciling or after a halt: nothing may be submitted.
        std::function<bool()> can_submit;
        std::function<std::string(const NewOrder&)> send;  // returns the ClOrdID
        std::function<bool(const CancelOrder&)> cancel;
        std::function<std::string()> next_cl_ord_id;
        std::function<void(const std::string&)> log;
    };

    QuoteManager(Params params, Hooks hooks);

    // One pass: compare the desired quotes with the live ones and act.
    void on_market(const InstrumentView& book, const Position& position, Clock::time_point now);
    // Keeps track of which quotes are live. Must be fed every report for this
    // instrument, including the unsolicited ones.
    void on_report(const ExecutionReport& report);
    // Cancels both quotes and stops quoting until the next on_market().
    void withdraw(const std::string& why);

    std::optional<Quote> live_bid() const;
    std::optional<Quote> live_ask() const;
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
};

}  // namespace cpm::strategy
