#pragma once

#include "cpm/common/config.hpp"

#include <quickfix/Message.h>

#include <string>
#include <string_view>

namespace cpm::fix {

struct AlignedSendingTime {
    std::string wire;    // yyyyMMdd-HH:mm:ss.SSS000 - what goes in SendingTime(52)
    std::string signed_; // yyyyMMdd-HH:mm:ss.SSS    - what goes into the signature
};

// Truncates a FIX UTCTimestamp (any precision, or none) to whole milliseconds.
// Sending µs with "000" digits makes the signed ms value and the wire value
// identical, so it does not matter whether the gateway truncates or rounds
// when it re-encodes SendingTime at ms precision (spec PDF p.50).
AlignedSendingTime align_sending_time(std::string_view sending_time);

// Completes an outgoing Logon(A) per spec §3: aligns SendingTime(52), sets
// Password(554) and Text(58) = HMAC signature over SendingTime + SenderCompID +
// TargetCompID + Password. Reads 49/52/56 from the header QuickFIX already filled.
void apply_logon_auth(FIX::Message& logon, const Credentials& credentials);

}  // namespace cpm::fix
