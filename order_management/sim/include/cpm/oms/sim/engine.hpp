#pragma once

#include "cpm/oms/orders.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

// A matching engine for oe_sim: plain C++, no QuickFIX. The FIX layer turns the
// events returned here into ExecutionReport(8), OrderCancelReject(9) and
// OrderMassCancelReport(r).
//
// Unlike the market data book, order entry is one-sided in price terms: Side(54)
// Buy and Sell both refer to the YES contract, so this is an ordinary two-sided
// book of YES bids and YES asks that crosses when bid >= ask.
namespace cpm::oms::sim {

struct Instrument {
    std::string symbol;
    bool active = true;
    // Orders on an instrument with a speed bump are accepted as PendingNew and
    // become matchable only when release() is called for them.
    std::chrono::milliseconds speed_bump{0};
};

// The exchange's fee schedule is not in the spec, so this is an assumption:
// basis points of the fill's notional, taker positive and maker negative.
struct Fees {
    int taker_bps = 100;  // 1.00% of notional
    int maker_bps = -50;  // 0.50% rebate
};

// One request's outcome, shaped like the report it becomes.
struct Event {
    enum class Kind {
        PendingNew,       // 150=A: held for the speed bump
        New,              // 150=0: resting
        Fill,             // 150=F
        Canceled,         // 150=4
        Expired,          // 150=C: an IOC remainder or an unfillable FOK
        Rejected,         // 150=8, with OrdRejReason
        CancelRejected,   // OrderCancelReject(9), with CxlRejReason
        BusinessRejected, // BusinessMessageReject(j): no report follows
    };

    Kind kind = Kind::New;
    // Whose order this report is about. A session is only told about its own:
    // both sides of a trade are reported, but each to its owner.
    std::string owner;
    std::string cl_ord_id;       // the triggering request
    std::string orig_cl_ord_id;  // the original order, on a cancel response
    std::string order_id;
    std::string symbol;
    std::string subaccount;  // echoed in the report's Parties group
    Side side = Side::Buy;
    Qty order_quantity = 0;
    Qty leaves = 0;
    Qty cumulative = 0;
    Price price = 0;  // the order's limit price, resolved for a market order
    std::optional<OrderType> type;
    std::optional<TimeInForce> time_in_force;
    PostOnly post_only = PostOnly::None;
    Price average_price = 0;

    // Fill only.
    Qty last_quantity = 0;
    Price last_price = 0;
    std::string trade_match_id;
    bool aggressor = false;
    Amount fee = 0;

    int reject_reason = 0;         // OrdRejReason(103) on Rejected
    int cancel_reject_reason = 0;  // CxlRejReason(102) on CancelRejected
    std::string text;              // the reason label
    Clock::time_point time{};
};

struct RestingOrder {
    std::string order_id;
    std::string cl_ord_id;
    // Whose order it is. A session may only cancel its own, which is what keeps
    // a mass cancel from touching the simulator's seeded liquidity.
    std::string owner;
    std::string subaccount;
    std::string symbol;
    Side side = Side::Buy;
    Price price = 0;
    Qty quantity = 0;
    Qty leaves = 0;
    Qty cumulative = 0;
    // Sum of quantity x price over the fills so far, for the average price.
    std::int64_t filled_value = 0;
    TimeInForce time_in_force = TimeInForce::Gtc;
    PostOnly post_only = PostOnly::None;
    OrderType type = OrderType::Limit;
    bool matchable = true;  // false while held by a speed bump
    Clock::time_point accepted_at{};
};

class Engine {
public:
    Engine(std::vector<Instrument> instruments, Fees fees);

    // NewOrderSingle(D). Returns the reports in the order the gateway sends
    // them: the aggressor's fills first, then its terminal state.
    std::vector<Event> submit(const NewOrder& order, const std::string& subaccount,
                              Clock::time_point now, const std::string& owner = "client");
    // OrderCancelRequest(F): resolves by OrderID first, then OrigClOrdID.
    std::vector<Event> cancel(const CancelOrder& request, Clock::time_point now,
                              const std::string& owner = "client");
    // OrderMassCancelRequest(q). Filters AND together. The individual cancels
    // come back here; the summary report is the FIX layer's business.
    std::vector<Event> mass_cancel(const MassCancel& request, const std::string& subaccount,
                                   Clock::time_point now, const std::string& owner = "client");
    // Makes speed-bumped orders matchable once their delay has passed, and
    // matches them. Returns whatever that produced.
    std::vector<Event> release_speed_bumped(Clock::time_point now);

    // Resting orders on one side, best price first then oldest first.
    std::vector<RestingOrder> book(const std::string& symbol, Side side) const;
    const Instrument* find(const std::string& symbol) const;
    std::size_t open_orders() const { return orders_.size(); }

private:
    struct Key {
        // Best first: highest price for a bid, lowest for an ask, then oldest.
        Price price;
        std::uint64_t sequence;
    };

    // Resolves within one owner's orders only.
    RestingOrder* resolve(const std::string& order_id, const std::string& cl_ord_id,
                          const std::string& owner);
    std::string next_order_id();
    std::string next_trade_id();
    // Fills `taker` against the opposite side as far as price allows, appending
    // the taker's fill events and the makers' to `out`.
    Qty match(RestingOrder& taker, std::vector<Event>& out, Clock::time_point now);
    // The quantity available to a taker at or better than its limit, and its
    // all-in cost, without changing anything: FOK and MaxExecutionCost both
    // need this before any matching happens.
    std::pair<Qty, Amount> available(const RestingOrder& taker) const;
    Event event_for(const RestingOrder& order, Event::Kind kind, Clock::time_point now) const;
    void rest(RestingOrder order);
    void remove(const std::string& order_id);
    // The price a market order is forced to, and the post-only slide target.
    Price worst_admissible(Side side) const;
    std::optional<Price> slide_price(const RestingOrder& order) const;

    std::vector<Instrument> instruments_;
    Fees fees_;
    // order id -> order, plus a price-time index per (symbol, side).
    std::map<std::string, RestingOrder> orders_;
    std::map<std::pair<std::string, Side>, std::multimap<std::int64_t, std::string>> levels_;
    std::uint64_t next_order_ = 240418888163000320ULL;  // shaped like the sandbox's (OE-O2)
    std::uint64_t next_trade_ = 3391027ULL;
    std::uint64_t sequence_ = 0;
};

}  // namespace cpm::oms::sim
