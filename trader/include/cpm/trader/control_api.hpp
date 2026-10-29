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
    Amount fees = 0;
    std::size_t fills = 0;
    std::optional<Amount> settlement_payout;
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
    // Stops, unsubscribes and forgets it. Only when it is already stopped.
    Result destroy(const std::string& id);
    // The kill switch: stops and disarms everything.
    Result stop_all();
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
    void bump();

    StrategyHost& host_;
    const StrategyRegistry& registry_;
    const Env env_;
    const bool allow_orders_;
    mutable std::mutex mutex_;
    std::uint64_t revision_ = 0;
};

}  // namespace cpm::trader
