#include "cpm/common/config.hpp"

#include "cpm/common/signing.hpp"

#include <array>
#include <charconv>
#include <cstdlib>

namespace cpm {
namespace {

std::string require(const EnvLookup& env, const std::string& name) {
    auto value = env(name);
    if (!value || value->empty()) throw ConfigError("environment variable " + name + " is not set");
    return *value;
}

std::uint16_t parse_port(const std::string& name, const std::string& text) {
    unsigned value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size() || value == 0 || value > 65535) {
        throw ConfigError(name + " must be an integer in 1..65535");
    }
    return static_cast<std::uint16_t>(value);
}

bool parse_flag(const std::string& name, const std::string& text) {
    if (text == "1") return true;
    if (text == "0") return false;
    throw ConfigError(name + " must be 0 or 1");
}

// Y/N, as the FIX BOOLEAN fields these end up in are written.
bool parse_yn(const std::string& name, const std::string& text) {
    if (text == "Y" || text == "y") return true;
    if (text == "N" || text == "n") return false;
    throw ConfigError(name + " must be Y or N");
}

// Reads the endpoint fields shared by both services from <prefix>-named vars.
Endpoint load_endpoint(const EnvLookup& env, const std::string& prefix) {
    Endpoint endpoint;
    endpoint.host = require(env, prefix + "HOST");
    endpoint.port = parse_port(prefix + "PORT", require(env, prefix + "PORT"));
    if (auto tls = env(prefix + "TLS"); tls && !tls->empty()) {
        endpoint.tls = parse_flag(prefix + "TLS", *tls);
    }
    if (auto ca = env(prefix + "CA_FILE"); ca && !ca->empty()) endpoint.ca_file = *ca;
    return endpoint;
}

bool is_loopback(std::string_view host) {
    constexpr std::array<std::string_view, 3> kLoopback{"127.0.0.1", "localhost", "::1"};
    for (auto h : kLoopback) {
        if (host == h) return true;
    }
    return false;
}

}  // namespace

EnvLookup process_env() {
    return [](const std::string& name) -> std::optional<std::string> {
        if (const char* v = std::getenv(name.c_str())) return std::string(v);
        return std::nullopt;
    };
}

Credentials load_credentials(const EnvLookup& env, const std::string& prefix) {
    Credentials creds;
    creds.api_key = require(env, prefix + "API_KEY");
    creds.passphrase = require(env, prefix + "PASSPHRASE");
    creds.secret_b64 = require(env, prefix + "SECRET");
    try {
        base64_decode(creds.secret_b64);
    } catch (const SigningError&) {
        throw ConfigError(prefix + "SECRET is not valid Base64");
    }
    return creds;
}

MdConfig load_md_config(const EnvLookup& env) {
    MdConfig cfg;
    cfg.credentials = load_credentials(env);
    // Market data keeps its historic variable names: CPM_FIX_HOST and friends.
    cfg.endpoint.host = require(env, "CPM_FIX_HOST");
    cfg.endpoint.port = parse_port("CPM_FIX_PORT", require(env, "CPM_FIX_PORT"));
    if (auto tls = env("CPM_FIX_TLS"); tls && !tls->empty()) {
        cfg.endpoint.tls = parse_flag("CPM_FIX_TLS", *tls);
    }
    if (auto ca = env("CPM_FIX_CA_FILE"); ca && !ca->empty()) cfg.endpoint.ca_file = *ca;
    if (auto id = env("CPM_FIX_TARGET_COMP_ID"); id && !id->empty()) cfg.target_comp_id = *id;

    validate_comp_id("CPM_FIX_TARGET_COMP_ID", cfg.target_comp_id);
    validate_endpoint(cfg.endpoint, kMdSandboxHost);
    return cfg;
}

OeConfig load_oe_config(const EnvLookup& env) {
    OeConfig cfg;
    cfg.credentials = load_credentials(env, "CPM_OE_");
    cfg.endpoint = load_endpoint(env, "CPM_OE_");

    if (auto hb = env("CPM_OE_HEARTBEAT"); hb && !hb->empty()) {
        unsigned value = 0;
        const auto [end, ec] = std::from_chars(hb->data(), hb->data() + hb->size(), value);
        // The gateway caps HeartBtInt at 300 s.
        if (ec != std::errc{} || end != hb->data() + hb->size() || value < 1 || value > 300) {
            throw ConfigError("CPM_OE_HEARTBEAT must be an integer in 1..300");
        }
        cfg.heartbeat_secs = static_cast<int>(value);
    }

    if (auto cod = env("CPM_OE_CANCEL_ON_DISCONNECT"); cod && !cod->empty()) {
        if (*cod == "N") {
            cfg.cancel_on_disconnect = CancelOnDisconnect::None;
        } else if (*cod == "Y") {
            cfg.cancel_on_disconnect = CancelOnDisconnect::Session;
        } else if (*cod == "A") {
            cfg.cancel_on_disconnect = CancelOnDisconnect::ApiKey;
        } else {
            throw ConfigError("CPM_OE_CANCEL_ON_DISCONNECT must be N, Y or A");
        }
    }
    if (auto s = env("CPM_OE_SETTLEMENT_REPORTS"); s && !s->empty()) {
        cfg.settlement_reports = parse_yn("CPM_OE_SETTLEMENT_REPORTS", *s);
    }
    if (auto m = env("CPM_OE_MANUAL_ORDERS"); m && !m->empty()) {
        cfg.manual_orders = parse_yn("CPM_OE_MANUAL_ORDERS", *m);
    }
    // Optional here: a session can log on without one, and the order path
    // checks it before building a NewOrderSingle (OE-Q2).
    if (auto sub = env("CPM_OE_SUBACCOUNT"); sub) cfg.subaccount = *sub;
    if (auto id = env("CPM_OE_TARGET_COMP_ID"); id && !id->empty()) cfg.target_comp_id = *id;

    validate_comp_id("CPM_OE_TARGET_COMP_ID", cfg.target_comp_id);
    validate_endpoint(cfg.endpoint, kOeSandboxHost);
    return cfg;
}

void validate_comp_id(const std::string& name, const std::string& value) {
    if (value.empty()) throw ConfigError(name + " must not be empty");
    if (value.size() > 64) throw ConfigError(name + " must be at most 64 characters");
    for (const unsigned char c : value) {
        // '=' and SOH are the FIX field separators; anything outside printable
        // ASCII would not survive the wire either.
        if (c < 0x21 || c > 0x7E || c == '=') {
            throw ConfigError(name + " must be printable ASCII without '=' or spaces");
        }
    }
}

void validate_endpoint(const Endpoint& endpoint, std::string_view allowed_host) {
    if (endpoint.port == 0) throw ConfigError("port is not set");
    if (is_loopback(endpoint.host)) return;
    if (endpoint.host != allowed_host) {
        throw ConfigError("refusing to connect to '" + endpoint.host + "': only " +
                          std::string(allowed_host) + " or a loopback simulator is allowed");
    }
    if (!endpoint.tls) throw ConfigError("the sandbox host requires TLS");
    if (endpoint.ca_file.empty()) throw ConfigError("TLS requires a CA file to verify the server");
}

std::string redact_api_key(std::string_view api_key) {
    return std::string(api_key.substr(0, 4)) + "****";
}

}  // namespace cpm
