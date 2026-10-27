#pragma once

#include "cpm/common/fixed.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <vector>

// Orders and their reports as plain structs with no QuickFIX in them, the same
// division as cpm/md/events.hpp. Prices are thousandths, quantities
// hundredths, amounts millionths; see cpm/common/fixed.hpp.
namespace cpm::oms {

using Clock = std::chrono::system_clock;

// Side(54). Both values refer to the YES contract: there is no NO-side order
// entry on FIX, so BUY_NO at P is submitted as SELL YES at 1.000 - P (OE-Q14).
enum class Side { Buy, Sell };

// OrdType(40). A market order is forced to the worst admissible price
// (BUY 0.999 / SELL 0.001) and converted to IOC by the gateway, and must carry
// no price.
enum class OrderType { Market, Limit };

// TimeInForce(59).
enum class TimeInForce { Gtc, Ioc, Fok };

// ExecInst(18) post-only modes. Mutually exclusive with a market order.
enum class PostOnly {
    None,
    Slide,   // 6: adjust the price to avoid crossing
    Reject,  // A: refuse the order if it would cross
};

// SelfMatchPreventionStrategy(8000).
enum class SmpStrategy { Unset, CancelAggressing, CancelResting, CancelBoth };

// ExecType(150), the reason a report was emitted.
enum class ExecType {
    PendingNew,  // A: held for an instrument's speed bump, not yet matchable
    New,         // 0: resting on the book
    Canceled,    // 4: user request, mass cancel, SMP, admin, lifecycle or COD
    Rejected,    // 8: carries OrdRejReason
    Trade,       // F: a partial or full fill
    Expired,     // C: an IOC remainder, or an FOK that could not fill
    Replaced,    // 5: declared by the spec but never emitted
    PendingCancel,  // 6: likewise
    Unknown,
};

// OrdStatus(39). Undefined is what OrderCancelReject(9) always carries.
enum class OrdStatus {
    New, PartiallyFilled, Filled, Canceled, Rejected, PendingNew, PendingCancel, Expired,
    Undefined, Unknown,
};

// ExecRestatementReason(378), present on unsolicited cancels that have one.
enum class CancelReason {
    None,
    CancelOnDisconnect,   // 1
    InstrumentLifecycle,  // 2: closed, determined or paused
    SelfMatchPrevention,  // 7
    AdminCancel,          // 105
    Unknown,
};

const char* to_string(Side side);
const char* to_string(OrderType type);
const char* to_string(TimeInForce tif);
const char* to_string(PostOnly post_only);
const char* to_string(ExecType type);
const char* to_string(OrdStatus status);
const char* to_string(CancelReason reason);

// --- client -> server -----------------------------------------------------

// NewOrderSingle(D). ManualOrderIndicator and the sub-account come from the
// session configuration, not from here: they are the same for every order.
struct NewOrder {
    std::string cl_ord_id;  // up to 16 printable ASCII, unique among live orders
    std::string symbol;
    Side side = Side::Buy;
    Qty quantity = 0;
    OrderType type = OrderType::Limit;
    Price price = 0;                              // limit only, 0.001-0.999
    TimeInForce time_in_force = TimeInForce::Gtc;
    PostOnly post_only = PostOnly::None;
    // MaxExecutionCost(21009): FOK only, and must be positive.
    std::optional<Amount> max_execution_cost;
    std::optional<int> smp_id;                    // 7928, must match the registered id
    SmpStrategy smp_strategy = SmpStrategy::Unset;  // 8000
};

// OrderCancelRequest(F). Either the original ClOrdID or the exchange OrderID
// is required; OrderID wins when both are given.
struct CancelOrder {
    std::string cl_ord_id;  // a NEW id for the cancel itself
    std::string orig_cl_ord_id;
    std::string order_id;
    std::string symbol;
    Side side = Side::Buy;
};

// OrderMassCancelRequest(q). Scopes 6 and 7 are equivalent, since an API key
// has one active session.
struct MassCancel {
    enum class Scope { Security, TradingSession, All };

    std::string cl_ord_id;
    Scope scope = Scope::All;
    std::string symbol;              // required for Scope::Security
    std::optional<Side> side;        // optional filter
    bool filter_by_subaccount = false;
};

// --- server -> client -----------------------------------------------------

struct Fill {
    Qty quantity = 0;
    Price price = 0;
    std::string trade_match_id;  // 880
    bool aggressor = false;      // 1057: Y taker, N maker
    // MiscFeeAmt(137): positive is a fee, negative a maker rebate.
    std::optional<Amount> fee;
};

// ExecutionReport(8).
struct ExecutionReport {
    std::string cl_ord_id;
    std::string orig_cl_ord_id;  // set on a cancel response
    std::string order_id;        // opaque 64-bit text
    std::string exec_id;         // opaque, monotonically increasing; the replay handle
    ExecType exec_type = ExecType::Unknown;
    OrdStatus status = OrdStatus::Unknown;
    std::string symbol;  // "deleted_instrument" if the instrument has gone
    Side side = Side::Buy;
    std::optional<Qty> order_quantity;
    std::optional<Price> price;
    Qty leaves_quantity = 0;
    Qty cumulative_quantity = 0;
    std::optional<Price> average_price;
    std::optional<Fill> fill;  // set when exec_type == Trade
    std::optional<TimeInForce> time_in_force;
    PostOnly post_only = PostOnly::None;
    std::optional<OrderType> type;
    bool manual_order = false;
    CancelReason cancel_reason = CancelReason::None;
    std::optional<int> reject_reason;  // OrdRejReason(103), on Rejected only
    std::string text;                  // a reason label; not a stable contract
    std::optional<int> smp_id;
    SmpStrategy smp_strategy = SmpStrategy::Unset;
    Clock::time_point transact_time{};
    bool replayed = false;  // PossResend(97)=Y: an Event Resend copy, not live
};

// OrderCancelReject(9).
struct CancelReject {
    std::string cl_ord_id;
    std::string orig_cl_ord_id;
    std::string order_id;  // "0" when the order is unknown
    // CxlRejReason(102): 0 too late (already filled), 1 unknown order,
    // 3 a cancel for this order is already pending.
    std::optional<int> reason;
    std::string text;  // UNKNOWN_ORDER, ORDER_ALREADY_FILLED, CANCEL_ALREADY_PENDING
    bool replayed = false;
};

// OrderMassCancelReport(r). TotalAffectedOrders is the authoritative signal:
// the echoed scope and the response code do not say what actually ran.
struct MassCancelReport {
    std::string cl_ord_id;
    std::string report_id;  // 0 when rejected on receipt
    MassCancel::Scope requested_scope = MassCancel::Scope::All;
    bool accepted = false;
    std::optional<int> reject_reason;  // 532
    std::optional<int> affected_orders;
    std::string text;
    Clock::time_point transact_time{};
    bool replayed = false;
};

// MarketSettlementReport(UMS), per (firm, instrument).
struct SettlementReport {
    enum class Outcome { Yes, No, Scalar, Unknown };

    std::string report_id;
    std::string symbol;
    std::string clearing_firm;
    std::string executing_firm;
    Outcome outcome = Outcome::Unknown;
    Price settlement_price = 0;
    bool net_aggregation = true;  // 21053: N net, G gross
    Qty long_quantity = 0;
    Qty short_quantity = 0;
    // Gross payout, never negative, and not profit: it does not subtract what
    // was paid to open. Consistency: payout = price*long + (1-price)*short.
    Amount payout = 0;
    Clock::time_point transact_time{};
    bool replayed = false;
};

// LastExecID(F2): the newest ExecID the exchange holds, or "0" if the
// retention window holds nothing for this user.
struct LastExecId {
    std::string ref_seq_num;
    std::string exec_id;
};

// EventResendComplete(F4) and EventResendReject(F5).
struct ResendComplete {
    std::string ref_seq_num;
    int event_count = 0;
};

struct ResendReject {
    std::string ref_seq_num;
    int reason = 0;  // 22006: 1 begin too small, 2 end too large, 3 in progress, 4 too many, 5 server
    std::string text;
};

// BusinessMessageReject(j): the synchronous rejection path, which carries no
// ExecutionReport at all. Terminal for the ClOrdID in business_reject_ref_id.
struct BusinessReject {
    std::string ref_seq_num;
    std::string ref_msg_type;
    std::string business_reject_ref_id;  // the ClOrdID, when the gateway sets it
    int reason = 0;                      // 380: 0 other, 2 unknown id
    std::string text;
};

// A message that could not be decoded. Reported, never thrown: decoding runs
// on a QuickFIX callback thread.
struct DecodeError {
    std::string reason;
    std::string msg_type;
    std::string cl_ord_id;
    std::string raw;  // SOH shown as '|', API key redacted
};

}  // namespace cpm::oms
