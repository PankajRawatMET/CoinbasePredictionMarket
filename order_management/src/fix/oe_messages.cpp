#include "fix/oe_messages.hpp"

#include <quickfix/FixFieldNumbers.h>
#include <quickfix/FixValues.h>
#include <quickfix/Group.h>

namespace cpm::oms::fix {
namespace {

namespace F = FIX::FIELD;

// CPM tags that FixFieldNumbers.h does not name.
constexpr int kMaxExecutionCost = 21009;
constexpr int kSelfMatchPreventionId = 7928;
constexpr int kSelfMatchPreventionStrategy = 8000;
constexpr int kBeginExecId = 22003;
constexpr int kEndExecId = 22004;

FIX::Message new_message(const char* type, const FIX::message_order& body) {
    FIX::Message message(FIX::message_order(FIX::message_order::header),
                         FIX::message_order(FIX::message_order::trailer), body);
    message.getHeader().setField(F::MsgType, type);
    return message;
}

const char* side_value(Side side) { return side == Side::Buy ? "1" : "2"; }

const char* tif_value(TimeInForce tif) {
    switch (tif) {
        case TimeInForce::Gtc: return "1";
        case TimeInForce::Ioc: return "3";
        case TimeInForce::Fok: return "4";
    }
    return "1";
}

const char* smp_value(SmpStrategy strategy) {
    switch (strategy) {
        case SmpStrategy::CancelAggressing: return "N";
        case SmpStrategy::CancelResting: return "O";
        case SmpStrategy::CancelBoth: return "Q";
        case SmpStrategy::Unset: break;
    }
    return "";
}

// Exactly one entry with PartyRole(452)=24, which the gateway requires and
// creates lazily on first use.
void add_subaccount(FIX::Message& message, const std::string& subaccount) {
    FIX::Group party(F::NoPartyIDs, F::PartyID, FIX::message_order(F::PartyID, F::PartyRole, 0));
    party.setField(F::PartyID, subaccount);
    party.setField(F::PartyRole, "24");
    message.addGroup(party);
}

}  // namespace

FIX::Message new_order_single(const NewOrder& order, const OeConfig& config) {
    FIX::Message message = new_message(
        FIX::MsgType_NewOrderSingle,
        FIX::message_order(F::ClOrdID, F::Symbol, F::Side, F::OrderQty, F::Price, F::OrdType,
                           F::TimeInForce, F::ExecInst, kMaxExecutionCost, kSelfMatchPreventionId,
                           kSelfMatchPreventionStrategy, F::ManualOrderIndicator, F::NoPartyIDs, 0));
    message.setField(F::ClOrdID, order.cl_ord_id);
    message.setField(F::Symbol, order.symbol);
    message.setField(F::Side, side_value(order.side));
    message.setField(F::OrderQty, format_qty(order.quantity));
    if (order.type == OrderType::Limit) {
        message.setField(F::Price, format_price(order.price));
        message.setField(F::OrdType, "2");
    } else {
        // No Price(44) at all: a market order carrying one is rejected.
        message.setField(F::OrdType, "1");
    }
    message.setField(F::TimeInForce, tif_value(order.time_in_force));
    if (order.post_only == PostOnly::Slide) message.setField(F::ExecInst, "6");
    if (order.post_only == PostOnly::Reject) message.setField(F::ExecInst, "A");
    if (order.max_execution_cost) {
        message.setField(kMaxExecutionCost, format_amount(*order.max_execution_cost));
    }
    if (order.smp_id) message.setField(kSelfMatchPreventionId, std::to_string(*order.smp_id));
    if (order.smp_strategy != SmpStrategy::Unset) {
        message.setField(kSelfMatchPreventionStrategy, smp_value(order.smp_strategy));
    }
    message.setField(F::ManualOrderIndicator, config.manual_orders ? "Y" : "N");
    add_subaccount(message, config.subaccount);
    return message;
}

FIX::Message order_cancel_request(const CancelOrder& cancel, const OeConfig& config) {
    FIX::Message message = new_message(
        FIX::MsgType_OrderCancelRequest,
        FIX::message_order(F::ClOrdID, F::OrigClOrdID, F::OrderID, F::Symbol, F::Side,
                           F::ManualOrderIndicator, 0));
    message.setField(F::ClOrdID, cancel.cl_ord_id);
    // The gateway resolves by OrderID when both are present; send what we have,
    // since an order can be cancelled before its first report arrives.
    if (!cancel.orig_cl_ord_id.empty()) message.setField(F::OrigClOrdID, cancel.orig_cl_ord_id);
    if (!cancel.order_id.empty()) message.setField(F::OrderID, cancel.order_id);
    message.setField(F::Symbol, cancel.symbol);
    message.setField(F::Side, side_value(cancel.side));
    // On a cancel this is the audit flag for the cancel action itself.
    message.setField(F::ManualOrderIndicator, config.manual_orders ? "Y" : "N");
    return message;
}

FIX::Message order_mass_cancel_request(const MassCancel& request, const OeConfig& config) {
    FIX::Message message = new_message(
        FIX::MsgType_OrderMassCancelRequest,
        FIX::message_order(F::ClOrdID, F::MassCancelRequestType, F::Symbol, F::Side, F::NoPartyIDs, 0));
    message.setField(F::ClOrdID, request.cl_ord_id);
    switch (request.scope) {
        case MassCancel::Scope::Security: message.setField(F::MassCancelRequestType, "1"); break;
        case MassCancel::Scope::TradingSession: message.setField(F::MassCancelRequestType, "6"); break;
        case MassCancel::Scope::All: message.setField(F::MassCancelRequestType, "7"); break;
    }
    if (!request.symbol.empty()) message.setField(F::Symbol, request.symbol);
    if (request.side) message.setField(F::Side, side_value(*request.side));
    // Filters AND together, so a sub-account entry narrows the cancel.
    if (request.filter_by_subaccount && !config.subaccount.empty()) {
        add_subaccount(message, config.subaccount);
    }
    return message;
}

FIX::Message last_exec_id_request() {
    // F1 carries no parameters; correlate the reply by RefSeqNum(45).
    return new_message("F1", FIX::message_order(F::TestReqID, 0));
}

FIX::Message event_resend_request(const std::string& begin_exec_id, const std::string& end_exec_id) {
    FIX::Message message = new_message("F3", FIX::message_order(kBeginExecId, kEndExecId, 0));
    message.setField(kBeginExecId, begin_exec_id);
    // Omitted means "everything since begin".
    if (!end_exec_id.empty()) message.setField(kEndExecId, end_exec_id);
    return message;
}

}  // namespace cpm::oms::fix
