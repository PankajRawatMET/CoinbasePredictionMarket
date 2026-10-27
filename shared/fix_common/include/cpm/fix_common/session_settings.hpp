#pragma once

#include "cpm/common/config.hpp"

#include <quickfix/SessionSettings.h>

#include <cstdint>
#include <string>

namespace cpm::fix {

// Market data service (CPMMD).
inline constexpr const char* kTransportDictionary = "FIXT11-CPMMD.xml";
inline constexpr const char* kAppDictionary = "FIX50SP2-CPMMD.xml";

// Order entry service (CPMOE): a separate gateway with its own message
// catalog, so its own pair of dictionaries.
inline constexpr const char* kOeTransportDictionary = "FIXT11-CPMOE.xml";
inline constexpr const char* kOeAppDictionary = "FIX50SP2-CPMOE.xml";

// QuickFIX initiator settings for the CPM MD gateway, built in code so no
// settings file (and no credential) ever touches disk. Session rules from spec
// §1: FIXT.1.1 / FIX50SP2, TargetCompID from the config, ResetSeqNumFlag=Y,
// µs SendingTime. No persistence: callers pair this with a memory/null store.
FIX::SessionSettings make_initiator_settings(const MdConfig& config, const std::string& dict_dir,
                                             int reconnect_interval_secs = 30);

// Initiator settings for the CPM order entry gateway (CPMOE). Same shape as the
// market data initiator, with its own dictionaries and TargetCompID. The Logon
// body options (8013, 20127) are set by the application, not here.
FIX::SessionSettings make_oe_initiator_settings(const OeConfig& config, const std::string& dict_dir,
                                                int reconnect_interval_secs = 30);

// Acceptor settings for an order entry simulator or loopback test: plays the
// order entry gateway for one client API key on plain TCP. `server_comp_id` is
// the comp id to play, which must match the client's CPM_OE_TARGET_COMP_ID.
FIX::SessionSettings make_oe_acceptor_settings(const std::string& client_api_key, std::uint16_t port,
                                               const std::string& dict_dir,
                                               const std::string& server_comp_id =
                                                   std::string(kOeTargetCompId));

// Acceptor settings for the local simulator: plays CPMMD for one client API key,
// plain TCP on `port`, same dictionaries and session rules. Nothing is
// persisted, so ResendRequests are answered with a gap fill.
FIX::SessionSettings make_acceptor_settings(const std::string& client_api_key, std::uint16_t port,
                                            const std::string& dict_dir,
                                            const std::string& server_comp_id =
                                                std::string(kMdTargetCompId));

}  // namespace cpm::fix
