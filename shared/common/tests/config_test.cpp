#include "cpm/common/config.hpp"

#include <gtest/gtest.h>

#include <map>
#include <string>

namespace {

using Env = std::map<std::string, std::string>;

cpm::EnvLookup lookup(const Env& env) {
    return [env](const std::string& name) -> std::optional<std::string> {
        if (auto it = env.find(name); it != env.end()) return it->second;
        return std::nullopt;
    };
}

Env sandbox_env() {
    return {{"CPM_API_KEY", "key-123456"},
            {"CPM_PASSPHRASE", "pass-secret"},
            {"CPM_SECRET", "dGVzdC1zZWNyZXQ="},
            {"CPM_FIX_HOST", std::string(cpm::kMdSandboxHost)},
            {"CPM_FIX_PORT", "6120"}};
}

std::string error_of(const Env& env) {
    try {
        cpm::load_md_config(lookup(env));
    } catch (const cpm::ConfigError& e) {
        return e.what();
    }
    return "";
}

TEST(Config, LoadsSandboxConfigWithTlsByDefault) {
    const auto cfg = cpm::load_md_config(lookup(sandbox_env()));
    EXPECT_EQ(cfg.credentials.api_key, "key-123456");
    EXPECT_EQ(cfg.endpoint.host, cpm::kMdSandboxHost);
    EXPECT_EQ(cfg.endpoint.port, 6120);
    EXPECT_TRUE(cfg.endpoint.tls);
    EXPECT_EQ(cfg.endpoint.ca_file, cpm::kDefaultCaFile);
    EXPECT_EQ(cfg.heartbeat_secs, 30);
}

TEST(Config, MissingVariableIsNamedWithoutLeakingOthers) {
    auto env = sandbox_env();
    env.erase("CPM_SECRET");
    const auto err = error_of(env);
    EXPECT_NE(err.find("CPM_SECRET"), std::string::npos);
    EXPECT_EQ(err.find("pass-secret"), std::string::npos);
}

TEST(Config, RejectsBadSecretPortAndFlag) {
    auto env = sandbox_env();
    env["CPM_SECRET"] = "not base64!";
    EXPECT_NE(error_of(env).find("CPM_SECRET"), std::string::npos);
    EXPECT_EQ(error_of(env).find("not base64"), std::string::npos);  // value never echoed

    for (const char* port : {"0", "65536", "61x", ""}) {
        env = sandbox_env();
        env["CPM_FIX_PORT"] = port;
        EXPECT_NE(error_of(env), "") << "port '" << port << "'";
    }

    env = sandbox_env();
    env["CPM_FIX_TLS"] = "yes";
    EXPECT_NE(error_of(env).find("CPM_FIX_TLS"), std::string::npos);
}

TEST(SandboxGuard, RefusesAnyOtherRemoteHost) {
    for (const char* host : {"fix-marketdata.cpm.fairx.net", "fix-marketdata.cpm-sandbox.fairx.net.evil.com",
                             "10.0.0.1", "example.com"}) {
        auto env = sandbox_env();
        env["CPM_FIX_HOST"] = host;
        EXPECT_NE(error_of(env).find("refusing"), std::string::npos) << host;
    }
}

TEST(SandboxGuard, SandboxRequiresTls) {
    auto env = sandbox_env();
    env["CPM_FIX_TLS"] = "0";
    EXPECT_NE(error_of(env).find("requires TLS"), std::string::npos);
}

TEST(SandboxGuard, LoopbackSimulatorAllowedWithoutTls) {
    for (const char* host : {"127.0.0.1", "localhost", "::1"}) {
        auto env = sandbox_env();
        env["CPM_FIX_HOST"] = host;
        env["CPM_FIX_TLS"] = "0";
        EXPECT_EQ(error_of(env), "") << host;
    }
}

TEST(Redaction, KeepsOnlyKeyPrefix) {
    EXPECT_EQ(cpm::redact_api_key("abcdEFGH12345678"), "abcd****");
    EXPECT_EQ(cpm::redact_api_key("ab"), "ab****");
}

TEST(Config, TargetCompIdDefaultsToTheServiceAndCanBeOverridden) {
    EXPECT_EQ(cpm::load_md_config(lookup(sandbox_env())).target_comp_id, cpm::kMdTargetCompId);

    Env env = sandbox_env();
    env["CPM_FIX_TARGET_COMP_ID"] = "CPMMD2";
    EXPECT_EQ(cpm::load_md_config(lookup(env)).target_comp_id, "CPMMD2");

    // An empty override falls back to the default rather than sending nothing.
    env["CPM_FIX_TARGET_COMP_ID"] = "";
    EXPECT_EQ(cpm::load_md_config(lookup(env)).target_comp_id, cpm::kMdTargetCompId);
}

TEST(Config, ACompIdThatCouldNotSurviveTheWireIsRefused) {
    // std::string rather than const char*: the last of these is built here,
    // and a .c_str() off a temporary in a braced list dangles before the loop
    // reads it - which is exactly how this test failed one run in a hundred.
    for (const std::string& bad :
         {std::string("CPM MD"), std::string("CPM=MD"), std::string("CPM\tMD"),
          std::string(65, 'x')}) {
        Env env = sandbox_env();
        env["CPM_FIX_TARGET_COMP_ID"] = bad;
        const auto message = error_of(env);
        EXPECT_NE(message.find("CPM_FIX_TARGET_COMP_ID"), std::string::npos) << "accepted '" << bad << "'";
    }
}

// --- order entry (CPM_OE_*) -----------------------------------------------

Env oe_env() {
    return {{"CPM_OE_API_KEY", "oe-key-1"},
            {"CPM_OE_PASSPHRASE", "oe-pass"},
            {"CPM_OE_SECRET", "dGVzdC1zZWNyZXQ="},
            {"CPM_OE_HOST", std::string(cpm::kOeSandboxHost)},
            {"CPM_OE_PORT", "6110"}};
}

std::string oe_error_of(const Env& env) {
    try {
        cpm::load_oe_config(lookup(env));
    } catch (const cpm::ConfigError& e) {
        return e.what();
    }
    return "";
}

TEST(OeConfigTest, LoadsTheSandboxDefaults) {
    const auto cfg = cpm::load_oe_config(lookup(oe_env()));
    EXPECT_EQ(cfg.credentials.api_key, "oe-key-1");
    EXPECT_EQ(cfg.endpoint.host, cpm::kOeSandboxHost);
    EXPECT_EQ(cfg.endpoint.port, 6110);
    EXPECT_TRUE(cfg.endpoint.tls);
    EXPECT_EQ(cfg.endpoint.ca_file, cpm::kDefaultCaFile);
    EXPECT_EQ(cfg.heartbeat_secs, 30);
    // The defaults of docs/open-questions-oms.txt OE-Q4, OE-Q5 and OE-Q11.
    EXPECT_EQ(cfg.cancel_on_disconnect, cpm::CancelOnDisconnect::ApiKey);
    EXPECT_TRUE(cfg.settlement_reports);
    EXPECT_FALSE(cfg.manual_orders);
    EXPECT_TRUE(cfg.subaccount.empty());
}

TEST(OeConfigTest, ReadsItsOwnVariablesAndNotMarketDatas) {
    // Market data variables must not satisfy an order entry config.
    Env env = sandbox_env();
    EXPECT_NE(oe_error_of(env).find("CPM_OE_API_KEY"), std::string::npos) << oe_error_of(env);

    // And the two can coexist in one environment without interfering.
    for (const auto& [k, v] : oe_env()) env[k] = v;
    const auto oe = cpm::load_oe_config(lookup(env));
    const auto md = cpm::load_md_config(lookup(env));
    EXPECT_EQ(oe.endpoint.host, cpm::kOeSandboxHost);
    EXPECT_EQ(oe.endpoint.port, 6110);
    EXPECT_EQ(md.endpoint.host, cpm::kMdSandboxHost);
    EXPECT_EQ(md.endpoint.port, 6120);
}

TEST(OeConfigTest, TheGuardIsPerService) {
    Env env = oe_env();
    // The market data host is not acceptable to an order entry config.
    env["CPM_OE_HOST"] = std::string(cpm::kMdSandboxHost);
    const auto message = oe_error_of(env);
    EXPECT_NE(message.find("refusing to connect"), std::string::npos) << message;
    EXPECT_NE(message.find(std::string(cpm::kOeSandboxHost)), std::string::npos) << message;

    // Nor is anything else.
    env["CPM_OE_HOST"] = "fix-order.example.com";
    EXPECT_NE(oe_error_of(env).find("refusing to connect"), std::string::npos);

    // Loopback is allowed for a local simulator, in the clear.
    env["CPM_OE_HOST"] = "127.0.0.1";
    env["CPM_OE_TLS"] = "0";
    EXPECT_EQ(oe_error_of(env), "");

    // The sandbox host requires TLS.
    env["CPM_OE_HOST"] = std::string(cpm::kOeSandboxHost);
    EXPECT_NE(oe_error_of(env).find("requires TLS"), std::string::npos);
}

TEST(OeConfigTest, TargetCompIdComesFromItsOwnVariable) {
    EXPECT_EQ(cpm::load_oe_config(lookup(oe_env())).target_comp_id, cpm::kOeTargetCompId);

    // The two services read different variables, so one cannot shadow the other.
    Env env = oe_env();
    for (const auto& [k, v] : sandbox_env()) env[k] = v;
    env["CPM_FIX_TARGET_COMP_ID"] = "CPMMD";
    env["CPM_OE_TARGET_COMP_ID"] = "CPMOE2";
    EXPECT_EQ(cpm::load_oe_config(lookup(env)).target_comp_id, "CPMOE2");
    EXPECT_EQ(cpm::load_md_config(lookup(env)).target_comp_id, "CPMMD");

    env["CPM_OE_TARGET_COMP_ID"] = "CPM OE";
    EXPECT_NE(oe_error_of(env).find("CPM_OE_TARGET_COMP_ID"), std::string::npos);
}

TEST(OeConfigTest, SessionOptionsAreParsedAndValidated) {
    Env env = oe_env();
    env["CPM_OE_CANCEL_ON_DISCONNECT"] = "N";
    env["CPM_OE_SETTLEMENT_REPORTS"] = "N";
    env["CPM_OE_MANUAL_ORDERS"] = "Y";
    env["CPM_OE_SUBACCOUNT"] = "sub-1";
    env["CPM_OE_HEARTBEAT"] = "10";
    const auto cfg = cpm::load_oe_config(lookup(env));
    EXPECT_EQ(cfg.cancel_on_disconnect, cpm::CancelOnDisconnect::None);
    EXPECT_FALSE(cfg.settlement_reports);
    EXPECT_TRUE(cfg.manual_orders);
    EXPECT_EQ(cfg.subaccount, "sub-1");
    EXPECT_EQ(cfg.heartbeat_secs, 10);

    env["CPM_OE_CANCEL_ON_DISCONNECT"] = "Y";
    EXPECT_EQ(cpm::load_oe_config(lookup(env)).cancel_on_disconnect, cpm::CancelOnDisconnect::Session);

    // Anything else is refused rather than silently defaulted: these choices
    // cannot be changed after Logon.
    env["CPM_OE_CANCEL_ON_DISCONNECT"] = "ALL";
    EXPECT_NE(oe_error_of(env).find("must be N, Y or A"), std::string::npos);
    env.erase("CPM_OE_CANCEL_ON_DISCONNECT");
    env["CPM_OE_SETTLEMENT_REPORTS"] = "1";
    EXPECT_NE(oe_error_of(env).find("must be Y or N"), std::string::npos);
    env["CPM_OE_SETTLEMENT_REPORTS"] = "N";
    // The gateway caps HeartBtInt at 300 s.
    env["CPM_OE_HEARTBEAT"] = "301";
    EXPECT_NE(oe_error_of(env).find("1..300"), std::string::npos);
}

TEST(OeConfigTest, MissingOrMalformedVariablesNameThemselves) {
    for (const char* name : {"CPM_OE_API_KEY", "CPM_OE_PASSPHRASE", "CPM_OE_SECRET", "CPM_OE_HOST",
                             "CPM_OE_PORT"}) {
        Env env = oe_env();
        env.erase(name);
        const auto message = oe_error_of(env);
        EXPECT_NE(message.find(name), std::string::npos) << message;
    }
    Env env = oe_env();
    env["CPM_OE_SECRET"] = "not base64!";
    EXPECT_NE(oe_error_of(env).find("CPM_OE_SECRET is not valid Base64"), std::string::npos);
    env = oe_env();
    env["CPM_OE_PORT"] = "70000";
    EXPECT_NE(oe_error_of(env).find("CPM_OE_PORT"), std::string::npos);
}

// Error messages name variables, never their values.
TEST(OeConfigTest, ErrorsNeverEchoSecrets) {
    Env env = oe_env();
    env["CPM_OE_SECRET"] = "s3cr3t-not-base64!";
    const auto message = oe_error_of(env);
    EXPECT_EQ(message.find("s3cr3t"), std::string::npos) << message;
}

}  // namespace
