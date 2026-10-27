#include "cpm/oms/order_state.hpp"

#include <algorithm>

namespace cpm::oms {
namespace {

bool is_terminal(OrdStatus status) {
    switch (status) {
        case OrdStatus::Filled:
        case OrdStatus::Canceled:
        case OrdStatus::Expired:
        case OrdStatus::Rejected: return true;
        default: return false;
    }
}

// How far along an order is, so a report that moves it backwards can be
// spotted. Equal ranks are not a regression: a partial fill may repeat.
int rank(OrdStatus status) {
    switch (status) {
        case OrdStatus::PendingNew: return 1;
        case OrdStatus::New: return 2;
        case OrdStatus::PartiallyFilled: return 3;
        case OrdStatus::PendingCancel: return 4;
        case OrdStatus::Filled:
        case OrdStatus::Canceled:
        case OrdStatus::Expired:
        case OrdStatus::Rejected: return 5;
        default: return 0;
    }
}

Amount notional_of(Qty quantity, Price price) { return quantity * price * 10; }

}  // namespace

const char* to_string(OrderAnomaly::Kind kind) {
    switch (kind) {
        case OrderAnomaly::Kind::UnknownOrder: return "report for an unknown order";
        case OrderAnomaly::Kind::AfterTerminal: return "report after a terminal state";
        case OrderAnomaly::Kind::QuantityMismatch: return "CumQty + LeavesQty mismatch";
        case OrderAnomaly::Kind::MissingFill: return "trade report without fill fields";
        case OrderAnomaly::Kind::StatusRegression: return "order status went backwards";
        case OrderAnomaly::Kind::OrderIdChanged: return "OrderID changed";
        case OrderAnomaly::Kind::PayoutMismatch: return "settlement payout identity does not hold";
        case OrderAnomaly::Kind::PositionMismatch: return "settlement disagrees with our fills";
    }
    return "unknown";
}

void OrderState::on_anomaly(AnomalyHandler handler) {
    std::lock_guard lock(mutex_);
    on_anomaly_ = std::move(handler);
}

OeCallbacks OrderState::callbacks() {
    OeCallbacks callbacks;
    callbacks.report = [this](const ExecutionReport& e) { apply(e); };
    callbacks.cancel_reject = [this](const CancelReject& e) { apply(e); };
    callbacks.business_reject = [this](const BusinessReject& e) { apply(e); };
    callbacks.mass_cancel_report = [this](const MassCancelReport& e) { apply(e); };
    callbacks.settlement = [this](const SettlementReport& e) { apply(e); };
    return callbacks;
}

void OrderState::record_sent(const NewOrder& order) {
    {
        std::lock_guard lock(mutex_);
        auto& record = orders_[order.cl_ord_id];
        if (!record.cl_ord_id.empty()) return;  // already known
        record.cl_ord_id = order.cl_ord_id;
        record.symbol = order.symbol;
        record.side = order.side;
        record.type = order.type;
        record.time_in_force = order.time_in_force;
        record.post_only = order.post_only;
        record.order_quantity = order.quantity;
        record.price = order.price;
        record.leaves_quantity = order.quantity;
        record.created = Clock::now();
        record.updated = record.created;
    }
    dispatch();
}

void OrderState::record_cancel_sent(const CancelOrder& cancel) {
    {
        std::lock_guard lock(mutex_);
        // Either identifier may name the order; prefer the ClOrdID we know.
        std::string target = cancel.orig_cl_ord_id;
        if (target.empty()) {
            for (const auto& [id, record] : orders_) {
                if (record.order_id == cancel.order_id) {
                    target = id;
                    break;
                }
            }
        }
        cancels_[cancel.cl_ord_id] = target;
        const auto it = orders_.find(target);
        if (it != orders_.end()) {
            it->second.cancel_pending = true;
            it->second.pending_cancel_id = cancel.cl_ord_id;
        }
    }
    dispatch();
}

OrderRecord* OrderState::find(const ExecutionReport& report) {
    // A cancel's report names the cancel in ClOrdID and the order in
    // OrigClOrdID (OE-O2), so that is tried first.
    if (!report.orig_cl_ord_id.empty()) {
        const auto it = orders_.find(report.orig_cl_ord_id);
        if (it != orders_.end()) return &it->second;
    }
    const auto direct = orders_.find(report.cl_ord_id);
    if (direct != orders_.end()) return &direct->second;
    // A cancel we sent, whose target we recorded when we sent it.
    const auto cancel = cancels_.find(report.cl_ord_id);
    if (cancel != cancels_.end()) {
        const auto target = orders_.find(cancel->second);
        if (target != orders_.end()) return &target->second;
    }
    // Finally by OrderID, for anything unsolicited.
    for (auto& [id, record] : orders_) {
        if (!report.order_id.empty() && record.order_id == report.order_id) return &record;
    }
    return nullptr;
}

void OrderState::note(OrderAnomaly::Kind kind, const std::string& cl_ord_id, const std::string& symbol,
                      std::string detail) {
    ++anomalies_;
    pending_.push_back({kind, cl_ord_id, symbol, std::move(detail)});
}

void OrderState::apply_fill(OrderRecord& record, const ExecutionReport& report) {
    if (!report.fill) {
        note(OrderAnomaly::Kind::MissingFill, record.cl_ord_id, record.symbol,
             "ExecType=F carried no LastQty/LastPx");
        return;
    }
    const auto& fill = *report.fill;
    ++record.fills;
    record.filled_value += fill.quantity * fill.price;
    if (fill.fee) record.fees += *fill.fee;

    Position& position = positions_[record.symbol];
    position.symbol = record.symbol;
    ++position.fills;
    const Amount notional = notional_of(fill.quantity, fill.price);
    if (record.side == Side::Buy) {
        position.net += fill.quantity;
        position.bought += fill.quantity;
        position.cash += notional;  // paid away
    } else {
        position.net -= fill.quantity;
        position.sold += fill.quantity;
        position.cash -= notional;  // received
    }
    if (fill.fee) {
        position.fees += *fill.fee;
        position.cash += *fill.fee;
    }
}

void OrderState::apply(const ExecutionReport& report) {
    {
        std::lock_guard lock(mutex_);
        // Event Resend delivers the same events again, with the same ExecID
        // and PossResend=Y, so an ExecID already applied changes nothing.
        if (!report.exec_id.empty() && !seen_exec_ids_.insert(report.exec_id).second) {
            ++duplicate_reports_;
            return;
        }
        if (!report.exec_id.empty()) {
            // ExecIDs share one increasing space, so the newest is the bookmark.
            if (last_exec_id_.empty() || report.exec_id.size() > last_exec_id_.size() ||
                (report.exec_id.size() == last_exec_id_.size() && report.exec_id > last_exec_id_)) {
                last_exec_id_ = report.exec_id;
            }
        }

        OrderRecord* found = find(report);
        if (found == nullptr) {
            // Unsolicited, or ours from a previous run: record it so the
            // position is still right, and say so.
            note(OrderAnomaly::Kind::UnknownOrder, report.cl_ord_id, report.symbol,
                 "no order was sent with this id in this session");
            auto& created = orders_[report.cl_ord_id];
            created.cl_ord_id = report.cl_ord_id;
            created.symbol = report.symbol;
            created.side = report.side;
            created.created = report.transact_time;
            found = &created;
        }
        OrderRecord& record = *found;

        if (record.terminal && report.exec_type != ExecType::Rejected) {
            note(OrderAnomaly::Kind::AfterTerminal, record.cl_ord_id, record.symbol,
                 std::string("a ") + to_string(report.exec_type) + " report arrived after " +
                     to_string(record.status));
        }
        if (!report.order_id.empty() && report.order_id != "0") {
            if (!record.order_id.empty() && record.order_id != report.order_id) {
                note(OrderAnomaly::Kind::OrderIdChanged, record.cl_ord_id, record.symbol,
                     record.order_id + " became " + report.order_id);
            }
            record.order_id = report.order_id;
        }
        if (rank(report.status) > 0 && rank(report.status) < rank(record.status)) {
            note(OrderAnomaly::Kind::StatusRegression, record.cl_ord_id, record.symbol,
                 std::string(to_string(record.status)) + " -> " + to_string(report.status));
        }

        if (report.symbol.empty() == false) record.symbol = report.symbol;
        record.side = report.side;
        if (report.type) record.type = report.type;
        if (report.time_in_force) record.time_in_force = report.time_in_force;
        if (report.post_only != PostOnly::None) record.post_only = report.post_only;
        if (report.order_quantity) record.order_quantity = *report.order_quantity;
        if (report.price) record.price = *report.price;
        record.status = report.status;
        record.leaves_quantity = report.leaves_quantity;
        record.cumulative_quantity = report.cumulative_quantity;
        if (report.reject_reason) record.reject_reason = report.reject_reason;
        if (report.cancel_reason != CancelReason::None) record.cancel_reason = report.cancel_reason;
        if (!report.text.empty()) record.text = report.text;
        record.last_exec_id = report.exec_id;
        record.updated = report.transact_time;
        if (record.created == Clock::time_point{}) record.created = report.transact_time;

        // The fill first: the average below is computed from the running total,
        // which this updates.
        if (report.exec_type == ExecType::Trade) apply_fill(record, report);

        // The gateway omits AvgPx on a cancel of an order that had fills, so
        // ours is the fallback - and ours is the one that must agree with the
        // fills we have actually seen.
        if (report.average_price && *report.average_price > 0) {
            record.average_price = *report.average_price;
        } else if (record.cumulative_quantity > 0 && record.filled_value > 0) {
            record.average_price = static_cast<Price>(record.filled_value / record.cumulative_quantity);
        }

        // CumQty + LeavesQty should always equal the original quantity, except
        // on a terminal report that gives up the remainder.
        if (record.order_quantity > 0 && !is_terminal(report.status) &&
            report.cumulative_quantity + report.leaves_quantity != record.order_quantity) {
            note(OrderAnomaly::Kind::QuantityMismatch, record.cl_ord_id, record.symbol,
                 "CumQty " + format_qty(report.cumulative_quantity) + " + LeavesQty " +
                     format_qty(report.leaves_quantity) + " != OrderQty " +
                     format_qty(record.order_quantity));
        }

        if (is_terminal(report.status)) {
            record.terminal = true;
            record.cancel_pending = false;
        }
    }
    dispatch();
}

void OrderState::apply(const CancelReject& reject) {
    {
        std::lock_guard lock(mutex_);
        std::string target;
        if (!reject.orig_cl_ord_id.empty()) target = reject.orig_cl_ord_id;
        if (target.empty()) {
            const auto it = cancels_.find(reject.cl_ord_id);
            if (it != cancels_.end()) target = it->second;
        }
        const auto it = orders_.find(target);
        if (it == orders_.end()) {
            // A cancel for an order we never had, which is the gateway telling
            // us the order is unknown to it either.
            return;
        }
        // The cancel did not take effect; the order is whatever it was.
        it->second.cancel_pending = false;
        it->second.pending_cancel_id.clear();
        if (!reject.text.empty()) it->second.text = reject.text;
    }
    dispatch();
}

void OrderState::apply(const BusinessReject& reject) {
    {
        std::lock_guard lock(mutex_);
        // The synchronous path: no ExecutionReport follows, so this is the
        // order's terminal state. BusinessRejectRefID carries the ClOrdID.
        const auto it = orders_.find(reject.business_reject_ref_id);
        if (it == orders_.end()) return;
        it->second.status = OrdStatus::Rejected;
        it->second.terminal = true;
        it->second.leaves_quantity = 0;
        it->second.text = reject.text;
        it->second.updated = Clock::now();
    }
    dispatch();
}

void OrderState::apply(const MassCancelReport& report) {
    // The individual cancels arrive as ExecutionReports, which do the work; the
    // summary is only a receipt, so nothing here changes an order.
    (void)report;
}

void OrderState::apply(const SettlementReport& report) {
    {
        std::lock_guard lock(mutex_);
        Position& position = positions_[report.symbol];
        position.symbol = report.symbol;
        position.settlement_payout = report.payout;
        position.settlement_price = report.settlement_price;

        // The identity the report must satisfy:
        //   payout = price x long + (1 - price) x short
        const Amount expected = (report.settlement_price * report.long_quantity +
                                 (kPriceOne - report.settlement_price) * report.short_quantity) * 10;
        if (expected != report.payout) {
            note(OrderAnomaly::Kind::PayoutMismatch, {}, report.symbol,
                 "PayoutAmount " + format_amount(report.payout) + " but price x long + (1-price) x short = " +
                     format_amount(expected));
        }

        // Under net aggregation the report's legs should match our own net
        // position. Under gross they are per-leg and need not.
        if (report.net_aggregation) {
            const Qty settled_net = report.long_quantity - report.short_quantity;
            if (settled_net != position.net) {
                note(OrderAnomaly::Kind::PositionMismatch, {}, report.symbol,
                     "settled net " + format_qty(settled_net) + " but our fills make " +
                         format_qty(position.net) + " - a fill was missed");
            }
        }
    }
    dispatch();
}

void OrderState::dispatch() {
    std::vector<OrderAnomaly> anomalies;
    AnomalyHandler handler;
    {
        std::lock_guard lock(mutex_);
        anomalies.swap(pending_);
        handler = on_anomaly_;
    }
    if (!handler) return;
    for (const auto& anomaly : anomalies) handler(anomaly);
}

std::optional<OrderRecord> OrderState::order(const std::string& cl_ord_id) const {
    std::lock_guard lock(mutex_);
    const auto it = orders_.find(cl_ord_id);
    if (it == orders_.end()) return std::nullopt;
    return it->second;
}

std::vector<OrderRecord> OrderState::orders() const {
    std::lock_guard lock(mutex_);
    std::vector<OrderRecord> out;
    out.reserve(orders_.size());
    for (const auto& [id, record] : orders_) out.push_back(record);
    return out;
}

std::vector<OrderRecord> OrderState::open_orders() const {
    std::lock_guard lock(mutex_);
    std::vector<OrderRecord> out;
    for (const auto& [id, record] : orders_) {
        if (!record.terminal) out.push_back(record);
    }
    return out;
}

std::optional<Position> OrderState::position(const std::string& symbol) const {
    std::lock_guard lock(mutex_);
    const auto it = positions_.find(symbol);
    if (it == positions_.end()) return std::nullopt;
    return it->second;
}

std::vector<Position> OrderState::positions() const {
    std::lock_guard lock(mutex_);
    std::vector<Position> out;
    out.reserve(positions_.size());
    for (const auto& [symbol, position] : positions_) out.push_back(position);
    return out;
}

std::string OrderState::last_exec_id() const {
    std::lock_guard lock(mutex_);
    return last_exec_id_;
}

std::size_t OrderState::duplicate_reports() const {
    std::lock_guard lock(mutex_);
    return duplicate_reports_;
}

std::size_t OrderState::anomalies() const {
    std::lock_guard lock(mutex_);
    return anomalies_;
}

}  // namespace cpm::oms
