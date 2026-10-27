#pragma once

#include "cpm/common/logging.hpp"

#include <quickfix/Log.h>

#include <string>
#include <string_view>

namespace cpm::fix {

// Renders a raw FIX message for logging: SOH shown as '|', and secrets removed.
//  - Logon(A): Password(554), Text(58) (the HMAC signature) and RawData(96) -> "***"
//  - every occurrence of the API key -> redact_api_key(key)
std::string format_raw(std::string_view message, std::string_view api_key);

// Same API-key redaction for QuickFIX event text (session IDs contain the key).
std::string redact_text(std::string_view text, std::string_view api_key);

// QuickFIX LogFactory that writes raw traffic and session events to a Logger:
//   "<< 8=FIXT.1.1|..."   inbound
//   ">> 8=FIXT.1.1|..."   outbound
//   "-- <event>"          QuickFIX session events
class RawLogFactory : public FIX::LogFactory {
public:
    RawLogFactory(Logger& logger, std::string api_key);

    FIX::Log* create() override;
    FIX::Log* create(const FIX::SessionID&) override;
    void destroy(FIX::Log* log) override;

private:
    Logger& logger_;
    std::string api_key_;
};

}  // namespace cpm::fix
