// The order entry session against an in-process QuickFIX acceptor on loopback
// (plain TCP). The acceptor plays the gateway's session role: it verifies the
// Logon signature with the spec's recipe against TargetCompID=CPMOE, and
// records the Logon body so the session options can be asserted.
#include "cpm/common/signing.hpp"
#include "cpm/fix_common/logon_auth.hpp"
#include "cpm/fix_common/raw_log.hpp"
#include "cpm/fix_common/session_settings.hpp"
#include "cpm/oms/oe_client.hpp"

#include <quickfix/Application.h>
#include <quickfix/FixFieldNumbers.h>
#include <quickfix/FixValues.h>
#include <quickfix/MessageStore.h>
#include <quickfix/Session.h>
#include <quickfix/ThreadedSocketAcceptor.h>

#include <gtest/gtest.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;

const cpm::Credentials kCreds{"oe-test-key", "oe-test-passphrase", "b2UtdGVzdC1zZWNyZXQ="};

constexpr int kCancelOrdersOnDisconnect = 8013;
constexpr int kReceiveSettlementReports = 20127;

std::uint16_t free_port() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    socklen_t len = sizeof(addr);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    ::close(fd);
    return ntohs(addr.sin_port);
}

template <typename Pred>
bool eventually(Pred pred, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(20ms);
    }
    return pred();
}

// Gateway side: verifies Password(554) and the HMAC in Text(58) exactly as the
// spec describes, then keeps the Logon for inspection.
class TestGateway : public FIX::Application {
public:
    explicit TestGateway(cpm::Credentials expected) : expected_(std::move(expected)) {}

    std::atomic<int> logons{0};
    std::atomic<int> heartbeats_from_client{0};
    std::atomic<int> logouts_from_client{0};
    // Set to send Logout with this text right after a successful Logon.
    std::string logout_after_logon;

    std::string logon() const {
        std::lock_guard lock(mutex_);
        return logon_;
    }

    void onCreate(const FIX::SessionID&) override {}
    void onLogon(const FIX::SessionID& session_id) override {
        ++logons;
        std::string text;
        {
            std::lock_guard lock(mutex_);
            text = logout_after_logon;
        }
        if (text.empty()) return;
        // The gateway's wording when a newer connection takes over.
        FIX::Message logout;
        logout.getHeader().setField(FIX::FIELD::MsgType, FIX::MsgType_Logout);
        logout.setField(FIX::FIELD::Text, text);
        FIX::Session::sendToTarget(logout, session_id);
    }
    void onLogout(const FIX::SessionID&) override {}
    void toAdmin(FIX::Message&, const FIX::SessionID&) override {}
    void toApp(FIX::Message&, const FIX::SessionID&) override {}
    void fromApp(const FIX::Message&, const FIX::SessionID&) override {}

    void fromAdmin(const FIX::Message& m, const FIX::SessionID&) override {
        const auto type = m.getHeader().getField(FIX::FIELD::MsgType);
        if (type == FIX::MsgType_Heartbeat) ++heartbeats_from_client;
        if (type == FIX::MsgType_Logout) ++logouts_from_client;
        if (type != FIX::MsgType_Logon) return;
        {
            std::lock_guard lock(mutex_);
            logon_ = m.toString();
        }
        const auto& h = m.getHeader();
        const auto t = cpm::fix::align_sending_time(h.getField(FIX::FIELD::SendingTime));
        // payload = SendingTime(ms) + ApiKey + TargetCompID + Password, and the
        // TargetCompID here is CPMOE, not CPMMD.
        const auto expected_sig = cpm::fix_logon_signature(
            t.signed_, h.getField(FIX::FIELD::SenderCompID), h.getField(FIX::FIELD::TargetCompID),
            expected_.passphrase, expected_.secret_b64);
        if (!m.isSetField(FIX::FIELD::Password) || m.getField(FIX::FIELD::Password) != expected_.passphrase) {
            throw FIX::RejectLogon("invalid passphrase");
        }
        if (!m.isSetField(FIX::FIELD::Text) || m.getField(FIX::FIELD::Text) != expected_sig) {
            throw FIX::RejectLogon("invalid signature");
        }
    }

private:
    cpm::Credentials expected_;
    mutable std::mutex mutex_;
    std::string logon_;
};

class Gateway {
public:
    Gateway(std::uint16_t port, const cpm::Credentials& expected,
            const std::string& comp_id = std::string(cpm::kOeTargetCompId))
        : app(expected),
          settings(cpm::fix::make_oe_acceptor_settings(expected.api_key, port, CPM_DICT_DIR, comp_id)),
          log(logger, expected.api_key),
          acceptor(app, store, settings, log) {
        acceptor.start();
    }
    ~Gateway() { acceptor.stop(true); }

    TestGateway app;
    std::ostringstream sink;
    cpm::Logger logger{sink};
    FIX::SessionSettings settings;
    FIX::MemoryStoreFactory store;
    cpm::fix::RawLogFactory log;
    FIX::ThreadedSocketAcceptor acceptor;
};

cpm::oms::OeClientOptions loopback_options(std::uint16_t port, const cpm::Credentials& creds) {
    cpm::oms::OeClientOptions o;
    o.config.credentials = creds;
    o.config.endpoint = {"127.0.0.1", port, false, ""};
    o.config.heartbeat_secs = 30;
    o.dict_dir = CPM_DICT_DIR;
    o.reconnect_interval_secs = 1;
    return o;
}

TEST(OeSession, LogsOnWithASignedLogonAndLogsOutCleanly) {
    const auto port = free_port();
    Gateway gateway(port, kCreds);

    std::ostringstream sink;
    cpm::Logger logger(sink);
    std::vector<cpm::oms::SessionEvent::Kind> events;
    std::mutex events_mutex;
    {
        cpm::oms::OeClient client(loopback_options(port, kCreds), logger);
        client.on_session_event([&](const cpm::oms::SessionEvent& e) {
            std::lock_guard lock(events_mutex);
            events.push_back(e.kind);
        });
        client.start();
        ASSERT_TRUE(client.wait_logged_on(5s)) << sink.str();
        EXPECT_TRUE(client.logged_on());
        EXPECT_FALSE(client.replaced());
        client.stop();
    }
    EXPECT_EQ(gateway.app.logons.load(), 1);
    EXPECT_GE(gateway.app.logouts_from_client.load(), 1);
    {
        std::lock_guard lock(events_mutex);
        ASSERT_GE(events.size(), 2u);
        EXPECT_EQ(events.front(), cpm::oms::SessionEvent::Kind::LoggedOn);
        EXPECT_EQ(events.back(), cpm::oms::SessionEvent::Kind::LoggedOut);
    }
    // The key is redacted in the log, and the secret never appears.
    const auto text = sink.str();
    EXPECT_NE(text.find("oe-t****"), std::string::npos) << text;
    EXPECT_EQ(text.find(kCreds.secret_b64), std::string::npos);
    EXPECT_EQ(text.find(kCreds.passphrase), std::string::npos);
}

// The Logon must carry what the gateway only reads at Logon.
TEST(OeSession, LogonCarriesTheSessionOptionsAndTheSpecsFields) {
    const auto port = free_port();
    Gateway gateway(port, kCreds);

    std::ostringstream sink;
    cpm::Logger logger(sink);
    auto options = loopback_options(port, kCreds);
    options.config.cancel_on_disconnect = cpm::CancelOnDisconnect::ApiKey;
    options.config.settlement_reports = true;
    cpm::oms::OeClient client(std::move(options), logger);
    client.start();
    ASSERT_TRUE(client.wait_logged_on(5s)) << sink.str();

    FIX::Message logon(gateway.app.logon(), false);
    EXPECT_EQ(logon.getHeader().getField(FIX::FIELD::TargetCompID), "CPMOE");
    EXPECT_EQ(logon.getHeader().getField(FIX::FIELD::SenderCompID), kCreds.api_key);
    EXPECT_EQ(logon.getField(FIX::FIELD::EncryptMethod), "0");
    EXPECT_EQ(logon.getField(FIX::FIELD::HeartBtInt), "30");
    EXPECT_EQ(logon.getField(FIX::FIELD::ResetSeqNumFlag), "Y");
    EXPECT_EQ(logon.getField(FIX::FIELD::DefaultApplVerID), "9");
    EXPECT_EQ(logon.getField(FIX::FIELD::Password), kCreds.passphrase);
    // 44-character base64 of a 32-byte HMAC-SHA256 digest.
    EXPECT_EQ(logon.getField(FIX::FIELD::Text).size(), 44u);
    EXPECT_EQ(logon.getField(kCancelOrdersOnDisconnect), "A");
    EXPECT_EQ(logon.getField(kReceiveSettlementReports), "Y");
    // SendingTime is sent at microsecond precision with zero sub-millisecond
    // digits, so the signed millisecond value and the wire value agree.
    const auto sending_time = logon.getHeader().getField(FIX::FIELD::SendingTime);
    ASSERT_EQ(sending_time.size(), 24u) << sending_time;
    EXPECT_EQ(sending_time.substr(21), "000");
    client.stop();
}

TEST(OeSession, EachCancelOnDisconnectScopeIsSentVerbatim) {
    for (const auto scope : {cpm::CancelOnDisconnect::None, cpm::CancelOnDisconnect::Session,
                             cpm::CancelOnDisconnect::ApiKey}) {
        const auto port = free_port();
        Gateway gateway(port, kCreds);
        std::ostringstream sink;
        cpm::Logger logger(sink);
        auto options = loopback_options(port, kCreds);
        options.config.cancel_on_disconnect = scope;
        options.config.settlement_reports = false;
        cpm::oms::OeClient client(std::move(options), logger);
        client.start();
        ASSERT_TRUE(client.wait_logged_on(5s)) << sink.str();

        FIX::Message logon(gateway.app.logon(), false);
        EXPECT_EQ(logon.getField(kCancelOrdersOnDisconnect), std::string(1, static_cast<char>(scope)));
        EXPECT_EQ(logon.getField(kReceiveSettlementReports), "N");
        client.stop();
    }
}

TEST(OeSession, ABadSignatureIsRejectedWithTheServersReason) {
    const auto port = free_port();
    Gateway gateway(port, kCreds);

    std::ostringstream sink;
    cpm::Logger logger(sink);
    cpm::Credentials wrong = kCreds;
    wrong.secret_b64 = "d3Jvbmctc2VjcmV0";  // valid base64, wrong secret
    cpm::oms::OeClient client(loopback_options(port, wrong), logger);
    client.start();
    EXPECT_FALSE(client.wait_logged_on(3s));
    EXPECT_TRUE(eventually([&] { return client.last_logout_reason().find("invalid signature") !=
                                        std::string::npos; }, 3s))
        << "reason: '" << client.last_logout_reason() << "'\n" << sink.str();
    client.stop();
}

// A second Logon for the same API key displaces this session, and the gateway
// says so. Cancel-on-disconnect does NOT fire, so the orders stay live.
TEST(OeSession, DisplacedByANewerConnection) {
    const auto port = free_port();
    Gateway gateway(port, kCreds);
    gateway.app.logout_after_logon = "replaced by newer connection";

    std::ostringstream sink;
    cpm::Logger logger(sink);
    std::atomic<int> replaced_events{0};
    cpm::oms::OeClient client(loopback_options(port, kCreds), logger);
    client.on_session_event([&](const cpm::oms::SessionEvent& e) {
        if (e.kind == cpm::oms::SessionEvent::Kind::Replaced) ++replaced_events;
    });
    client.start();
    ASSERT_TRUE(client.wait_logged_on(5s)) << sink.str();

    EXPECT_TRUE(eventually([&] { return client.replaced(); }, 5s)) << sink.str();
    EXPECT_TRUE(eventually([&] { return replaced_events.load() > 0; }, 2s));
    EXPECT_NE(client.last_logout_reason().find("replaced by newer connection"), std::string::npos);
    // Logged loudly, because resting orders survive and now belong elsewhere.
    EXPECT_NE(sink.str().find("resting orders are NOT canceled"), std::string::npos) << sink.str();
    client.stop();
}

// The guard is per service: the order entry client refuses the market data host
// and anything else that is not its own sandbox.
TEST(OeSession, RefusesAnyHostButItsOwnSandbox) {
    std::ostringstream sink;
    cpm::Logger logger(sink);
    for (const char* host : {"fix-marketdata.cpm-sandbox.fairx.net", "fix-order.example.com",
                             "cpm.fairx.net"}) {
        auto options = loopback_options(6110, kCreds);
        options.config.endpoint = {host, 6110, true, std::string(cpm::kDefaultCaFile)};
        EXPECT_THROW(cpm::oms::OeClient(std::move(options), logger), cpm::ConfigError) << host;
    }
    // Its own sandbox host is accepted, over TLS.
    auto ok = loopback_options(6110, kCreds);
    ok.config.endpoint = {std::string(cpm::kOeSandboxHost), 6110, true, std::string(cpm::kDefaultCaFile)};
    EXPECT_NO_THROW(cpm::oms::OeClient(std::move(ok), logger));
    // But not in the clear.
    auto plain = loopback_options(6110, kCreds);
    plain.config.endpoint = {std::string(cpm::kOeSandboxHost), 6110, false, ""};
    EXPECT_THROW(cpm::oms::OeClient(std::move(plain), logger), cpm::ConfigError);
}

// TargetCompID comes from the config, and the signature is computed over
// whatever it says: the gateway here verifies with the same value, so a
// non-default comp id must log on exactly as CPMOE does.
TEST(OeSession, TheConfiguredTargetCompIdIsUsedOnTheWireAndInTheSignature) {
    const std::string comp_id = "CPMOE-ALT";
    const auto port = free_port();
    Gateway gateway(port, kCreds, comp_id);

    std::ostringstream sink;
    cpm::Logger logger(sink);
    auto options = loopback_options(port, kCreds);
    options.config.target_comp_id = comp_id;
    cpm::oms::OeClient client(std::move(options), logger);
    client.start();
    ASSERT_TRUE(client.wait_logged_on(5s)) << sink.str();

    FIX::Message logon(gateway.app.logon(), false);
    EXPECT_EQ(logon.getHeader().getField(FIX::FIELD::TargetCompID), comp_id);
    client.stop();
}

// And the signature is NOT computed over the default: a client signing for one
// comp id cannot log on to a gateway expecting another.
TEST(OeSession, AMismatchedTargetCompIdCannotLogOn) {
    const auto port = free_port();
    Gateway gateway(port, kCreds, "CPMOE-ALT");

    std::ostringstream sink;
    cpm::Logger logger(sink);
    auto options = loopback_options(port, kCreds);  // signs for CPMOE
    cpm::oms::OeClient client(std::move(options), logger);
    client.start();
    EXPECT_FALSE(client.wait_logged_on(3s)) << sink.str();
    client.stop();
}

TEST(OeSession, HeartbeatsKeepTheSessionUp) {
    const auto port = free_port();
    Gateway gateway(port, kCreds);

    std::ostringstream sink;
    cpm::Logger logger(sink);
    auto options = loopback_options(port, kCreds);
    options.config.heartbeat_secs = 1;
    cpm::oms::OeClient client(std::move(options), logger);
    client.start();
    ASSERT_TRUE(client.wait_logged_on(5s)) << sink.str();
    EXPECT_TRUE(eventually([&] { return gateway.app.heartbeats_from_client.load() >= 2; }, 6s))
        << sink.str();
    EXPECT_TRUE(client.logged_on());
    client.stop();
}

TEST(OeSession, StopIsIdempotentAndSafeBeforeStart) {
    const auto port = free_port();
    Gateway gateway(port, kCreds);
    std::ostringstream sink;
    cpm::Logger logger(sink);
    cpm::oms::OeClient client(loopback_options(port, kCreds), logger);
    EXPECT_NO_THROW(client.stop());  // never started
    client.start();
    ASSERT_TRUE(client.wait_logged_on(5s)) << sink.str();
    client.stop();
    EXPECT_NO_THROW(client.stop());
    EXPECT_FALSE(client.logged_on());
}

}  // namespace
