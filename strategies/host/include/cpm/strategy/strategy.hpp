#pragma once

#include "cpm/binance/reference.hpp"
#include "cpm/md/market_state.hpp"
#include "cpm/oms/order_state.hpp"
#include "cpm/oms/orders.hpp"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

// What the host needs from a strategy, and what a strategy reports back. Plain
// structs with no QuickFIX and no FIX session in them, the same division the
// rest of the project keeps: a strategy sees decoded books and reports, and
// reaches the venue only through hooks.
namespace cpm::strategy {

using cpm::md::InstrumentView;
using cpm::oms::CancelOrder;
using cpm::oms::ExecutionReport;
using cpm::oms::NewOrder;
using cpm::oms::Position;
using Clock = cpm::oms::Clock;

// The "[id symbol] " every line of strategy.log carries, so that one file
// holding every series can still be read one instrument at a time
// (docs/design-logs.md decision 4).
//
// Here rather than in either caller because there are two: the host puts it
// on a strategy's own commentary, and the daemon puts it on the order
// lifecycle it observes from outside - and a tag written two ways would
// defeat the grep it exists for.
//
// Built from the arguments at the moment of the call, never frozen: a roll
// gives a row a new window and the tag has to follow it. One symbol is the
// shape there is - one kind, and a symbol is claimed by one strategy
// (GUI-Q6) - so [id symbol] identifies a line completely; a kind quoting two
// legs gets [id] alone and says the leg itself.
inline std::string log_tag(const std::string& id, const std::vector<std::string>& symbols) {
    std::string out = "[" + id;
    if (symbols.size() == 1) out += " " + symbols.front();
    return out + "] ";
}

// The same tag for one symbol that is already known, which is the shape the
// daemon has: it reads a report, not a row.
inline std::string log_tag(const std::string& id, const std::string& symbol) {
    if (id.empty()) return symbol.empty() ? std::string() : "[" + symbol + "] ";
    return symbol.empty() ? "[" + id + "] " : "[" + id + " " + symbol + "] ";
}

// One order a strategy currently has in the market, flattened for display.
// Prices and quantities stay scaled integers all the way to the browser.
struct LiveLeg {
    cpm::oms::Side side = cpm::oms::Side::Buy;
    std::string cl_ord_id;
    Price price = 0;
    Qty quantity = 0;
    bool cancel_pending = false;
};

// Everything a strategy may do to the outside world. The host composes these
// so that arming, the submission gate and the halt are all enforced below the
// strategy rather than by it: a strategy that ignored can_submit() still could
// not send, because the client refuses too.
struct StrategyHooks {
    // May an order go out *right now*? False while reconciling, while
    // disarmed, or after a halt, so it changes from moment to moment.
    std::function<bool()> can_submit;
    // May this daemon ever trade? The --allow-orders key, fixed for the life
    // of the run. Arming consults this rather than can_submit: arming is the
    // operator's intent, and reconciliation is not a reason to refuse an
    // intent - it is a reason not to act on it yet, which can_submit covers.
    // Unset means permitted, as for can_submit.
    std::function<bool()> orders_permitted;
    std::function<std::string(const NewOrder&)> send;  // returns the ClOrdID
    std::function<bool(const CancelOrder&)> cancel;
    // Has this order already finished at the venue - filled, cancelled,
    // expired or rejected? The order manager knows this the moment the report
    // is decoded, while a strategy's own view of its quotes waits for the
    // next tick, so the two disagree for as long as a report sits in the
    // queue. Cancelling an order that has finished is answered with
    // UNKNOWN_ORDER, and a cancel reject halts the run, so a strategy asks
    // before it cancels. Unset means "not known", which keeps the old
    // behaviour: send the cancel.
    std::function<bool(const std::string& cl_ord_id)> order_finished;
    // Everything of ours resting on one instrument, as the order manager
    // holds it: the price each order actually rests at, and what is still
    // open on it. A strategy leans on what OTHER people are showing, so it
    // takes its own size back out of the book before reading it - our own
    // order on a level is not information about the market, and a level that
    // is only ours is not a market at all. Unset means "not known", and the
    // book is then read as it comes.
    std::function<std::vector<LiveLeg>(const std::string& symbol)> resting_orders;
    std::function<std::string()> next_cl_ord_id;
    // Which strategy an order or a cancel came from, for the record files:
    // the ClOrdID does not say, because the counter behind it is the
    // daemon's and not a strategy's. The host calls this just before the
    // send it wraps, so a strategy neither knows nor can lie about its own
    // id. Unset is "do not attribute", which is every test that does not
    // care and every run with no record files.
    std::function<void(const std::string& cl_ord_id, const std::string& strategy_id)> attribute;
    // A strategy asking to be stopped, with the reason the operator will
    // read on its row. It is a request, not a call into the host: the host
    // is driving this pass and holds its lock, so the stop happens as soon
    // as the pass returns - the quotes come off, the row disarms and goes to
    // Stopping, and nothing of this strategy is driven again.
    //
    // Unset means the strategy has no owner to ask, which is the one-shot
    // crypto_mm binary; it withdraws instead and keeps running.
    std::function<void(const std::string& why)> stop_me;
    // This strategy's own commentary: what the market looked like and what
    // it decided, which is strategy.log and not the operator's panel. Write
    // the line and nothing about who is writing it - the host prefixes
    // `[<id> <symbol>]` on the way past, so a file holding two series can
    // be read a strategy at a time (docs/design-logs.md section 3). Unset
    // discards, which is the one-shot crypto_mm binary and every test that
    // does not care.
    std::function<void(const std::string&)> log;
};

// What a strategy says about itself. Deliberately flat and kind-agnostic: the
// host copies it out and the GUI renders it without knowing what produced it,
// which is what lets a second strategy kind be added without touching either.
struct StrategyStatus {
    std::vector<LiveLeg> legs;
    // Why it is not acting, for the operator. Empty while it is.
    std::string reason;
    std::size_t passes = 0;
    std::size_t passes_without_target = 0;
    std::size_t placed = 0;
    std::size_t cancelled = 0;
    std::size_t fills = 0;
    // Optional model diagnostics, rendered generically by the operator UI.
    std::map<std::string, std::string> diagnostics;
};

// A strategy is whatever satisfies this. The host owns the clock, the books,
// the positions and the fan-out of reports; the strategy owns the decision.
class IStrategy {
public:
    virtual ~IStrategy() = default;

    // One pass over one of the strategy's symbols. Called from the host loop,
    // never from a QuickFIX thread.
    virtual void on_market(const InstrumentView& book, const Position& position,
                           Clock::time_point now) = 0;
    // The reference venue moved. Called on every update from it, before
    // the pass above and WHETHER OR NOT this strategy has a book - a
    // strategy with orders resting on a market that has gone quiet is
    // exactly the one that needs to hear this (B-Q5).
    //
    // Virtual with a default rather than pure: a strategy that does not
    // care about a reference price should not have to say so.
    virtual void on_reference(const cpm::binance::ReferenceView&, Clock::time_point) {}

    // Every report the session decodes, including unsolicited ones. A
    // strategy ignores the ones whose ClOrdID is not its own: the host does
    // not filter, because a report for a deleted instrument carries
    // "deleted_instrument" as its symbol rather than the order's.
    virtual void on_report(const ExecutionReport& report) = 0;
    // Cancel everything and stop acting until the next on_market().
    virtual void withdraw(const std::string& why) = 0;
    // Forget everything and stop acting, WITHOUT sending a cancel: the
    // instrument has closed, and the venue has already cancelled whatever was
    // resting on it - an unsolicited Canceled carrying
    // ExecRestatementReason=2, "instrument lifecycle" (OE spec).
    //
    // The distinction is not tidiness. A cancel that reaches the gateway after
    // the lifecycle cancel has terminated the order comes back as
    // OrderCancelReject / UNKNOWN_ORDER, and any cancel reject halts the
    // daemon (OE-Q6). So a closing window is the one case where the right
    // move is to send nothing at all.
    virtual void abandon(const std::string& why) = 0;

    // True once nothing of this strategy's is left at the venue, which is what
    // the host waits for before reporting a strategy stopped.
    virtual bool quiescent() const = 0;
    // The symbols it wants books and positions for. Fixed for its lifetime:
    // the host refuses an overlap between strategies at create time (GUI-Q6),
    // which it could not do if this changed underneath.
    virtual std::vector<std::string> symbols() const = 0;
    // Model makers need a valid snapshot, but not a continuously moving book.
    // Session disconnects, gaps and rejects still halt.
    virtual bool requires_book_updates() const { return true; }
    virtual StrategyStatus status() const = 0;
};

}  // namespace cpm::strategy
