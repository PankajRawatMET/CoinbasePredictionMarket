#include "cpm/oms/orders.hpp"

namespace cpm::oms {

const char* to_string(Side side) { return side == Side::Buy ? "buy" : "sell"; }

const char* to_string(OrderType type) { return type == OrderType::Market ? "market" : "limit"; }

const char* to_string(TimeInForce tif) {
    switch (tif) {
        case TimeInForce::Gtc: return "GTC";
        case TimeInForce::Ioc: return "IOC";
        case TimeInForce::Fok: return "FOK";
    }
    return "GTC";
}

const char* to_string(PostOnly post_only) {
    switch (post_only) {
        case PostOnly::None: return "none";
        case PostOnly::Slide: return "post-only-slide";
        case PostOnly::Reject: return "post-only-reject";
    }
    return "none";
}

const char* to_string(ExecType type) {
    switch (type) {
        case ExecType::PendingNew: return "pending-new";
        case ExecType::New: return "new";
        case ExecType::Canceled: return "canceled";
        case ExecType::Rejected: return "rejected";
        case ExecType::Trade: return "trade";
        case ExecType::Expired: return "expired";
        case ExecType::Replaced: return "replaced";
        case ExecType::PendingCancel: return "pending-cancel";
        case ExecType::Unknown: return "unknown";
    }
    return "unknown";
}

const char* to_string(OrdStatus status) {
    switch (status) {
        case OrdStatus::New: return "new";
        case OrdStatus::PartiallyFilled: return "partially-filled";
        case OrdStatus::Filled: return "filled";
        case OrdStatus::Canceled: return "canceled";
        case OrdStatus::Rejected: return "rejected";
        case OrdStatus::PendingNew: return "pending-new";
        case OrdStatus::PendingCancel: return "pending-cancel";
        case OrdStatus::Expired: return "expired";
        case OrdStatus::Undefined: return "undefined";
        case OrdStatus::Unknown: return "unknown";
    }
    return "unknown";
}

const char* to_string(CancelReason reason) {
    switch (reason) {
        case CancelReason::None: return "none";
        case CancelReason::CancelOnDisconnect: return "cancel-on-disconnect";
        case CancelReason::InstrumentLifecycle: return "instrument-lifecycle";
        case CancelReason::SelfMatchPrevention: return "self-match-prevention";
        case CancelReason::AdminCancel: return "admin-cancel";
        case CancelReason::Unknown: return "unknown";
    }
    return "none";
}

}  // namespace cpm::oms
