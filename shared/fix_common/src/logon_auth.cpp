#include "cpm/fix_common/logon_auth.hpp"

#include "cpm/common/signing.hpp"

#include <quickfix/FixFieldNumbers.h>

#include <cctype>
#include <stdexcept>

namespace cpm::fix {

AlignedSendingTime align_sending_time(std::string_view t) {
    // yyyyMMdd-HH:mm:ss is 17 chars; optional '.' + fraction digits.
    const auto digits = [](std::string_view s) {
        for (char c : s) {
            if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        }
        return true;
    };
    const bool shape_ok = t.size() >= 17 && digits(t.substr(0, 8)) && t[8] == '-' &&
                          digits(t.substr(9, 2)) && t[11] == ':' && digits(t.substr(12, 2)) &&
                          t[14] == ':' && digits(t.substr(15, 2));
    if (!shape_ok || (t.size() > 17 && (t[17] != '.' || t.size() < 19 || !digits(t.substr(18))))) {
        throw std::invalid_argument("SendingTime is not a FIX UTCTimestamp");
    }
    std::string ms(t.substr(0, 17));
    std::string frac = t.size() > 17 ? std::string(t.substr(18)) : std::string();
    frac.resize(3, '0');  // pad or truncate to milliseconds
    ms += '.';
    ms += frac;
    return {ms + "000", ms};
}

void apply_logon_auth(FIX::Message& logon, const Credentials& credentials) {
    FIX::Header& header = logon.getHeader();
    const AlignedSendingTime t = align_sending_time(header.getField(FIX::FIELD::SendingTime));
    header.setField(FIX::FIELD::SendingTime, t.wire);

    const std::string sender = header.getField(FIX::FIELD::SenderCompID);
    const std::string target = header.getField(FIX::FIELD::TargetCompID);
    logon.setField(FIX::FIELD::Password, credentials.passphrase);
    logon.setField(FIX::FIELD::Text, fix_logon_signature(t.signed_, sender, target,
                                                         credentials.passphrase,
                                                         credentials.secret_b64));
}

}  // namespace cpm::fix
