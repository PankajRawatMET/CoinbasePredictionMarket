#pragma once

#include "cpm/oms/orders.hpp"

#include <quickfix/Message.h>

#include <string_view>
#include <vector>

namespace cpm::oms::fix {

// Everything one inbound message decoded into. A message yields at most one
// event, but errors can accompany it, so each is a vector.
struct DecodedBatch {
    std::vector<ExecutionReport> reports;
    std::vector<CancelReject> cancel_rejects;
    std::vector<MassCancelReport> mass_cancel_reports;
    std::vector<SettlementReport> settlements;
    std::vector<LastExecId> last_exec_ids;
    std::vector<ResendComplete> resend_completes;
    std::vector<ResendReject> resend_rejects;
    std::vector<BusinessReject> business_rejects;
    std::vector<DecodeError> errors;

    bool empty() const;
};

// Decodes ExecutionReport(8), OrderCancelReject(9), OrderMassCancelReport(r),
// MarketSettlementReport(UMS), LastExecID(F2), EventResendComplete(F4),
// EventResendReject(F5) and BusinessMessageReject(j).
//
// Decimals are parsed the way the gateway writes them: excess precision is
// truncated toward zero rather than refused (OE-Q8), and amounts may be
// negative. Identifiers stay strings. Never throws: a malformed message
// becomes a DecodeError, with `api_key` redacted out of its raw text.
DecodedBatch decode_message(const FIX::Message& message, std::string_view api_key = {});

}  // namespace cpm::oms::fix
