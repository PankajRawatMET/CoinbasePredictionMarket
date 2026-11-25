#pragma once

#include "cpm/strategy/host.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// The operator's surface on the daemon: everything the GUI can ask for or do,
// with no sockets and no FIX in it. The HTTP layer of Stage G3 only translates
// these calls; the tests drive them directly.
namespace cpm::trader {

using cpm::oms::NewOrder;
using cpm::strategy::ParamSchema;
using cpm::strategy::ParamValues;
using cpm::strategy::StrategyHost;
using cpm::strategy::StrategyInfo;
using cpm::strategy::StrategyRegistry;
using cpm::strategy::StrategySpec;
using Clock = cpm::strategy::Clock;

// What the daemon as a whole is doing. Halted is terminal (GUI-Q4): the
// process stays up so the reason can be read, and nothing trades again.
enum class DaemonState { Starting, Live, Halted };

const char* to_string(DaemonState state);

struct SessionView {
    bool logged_on = false;
    // Empty until the first logout or disconnect.
    std::string last_reason;
};

// One rung of a ladder: a price and what is resting on it. Both sides are in
// YES terms, because an operator reading a ladder wants one price axis, not
// two - so the ask rungs carry the implied YES price (Q9) while `venue_price`
// keeps what the venue actually holds, which on that side is a NO bid.
struct LadderLevel {
    Price price = 0;        // YES terms
    Qty qty = 0;
    Price venue_price = 0;  // the YES bid, or the NO bid the ask is implied from
};

// Top of book and the rest of one instrument's state, flattened. Prices stay
// scaled integers all the way to the browser: nothing here is a double.
struct BookView {
    std::string symbol;
    bool valid = false;
    std::optional<Price> best_yes_bid;
    std::optional<Qty> best_yes_bid_qty;
    // 1.000 - the best NO bid: the YES ask this venue implies (Q9).
    std::optional<Price> implied_yes_ask;
    std::optional<Qty> best_no_bid_qty;
    std::optional<Price> mid;
    // Only when both sides are there and the book is not crossed, the same
    // rule the mid follows: one side missing and there is nothing to measure.
    std::optional<Price> spread;
    bool crossed_or_locked = false;
    // The lifecycle status, spelled as cpm::md::to_string spells it. Unknown
    // until the first SecurityStatus arrives (Q13), which is also what a
    // symbol with no instrument at all reads as.
    std::string status{"Unknown"};
    std::optional<Price> last_trade;
    std::optional<Qty> volume;
    std::optional<Qty> open_interest;
    // The whole book, best first on each side, as deep as the subscription
    // asked for. The top-of-book fields above are the first rung of each and
    // stay because most of the GUI only wants those.
    std::vector<LadderLevel> bids;
    std::vector<LadderLevel> asks;
};

struct PositionView {
    std::string symbol;
    Qty net = 0;
    Qty bought = 0;
    Qty sold = 0;
    Amount cash = 0;
    Amount bought_value = 0;
    Amount sold_value = 0;
    Amount fees = 0;
    std::size_t fills = 0;
    std::optional<Amount> settlement_payout;

    // Exiting it, as this daemon would do it right now (G13). The page
    // enables its Exit button from `exit_refusal` and prices the dialog from
    // the rest, rather than joining books, strategies and limits in
    // JavaScript: what the button offers and what exit_position() would
    // actually send have to be one answer, not two that can disagree.
    std::string status{"Unknown"};    // the instrument's lifecycle status
    std::optional<Price> exit_price;  // the touch an exit would cross to
    Qty exit_available = 0;           // what rests at that price, as deep as one rung
    std::string exit_refusal;         // empty exactly while an exit can be sent
};

// One symbol's P&L (GUI-Q15), from cpm::oms::pnl_of. The mark is carried with
// it so the operator can see what the unrealized half was measured against,
// and an absent one is why the unrealized half is absent too.
struct PnlView {
    std::string symbol;
    std::optional<Price> mark;
    Amount realized = 0;
    std::optional<Amount> unrealized;
    std::optional<Amount> total;
};

// The daemon's own line. Realized always adds up; unrealized is null as soon
// as one open position has no mark, because a sum that quietly leaves one out
// reads as a smaller number rather than an incomplete one.
struct PnlTotals {
    Amount realized = 0;
    std::optional<Amount> unrealized;
    std::optional<Amount> total;
};

// The client-side limits in force, which are shared by every strategy because
// they belong to the session (GUI-Q10).
struct LimitsView {
    bool allow_orders = false;
    Qty max_quantity = 0;
    Amount max_notional = 0;
    double max_orders_per_sec = 0;
};

// Everything the page shows, in one consistent read. The GUI gets one of these
// on connect and after every reconnect, so a browser arriving mid-run needs no
// history (design-gui.md section 7).
struct Snapshot {
    DaemonState state = DaemonState::Starting;
    // The daemon-wide key: false unless it was started with --allow-orders.
    bool allow_orders = false;
    std::string halt_reason;
    SessionView market_data;
    SessionView order_entry;
    std::string recovery;  // the Reconciler's RecoveryState
    // The reference venue, for the pricing speed bump. "off" when no feed
    // was configured, which is the daemon as it was before B3.
    std::string reference{"off"};
    std::vector<StrategyInfo> strategies;
    std::vector<BookView> books;
    std::vector<PositionView> positions;
    std::vector<cpm::oms::ExecutionReport> fills;
    // One row per position, in the same order, and the daemon's total.
    std::vector<PnlView> pnl;
    PnlTotals pnl_total;
    LimitsView limits;
    Clock::time_point taken{};
    // Bumped by every action that changes anything, so the stream layer can
    // tell a stale snapshot from a current one.
    std::uint64_t revision = 0;
};

// An instrument's whole price axis: every price the grid allows, ascending.
// It is static for the life of the instrument, so the GUI asks for it once per
// symbol rather than carrying it in every snapshot - 281 rungs twice a second
// is a lot of bytes to spend saying the same thing.
struct AxisView {
    std::string symbol;
    std::string structure;  // "tapered", "cent", "deci"
    std::vector<Price> prices;
};

// One registered strategy kind, with the schema a form is built from.
struct KindView {
    std::string name;
    std::string help;
    ParamSchema schema;
};

// What an action did. Never throws for operator input: a refusal is a value,
// because it has to cross an HTTP boundary intact.
struct Result {
    std::string error;
    std::string id;  // set by create()

    bool ok() const { return error.empty(); }
};

class ControlApi {
public:
    // What the API needs from the daemon around it. In cpm_trader these read
    // the live sessions; in the tests they are fakes, which is why no part of
    // this class mentions QuickFIX.
    struct Env {
        std::function<SessionView()> market_data;
        std::function<SessionView()> order_entry;
        std::function<std::string()> recovery;
        // One word for the header: off, up, stale or down.
        std::function<std::string()> reference;
        std::function<std::vector<PositionView>()> positions;
        std::function<std::vector<cpm::oms::ExecutionReport>()> fills;
        std::function<std::string(const std::string&)> recover_fills;
        // The P&L of each position, marked where a mid can be had. Computed
        // by the daemon because the mark comes from the book, which this
        // class does not read.
        std::function<std::vector<PnlView>()> pnl;
        std::function<std::optional<BookView>(const std::string& symbol)> book;
        std::function<LimitsView()> limits;
        // The price grid an instrument trades on. Configured rather than
        // discovered: it is not on the FIX feed (O5).
        std::function<AxisView(const std::string& symbol)> axis;
        // Turns a series into the instrument of it that is trading now. The
        // daemon answers this over REST, because FIX cannot (R-Q2). Returns
        // the ticker, or the reason there is none.
        struct Resolved {
            std::string symbol;
            std::string error;
        };
        std::function<Resolved(const std::string& series)> resolve;
        // What the lifecycle feed says is open for a series now, which is
        // what a roll follows once the daemon is running.
        std::function<std::optional<std::string>(const std::string& series)> current_window;
        // Opens and closes the market data subscriptions a strategy needs.
        // subscribe() returns an error string, so a strategy is never created
        // on a symbol whose book will never arrive.
        std::function<std::string(const std::string& symbol)> subscribe;
        std::function<void(const std::string& symbol)> unsubscribe;
        // The instruments the daemon watches for its own sake, whether or
        // not a strategy is on them. Their books are in every snapshot, so
        // the page shows a market before anything is quoted.
        std::function<std::vector<std::string>()> watched;

        // --- the operator's own order (G13) ---------------------------
        // May an order go out *right now*? The same answer the strategies
        // get: false while reconciling and after a halt. Unset is
        // permitted, as everywhere else here.
        std::function<bool()> can_submit;
        // The next ClOrdID for an order that belongs to no strategy. It
        // comes from the daemon's one counter, because that counter is what
        // the bookmark persists (OE-Q7).
        std::function<std::string()> next_cl_ord_id;
        // Sends one order outside every strategy. Returns the reason it
        // could not be sent, or empty. It returns rather than throws
        // because the answer has to cross an HTTP boundary intact, and it
        // is the daemon's job to record and attribute it.
        std::function<std::string(const NewOrder& order)> send_order;
        // Everything of ours still resting on one instrument, as the order
        // manager holds it. An exit must not cross our own order: the venue
        // answers a self-match by cancelling one of the two, so the exit
        // could simply vanish.
        std::function<std::vector<cpm::strategy::LiveLeg>(const std::string& symbol)>
            resting_orders;
    };

    ControlApi(StrategyHost& host, const StrategyRegistry& registry, Env env, bool allow_orders);

    std::vector<KindView> kinds() const;

    // Creates the strategy and subscribes its symbols. If the subscription
    // fails the strategy is removed again, so a half-built one is never left
    // in the list.
    Result create(const StrategySpec& spec);
    // Retune a strategy without moving it: the host refuses unless it is
    // disarmed, so a quoting strategy's parameters cannot change under the
    // confirmation that armed it (GUI-Q30). No subscription moves, because
    // the symbol is the one value an edit may not change.
    Result update(const std::string& id, const ParamValues& params);
    Result start(const std::string& id);
    Result stop(const std::string& id);
    Result arm(const std::string& id);
    Result disarm(const std::string& id);
    // Drops the row's subscription and leaves everything else: the row stays
    // on the page with its kind, its symbol and its parameters, and Start
    // Feed is the only thing left that can be done to it. Only when it is
    // already stopped, for the same reason destroy is - the cancels have to
    // be confirmed before the book they were quoting goes away.
    Result stop_feed(const std::string& id);
    // Subscribes a row whose feed was stopped. A row that follows a series
    // is moved onto the window that is open now, because the one it last
    // quoted has probably closed and subscribing to a closed instrument is
    // answered "unknown symbol", which is a reject, and a reject halts
    // (R-N2, [MD] O6).
    Result start_feed(const std::string& id);
    // Stops, unsubscribes and forgets it. Only when it is already stopped.
    Result destroy(const std::string& id);
    // The kill switch: stops and disarms everything.
    Result stop_all();
    // Flattens part or all of one position (G13): one IOC limit order
    // crossing to the touch, sent outside every strategy, so a position
    // outlives the row that opened it and can still be closed.
    //
    // `qty` is in contracts at kQtyScale; 0 means the whole position. More
    // than is held is refused rather than clamped - the operator typed a
    // number, and a different one reaching the venue is a surprise.
    //
    // IOC rather than GTC because a resting exit would have to be cancelled
    // later and a cancel reject halts the run (OE-Q6); a limit at the touch
    // rather than a market order because a market order is forced to the
    // worst admissible price, which on a 0-1 grid is the whole position.
    // What it does not fill is cancelled, and the operator presses again.
    Result exit_position(const std::string& symbol, Qty qty);
    // Move a strategy to the next window of its series: the subscriptions
    // follow, then the strategy. Driven by the daemon's own tick rather than
    // by an operator, so it bumps the revision like anything else that
    // changes what the page shows.
    Result roll(const std::string& id, const std::string& symbol);

    Snapshot snapshot() const;
    // Empty prices for a symbol no strategy has claimed: the daemon answers
    // for the instruments it is actually watching.
    AxisView axis(const std::string& symbol) const;
    std::uint64_t revision() const;

private:
    // What flattening one position would do, decided in one place so that
    // the button's state and the order that goes out cannot disagree. The
    // order carries no ClOrdID: the plan is computed for every snapshot, and
    // allocating an id twice a second would burn the counter the bookmark
    // persists. exit_position() stamps it just before sending.
    struct ExitPlan {
        NewOrder order;
        Qty available = 0;    // resting at the price it would cross
        std::string refusal;  // empty exactly when `order` may be sent
    };
    ExitPlan plan_exit(const std::string& symbol, Qty qty) const;

    void bump();
    // Unsubscribes those of `symbols` that no other row is still reading.
    // Unsubscribing is not ref-counted, and a stopped row and a live one can
    // be on the same instrument, so dropping one row's feed must not take
    // another's book with it.
    void drop_feed(const std::string& id, const std::vector<std::string>& symbols);

    StrategyHost& host_;
    const StrategyRegistry& registry_;
    const Env env_;
    const bool allow_orders_;
    mutable std::mutex mutex_;
    std::uint64_t revision_ = 0;
};

}  // namespace cpm::trader
