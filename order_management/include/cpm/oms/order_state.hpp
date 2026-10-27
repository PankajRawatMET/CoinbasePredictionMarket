#pragma once

#include "cpm/oms/oe_client.hpp"
#include "cpm/oms/orders.hpp"

#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace cpm::oms {

// Something in the order flow that contradicts the gateway's own rules or our
// record of it. Reported, never thrown: the reports are the venue's.
struct OrderAnomaly {
    enum class Kind {
        UnknownOrder,       // a report for a ClOrdID we never sent
        AfterTerminal,      // a further report for an order already finished
        QuantityMismatch,   // CumQty + LeavesQty disagrees with OrderQty
        MissingFill,        // ExecType=F with no LastQty/LastPx
        StatusRegression,   // Filled or Canceled going back to New
        OrderIdChanged,     // a second OrderID for one order
        PayoutMismatch,     // a settlement whose payout identity does not hold
        PositionMismatch,   // settlement quantities disagree with our fills
    };

    Kind kind;
    std::string cl_ord_id;
    std::string symbol;
    std::string detail;
};

const char* to_string(OrderAnomaly::Kind kind);

// One order's life, as the reports describe it.
struct OrderRecord {
    std::string cl_ord_id;
    std::string order_id;
    std::string symbol;
    Side side = Side::Buy;
    std::optional<OrderType> type;
    std::optional<TimeInForce> time_in_force;
    PostOnly post_only = PostOnly::None;
    Qty order_quantity = 0;
    Price price = 0;

    OrdStatus status = OrdStatus::Unknown;
    Qty leaves_quantity = 0;
    Qty cumulative_quantity = 0;
    Price average_price = 0;
    // Sum of quantity x price over the fills, for our own average.
    std::int64_t filled_value = 0;
    Amount fees = 0;  // positive is paid away, negative is a rebate received
    std::size_t fills = 0;

    bool terminal = false;      // Filled, Canceled, Expired or Rejected
    bool cancel_pending = false;
    std::string pending_cancel_id;
    std::optional<int> reject_reason;
    CancelReason cancel_reason = CancelReason::None;
    std::string text;
    std::string last_exec_id;
    Clock::time_point created{};
    Clock::time_point updated{};
};

// A position in one instrument, derived from fills because the venue publishes
// none until settlement (OE-Q11). Signed in YES contracts: buys add, sells
// subtract, so a negative net is short YES.
struct Position {
    std::string symbol;
    Qty net = 0;
    Qty bought = 0;
    Qty sold = 0;
    // Signed cash: what was paid out buying, less what was received selling,
    // plus fees. Not a P&L: settlement pays the rest.
    Amount cash = 0;
    Amount fees = 0;
    std::size_t fills = 0;
    // Set once a MarketSettlementReport arrives for it (Stage 6).
    std::optional<Amount> settlement_payout;
    std::optional<Price> settlement_price;
};

// Every order and position this session knows about, built from decoded
// reports. Plain C++: no QuickFIX, no session.
//
// Thread safety: apply() runs on the QuickFIX callback thread while readers run
// elsewhere, so everything is guarded and reads return copies. Anomaly handlers
// are called after the lock is released.
class OrderState {
public:
    using AnomalyHandler = std::function<void(const OrderAnomaly&)>;

    void on_anomaly(AnomalyHandler handler);
    // Callbacks to hand to OeClient::on_events.
    OeCallbacks callbacks();

    void apply(const ExecutionReport& report);
    void apply(const CancelReject& reject);
    void apply(const BusinessReject& reject);
    void apply(const MassCancelReport& report);
    void apply(const SettlementReport& report);

    // Records an order at the moment it is sent, so a report that arrives
    // before any New - or a BusinessMessageReject, which carries no report at
    // all - can still be attributed to it.
    void record_sent(const NewOrder& order);
    void record_cancel_sent(const CancelOrder& cancel);

    std::optional<OrderRecord> order(const std::string& cl_ord_id) const;
    std::vector<OrderRecord> orders() const;
    // Orders that are not terminal, i.e. what is still live at the venue.
    std::vector<OrderRecord> open_orders() const;
    std::optional<Position> position(const std::string& symbol) const;
    std::vector<Position> positions() const;

    // The newest ExecID applied, which is the bookmark Stage 5 persists.
    std::string last_exec_id() const;
    // Reports whose ExecID had already been applied, i.e. Event Resend copies.
    std::size_t duplicate_reports() const;
    std::size_t anomalies() const;

private:
    // Resolves the order a report belongs to: OrigClOrdID when set, because a
    // cancel's report names the cancel in ClOrdID (OE-O2).
    OrderRecord* find(const ExecutionReport& report);
    void note(OrderAnomaly::Kind kind, const std::string& cl_ord_id, const std::string& symbol,
              std::string detail);
    void apply_fill(OrderRecord& record, const ExecutionReport& report);
    void dispatch();

    mutable std::mutex mutex_;
    std::map<std::string, OrderRecord> orders_;
    // Cancel ClOrdID -> the order it was sent for, so a CancelReject can be
    // attributed without waiting for a report.
    std::map<std::string, std::string> cancels_;
    std::map<std::string, Position> positions_;
    std::unordered_set<std::string> seen_exec_ids_;
    std::string last_exec_id_;
    std::size_t duplicate_reports_ = 0;
    std::size_t anomalies_ = 0;
    std::vector<OrderAnomaly> pending_;
    AnomalyHandler on_anomaly_;
};

}  // namespace cpm::oms
