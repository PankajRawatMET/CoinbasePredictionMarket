#pragma once

#include "cpm/oms/oe_client.hpp"
#include "cpm/oms/orders.hpp"

#include <chrono>
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
        FillMismatch,       // two reports disagree on the same trade and order
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

    // Latency, for the record files (docs/design-logs.md section 3). `sent`
    // is the instant this order went to the wire and `rested` the instant it
    // reached the book, which is where a fill's resting time is measured
    // from. The two latencies are what the venue took to answer the send:
    // `pending_latency` to hold it in an instrument's speed bump, and
    // `ack_latency` to actually add it. On an instrument with no speed bump
    // there is no PendingNew at all and only the second is set (OE-O3).
    //
    // All of them stay empty for an order this session did not send - a
    // replayed report after a restart, or someone else's - because a send
    // stamp from another process means nothing to this one.
    SteadyClock::time_point sent{};
    SteadyClock::time_point rested{};
    std::optional<std::chrono::microseconds> pending_latency;
    std::optional<std::chrono::microseconds> ack_latency;
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
    // The two halves of that cash, kept apart and without the fees in them,
    // because an average cost cannot be recovered from the net. They are what
    // let pnl_of() split a P&L into realized and unrealized.
    Amount bought_value = 0;
    Amount sold_value = 0;
    Amount fees = 0;
    std::size_t fills = 0;
    // Set once a MarketSettlementReport arrives for it (Stage 6).
    std::optional<Amount> settlement_payout;
    std::optional<Price> settlement_price;
};

// A report after it has been applied: the report, the order as it now
// stands, the position the report produced, and how long the venue took to
// answer the request behind it.
//
// Handed to an observer rather than written here, so the record files are
// written from one place and from the state the report produced rather than
// the state before it. Copies, because the observer runs after the lock is
// released.
struct AppliedReport {
    // A CancelReject carries no ExecutionReport, so `report` is then left
    // default and `reject` holds what arrived.
    enum class Kind { Report, CancelReject };

    Kind kind = Kind::Report;
    ExecutionReport report;
    CancelReject reject;
    OrderRecord order;
    // Set on a fill: the position as that fill left it.
    std::optional<Position> position;
    // Of the request this report answers: the order for a PendingNew, a New
    // or a Rejected, and the cancel for a cancel we asked for. Empty when
    // nothing of ours was being answered - an unsolicited cancel, an expiry
    // at the bell, a report replayed from a previous run.
    std::optional<std::chrono::microseconds> latency;
    // New -> this fill: how long the quote rested before it was hit. Not a
    // latency, and named so it cannot be read as one.
    std::optional<std::chrono::microseconds> rested;
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
    using AppliedHandler = std::function<void(const AppliedReport&)>;

    void on_anomaly(AnomalyHandler handler);
    // Every report, after it has been applied, in the order it was applied.
    // Called on the QuickFIX callback thread with the lock released, like an
    // anomaly, which means a handler must not call back into OrderState.
    void on_applied(AppliedHandler handler);
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

    // REST history contains trades, with no FIX order lifecycle or replay cursor.
    std::string recover_fills(const std::string& symbol, const std::vector<ExecutionReport>& fills,
                              Qty venue_net);
    std::vector<ExecutionReport> fills() const;

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
    // A cancel we sent: the order it was for, so a CancelReject can be
    // attributed without waiting for a report, and when it went out, which
    // is what a cancel's latency is measured from.
    struct SentCancel {
        std::string order_cl_ord_id;
        SteadyClock::time_point sent{};
    };
    std::map<std::string, SentCancel> cancels_;
    std::map<std::string, Position> positions_;
    using FillKey = std::pair<std::string, std::string>;
    std::map<FillKey, ExecutionReport> fills_;
    std::vector<ExecutionReport> unidentified_fills_;
    std::unordered_set<std::string> seen_exec_ids_;
    std::string last_exec_id_;
    std::size_t duplicate_reports_ = 0;
    std::size_t anomalies_ = 0;
    std::vector<OrderAnomaly> pending_;
    std::vector<AppliedReport> applied_;
    AnomalyHandler on_anomaly_;
    AppliedHandler on_applied_;
};

}  // namespace cpm::oms
