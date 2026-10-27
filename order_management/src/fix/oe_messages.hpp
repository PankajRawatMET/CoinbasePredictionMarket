#pragma once

#include "cpm/common/config.hpp"
#include "cpm/oms/orders.hpp"

#include <quickfix/Message.h>

namespace cpm::oms::fix {

// Builds the three outbound application messages. The caller has already run
// the order_rules checks; these only encode. ManualOrderIndicator(1028) and the
// Parties sub-account come from the session config, because they are the same
// for every order the session sends.
//
// Field order follows the spec's examples, and the Parties group starts with
// PartyID(448), which is the group delimiter in our dictionary.
FIX::Message new_order_single(const NewOrder& order, const OeConfig& config);
FIX::Message order_cancel_request(const CancelOrder& cancel, const OeConfig& config);
FIX::Message order_mass_cancel_request(const MassCancel& request, const OeConfig& config);

// LastExecIDRequest(F1) and EventResendRequest(F3): the recovery pair.
FIX::Message last_exec_id_request();
FIX::Message event_resend_request(const std::string& begin_exec_id, const std::string& end_exec_id = {});

}  // namespace cpm::oms::fix
