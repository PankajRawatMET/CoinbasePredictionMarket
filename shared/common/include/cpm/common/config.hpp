#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace cpm {

struct ConfigError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// The only remote hosts this project may connect to, one per service (sandbox;
// docs/design.md §1 and docs/design-oms.md §1). The guard is per service rather
// than a shared allowlist, so neither client can be pointed at the other
// gateway: a market data config accepts only kMdSandboxHost and an order entry
// config only kOeSandboxHost.
inline constexpr std::string_view kMdSandboxHost = "fix-marketdata.cpm-sandbox.fairx.net";
inline constexpr std::string_view kOeSandboxHost = "fix-order.cpm-sandbox.fairx.net";
// TargetCompID of each CPM FIX gateway (spec), and the default when the
// environment does not override it. It is part of the Logon signature payload,
// so a wrong value fails the Logon as an invalid signature.
inline constexpr std::string_view kMdTargetCompId = "CPMMD";
inline constexpr std::string_view kOeTargetCompId = "CPMOE";
inline constexpr std::string_view kDefaultCaFile = "/etc/ssl/certs/ca-certificates.crt";

// Secrets: never log these. Use redact_api_key() for the key.
struct Credentials {
    std::string api_key;     // SenderCompID(49)
    std::string passphrase;  // Password(554)
    std::string secret_b64;  // HMAC signing secret, Base64
};

struct Endpoint {
    std::string host;
    std::uint16_t port = 0;
    bool tls = true;
    std::string ca_file{kDefaultCaFile};  // trust store used to verify the server (TLS only)
};

struct MdConfig {
    Credentials credentials;
    Endpoint endpoint;
    int heartbeat_secs = 30;  // HeartBtInt(108), spec §1 recommended value
    std::string target_comp_id{kMdTargetCompId};  // CPM_FIX_TARGET_COMP_ID
};

// Cancel-on-disconnect scope, CancelOrdersOnDisconnect(8013). Fixed at Logon
// for the life of the session (docs/open-questions-oms.txt OE-Q4).
enum class CancelOnDisconnect : char {
    None = 'N',       // resting orders survive the drop (the gateway's default)
    Session = 'Y',    // cancel this session's orders
    ApiKey = 'A',     // cancel every order of the API key
};

struct OeConfig {
    Credentials credentials;
    Endpoint endpoint;
    // HeartBtInt(108): the gateway defaults to 10 s and caps it at 300 s; the
    // spec recommends 30.
    int heartbeat_secs = 30;

    // Session options, sent on every Logon and immutable afterwards.
    CancelOnDisconnect cancel_on_disconnect = CancelOnDisconnect::ApiKey;
    bool settlement_reports = true;  // ReceiveSettlementReports(20127)

    // ManualOrderIndicator(1028), required on every order and cancel: a CFTC
    // Rule 1.35 audit flag, so it is configured rather than defaulted in code.
    bool manual_orders = false;
    // Parties/PartyRole=24 sub-account, required on every order. May be empty
    // until the first order is sent (OE-Q2).
    std::string subaccount;

    std::string target_comp_id{kOeTargetCompId};  // CPM_OE_TARGET_COMP_ID
};

using EnvLookup = std::function<std::optional<std::string>(const std::string& name)>;

// Reads the real process environment.
EnvLookup process_env();

// Reads <prefix>API_KEY, <prefix>PASSPHRASE and <prefix>SECRET (Base64).
// The default prefix is the market data one; order entry uses "CPM_OE_".
Credentials load_credentials(const EnvLookup& env, const std::string& prefix = "CPM_");

// Builds MdConfig from CPM_API_KEY, CPM_PASSPHRASE, CPM_SECRET, CPM_FIX_HOST,
// CPM_FIX_PORT, CPM_FIX_TLS (default 1), CPM_FIX_CA_FILE (optional) and
// CPM_FIX_TARGET_COMP_ID (optional, default CPMMD), then validates it. Error messages name variables, never their values.
MdConfig load_md_config(const EnvLookup& env);

// Builds OeConfig from CPM_OE_API_KEY, CPM_OE_PASSPHRASE, CPM_OE_SECRET,
// CPM_OE_HOST, CPM_OE_PORT, CPM_OE_TLS (default 1), CPM_OE_CA_FILE (optional),
// CPM_OE_SUBACCOUNT (optional until an order is sent),
// CPM_OE_CANCEL_ON_DISCONNECT (N/Y/A), CPM_OE_SETTLEMENT_REPORTS (Y/N),
// CPM_OE_MANUAL_ORDERS (Y/N) and CPM_OE_TARGET_COMP_ID (optional, default
// CPMOE), then validates it.
OeConfig load_oe_config(const EnvLookup& env);

// A FIX CompID must be non-empty printable ASCII with no '=' or SOH, which
// would corrupt the wire, and at most 64 characters. Throws ConfigError.
void validate_comp_id(const std::string& name, const std::string& value);

// Sandbox-only guard: the endpoint must be `allowed_host` over TLS, or a
// loopback address (local simulator, TLS optional). Throws ConfigError otherwise.
void validate_endpoint(const Endpoint& endpoint, std::string_view allowed_host);

// "abcd****": enough to tell keys apart in logs without revealing them.
std::string redact_api_key(std::string_view api_key);

}  // namespace cpm
