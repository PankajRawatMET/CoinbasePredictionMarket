// End-to-end session tests against an in-process QuickFIX acceptor on loopback
// (plain TCP). The acceptor plays the gateway's session role from spec §1-§3:
// it verifies the Logon signature with the spec recipe and rejects bad ones.
#include "cpm/common/signing.hpp"
#include "cpm/fix_common/logon_auth.hpp"
#include "cpm/fix_common/raw_log.hpp"
#include "cpm/fix_common/session_settings.hpp"
#include "cpm/md/md_client.hpp"

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
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

const cpm::Credentials kCreds{"test-key-0001", "test-passphrase", "dGVzdC1zZWNyZXQ="};

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

// Gateway side: verifies Password(554) and the HMAC in Text(58).
class TestGateway : public FIX::Application {
public:
    explicit TestGateway(cpm::Credentials expected) : expected_(std::move(expected)) {}

    std::atomic<int> logons{0};
    std::atomic<int> heartbeats_from_client{0};
    std::atomic<int> logouts_from_client{0};
    std::atomic<bool> saw_reset_flag{false};

    void onCreate(const FIX::SessionID&) override {}
    void onLogon(const FIX::SessionID&) override { ++logons; }
    void onLogout(const FIX::SessionID&) override {}
    void toAdmin(FIX::Message&, const FIX::SessionID&) override {}
    void toApp(FIX::Message&, const FIX::SessionID&) override {}

    // Instruments served to an all-instrument volume/OI subscribe, and an
    // optional MDReqRejReason(281) to answer every MarketDataRequest with instead.
    std::vector<std::pair<std::string, std::string>> instruments;
    std::string reject_reason;
    // The sandbox answers this subscribe with X updates and no W at all
    // (O8), unlike the W per instrument the spec describes.
    bool updates_not_snapshots = false;

    std::vector<std::string> requests() const {
        std::lock_guard lock(mutex_);
        return requests_;
    }

    void fromApp(const FIX::Message& m, const FIX::SessionID& session_id) override {
        if (m.getHeader().getField(FIX::FIELD::MsgType) != FIX::MsgType_MarketDataRequest) return;
        {
            std::lock_guard lock(mutex_);
            requests_.push_back(m.toString());
        }
        const auto req_id = m.getField(FIX::FIELD::MDReqID);
        if (!reject_reason.empty()) {
            FIX::Message y;
            y.getHeader().setField(FIX::FIELD::MsgType, FIX::MsgType_MarketDataRequestReject);
            y.setField(FIX::FIELD::MDReqID, req_id);
            y.setField(FIX::FIELD::MDReqRejReason, reject_reason);
            y.setField(FIX::FIELD::Text, "test reject");
            FIX::Session::sendToTarget(y, session_id);
            return;
        }
        if (m.getField(FIX::FIELD::SubscriptionRequestType) != "1") return;
        if (updates_not_snapshots) {
            send_updates(req_id, session_id);
            return;
        }
        // Spec §4.2: volume/OI answers a subscribe with a W snapshot (one per instrument).
        for (const auto& [symbol, security_id] : instruments) {
            FIX::Message w;
            w.getHeader().setField(FIX::FIELD::MsgType, FIX::MsgType_MarketDataSnapshotFullRefresh);
            w.setField(FIX::FIELD::MDReqID, req_id);
            w.setField(FIX::FIELD::Symbol, symbol);
            w.setField(FIX::FIELD::SecurityID, security_id);
            for (const char* type : {"B", "C"}) {
                FIX::Group entry(FIX::FIELD::NoMDEntries, FIX::FIELD::MDEntryType);
                entry.setField(FIX::FIELD::MDEntryType, type);
                entry.setField(FIX::FIELD::MDEntrySize, "100.00");
                w.addGroup(entry);
            }
            FIX::Session::sendToTarget(w, session_id);
        }
    }

    // The sandbox's layout (O8): one X per instrument, B and C per entry,
    // with Symbol/SecurityID inside the entries and 279=0.
    void send_updates(const std::string& req_id, const FIX::SessionID& session_id) const {
        for (const auto& [symbol, security_id] : instruments) {
            FIX::Message x;
            x.getHeader().setField(FIX::FIELD::MsgType, FIX::MsgType_MarketDataIncrementalRefresh);
            x.setField(FIX::FIELD::MDReqID, req_id);
            for (const char* type : {"B", "C"}) {
                FIX::Group entry(FIX::FIELD::NoMDEntries, FIX::FIELD::MDUpdateAction);
                entry.setField(FIX::FIELD::MDUpdateAction, "0");
                entry.setField(FIX::FIELD::MDEntryType, type);
                entry.setField(FIX::FIELD::Symbol, symbol);
                entry.setField(FIX::FIELD::SecurityID, security_id);
                entry.setField(FIX::FIELD::MDEntrySize, "74");
                x.addGroup(entry);
            }
            x.setField(FIX::FIELD::TransactTime, "20261026-05:02:11.325871");
            FIX::Session::sendToTarget(x, session_id);
        }
    }

    void fromAdmin(const FIX::Message& m, const FIX::SessionID&) override {
        const auto type = m.getHeader().getField(FIX::FIELD::MsgType);
        if (type == FIX::MsgType_Heartbeat) ++heartbeats_from_client;
        if (type == FIX::MsgType_Logout) ++logouts_from_client;
        if (type != FIX::MsgType_Logon) return;

        saw_reset_flag = m.isSetField(FIX::FIELD::ResetSeqNumFlag) &&
                         m.getField(FIX::FIELD::ResetSeqNumFlag) == "Y";
        const auto& h = m.getHeader();
        const auto t = cpm::fix::align_sending_time(h.getField(FIX::FIELD::SendingTime));
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
    std::vector<std::string> requests_;
};

class Gateway {
public:
    Gateway(std::uint16_t port, cpm::Credentials expected)
        : app(std::move(expected)), logger_(sink_), log_(logger_, "") {
        FIX::Dictionary defaults;
        defaults.setString("ConnectionType", "acceptor");
        defaults.setInt("SocketAcceptPort", port);
        defaults.setString("SocketReuseAddress", "Y");
        FIX::Dictionary s;
        s.setString("BeginString", "FIXT.1.1");
        s.setString("DefaultApplVerID", "FIX.5.0SP2");
        s.setString("SenderCompID", "CPMMD");
        s.setString("TargetCompID", kCreds.api_key);
        s.setString("StartTime", "00:00:00");
        s.setString("EndTime", "00:00:00");
        s.setString("UseDataDictionary", "Y");
        s.setString("TransportDataDictionary", std::string(CPM_DICT_DIR) + "/" + cpm::fix::kTransportDictionary);
        s.setString("AppDataDictionary", std::string(CPM_DICT_DIR) + "/" + cpm::fix::kAppDictionary);
        s.setString("ResetOnLogon", "Y");
        s.setString("ResetOnLogout", "Y");
        s.setString("ResetOnDisconnect", "Y");
        s.setInt("TimestampPrecision", 6);
        settings_.set(defaults);
        settings_.set(FIX::SessionID("FIXT.1.1", "CPMMD", kCreds.api_key), s);
        acceptor_ = std::make_unique<FIX::ThreadedSocketAcceptor>(app, store_, settings_, log_);
        acceptor_->start();
    }
    ~Gateway() { acceptor_->stop(); }

    TestGateway app;

private:
    std::ostringstream sink_;
    cpm::Logger logger_;
    cpm::fix::RawLogFactory log_;
    FIX::SessionSettings settings_;
    FIX::MemoryStoreFactory store_;
    std::unique_ptr<FIX::ThreadedSocketAcceptor> acceptor_;
};

cpm::md::MdClientOptions loopback_options(std::uint16_t port, cpm::Credentials creds, int heartbeat) {
    cpm::md::MdClientOptions o;
    o.config.credentials = std::move(creds);
    o.config.endpoint = {"127.0.0.1", port, false, ""};
    o.config.heartbeat_secs = heartbeat;
    o.dict_dir = CPM_DICT_DIR;
    o.reconnect_interval_secs = 1;
    return o;
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

TEST(SessionLoopback, LogsOnHeartbeatsAndLogsOutCleanly) {
    const auto port = free_port();
    Gateway gateway(port, kCreds);

    std::ostringstream client_sink;
    cpm::Logger client_logger(client_sink);
    std::vector<cpm::md::SessionEvent::Kind> events;
    std::mutex events_mutex;
    {
        cpm::md::MdClient client(loopback_options(port, kCreds, 1), client_logger);
        client.on_session_event([&](const cpm::md::SessionEvent& e) {
            std::lock_guard lock(events_mutex);
            events.push_back(e.kind);
        });
        client.start();

        ASSERT_TRUE(client.wait_logged_on(5s)) << client_sink.str();
        EXPECT_TRUE(gateway.app.saw_reset_flag);
        // HeartBtInt=1: the client must heartbeat on its own while idle.
        EXPECT_TRUE(eventually([&] { return gateway.app.heartbeats_from_client >= 2; }, 5s));

        client.stop();
        EXPECT_FALSE(client.logged_on());
        EXPECT_EQ(gateway.app.logouts_from_client, 1);
    }

    {
        std::lock_guard lock(events_mutex);
        ASSERT_EQ(events.size(), 2u);
        EXPECT_EQ(events[0], cpm::md::SessionEvent::Kind::LoggedOn);
        EXPECT_EQ(events[1], cpm::md::SessionEvent::Kind::LoggedOut);
    }

    // Raw traffic is logged with '|' and without any credential.
    const std::string log = client_sink.str();
    EXPECT_NE(log.find(">> 8=FIXT.1.1|"), std::string::npos);
    EXPECT_NE(log.find("|35=A|"), std::string::npos);
    EXPECT_NE(log.find("|554=***|"), std::string::npos);
    EXPECT_NE(log.find("|58=***|"), std::string::npos);
    EXPECT_NE(log.find("|141=Y|"), std::string::npos);
    EXPECT_NE(log.find("|1137=9|"), std::string::npos);
    EXPECT_NE(log.find("|56=CPMMD|"), std::string::npos);
    // The server answering our Logout is routine, not a warning.
    EXPECT_NE(log.find("server confirmed Logout"), std::string::npos);
    EXPECT_EQ(log.find("WARN"), std::string::npos);
    EXPECT_EQ(log.find(kCreds.api_key), std::string::npos);
    EXPECT_EQ(log.find(kCreds.passphrase), std::string::npos);
    EXPECT_EQ(log.find(kCreds.secret_b64), std::string::npos);
}

TEST(SessionLoopback, BadSignatureIsRejectedWithServerReason) {
    const auto port = free_port();
    Gateway gateway(port, kCreds);

    std::ostringstream sink;
    cpm::Logger logger(sink);
    auto wrong = kCreds;
    wrong.secret_b64 = "d3Jvbmctc2VjcmV0";  // "wrong-secret"
    cpm::md::MdClient client(loopback_options(port, wrong, 30), logger);
    client.start();

    EXPECT_FALSE(client.wait_logged_on(2s));
    EXPECT_TRUE(eventually([&] { return !client.last_logout_reason().empty(); }, 3s));
    // The QuickFIX acceptor prefixes the RejectLogon text; the client reports Text(58) verbatim.
    EXPECT_EQ(client.last_logout_reason(), "Rejected Logon Attempt: invalid signature");
    EXPECT_EQ(gateway.app.logons, 0);
    client.stop();
    // A Logout the server starts (here rejecting our Logon) stays a warning.
    EXPECT_NE(sink.str().find("WARN  server sent Logout: Rejected Logon Attempt"), std::string::npos);
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

TEST(SessionLoopback, ListsInstrumentsFromVolumeOiSnapshots) {
    const auto port = free_port();
    Gateway gateway(port, kCreds);
    gateway.app.instruments = {{"NFL-B", "200002"}, {"NFL-A", "200001"}};

    std::ostringstream sink;
    cpm::Logger logger(sink);
    cpm::md::MdClient client(loopback_options(port, kCreds, 30), logger);
    client.start();
    ASSERT_TRUE(client.wait_logged_on(5s)) << sink.str();

    const auto list = client.list_instruments(300ms, 5s);
    EXPECT_TRUE(list.reject.empty()) << list.reject;
    ASSERT_EQ(list.instruments.size(), 2u);
    EXPECT_EQ(list.instruments[0].symbol, "NFL-A");
    EXPECT_EQ(list.instruments[0].security_id, "200001");
    EXPECT_EQ(list.instruments[1].symbol, "NFL-B");

    // Subscribe then unsubscribe, both shaped like spec §4: B+C, all instruments, no 264.
    ASSERT_TRUE(eventually([&] { return gateway.app.requests().size() == 2; }, 3s));
    const auto requests = gateway.app.requests();
    for (const auto& r : requests) {
        EXPECT_FALSE(contains(r, "\x01" "264=")) << r;
        EXPECT_FALSE(contains(r, "\x01" "1128=")) << r;
    }
    client.stop();
    // Wire order, from the client's raw log (the acceptor re-serializes in tag order).
    const auto log = sink.str();
    EXPECT_TRUE(contains(log, "|262=volume-oi-1|263=1|267=2|269=B|269=C|146=0|10=")) << log;
    EXPECT_TRUE(contains(log, "|262=volume-oi-1|263=2|267=2|269=B|269=C|146=0|10=")) << log;
}

// O8: the live gateway answers the discovery subscribe with X updates, not W.
TEST(SessionLoopback, ListsInstrumentsFromVolumeOiUpdates) {
    const auto port = free_port();
    Gateway gateway(port, kCreds);
    gateway.app.updates_not_snapshots = true;
    gateway.app.instruments = {{"ETHD15M-ETH26OCT260515-0", "240338655015600128"},
                               {"BTCD15M-BTC26OCT260515-0", "240338655317590016"}};

    std::ostringstream sink;
    cpm::Logger logger(sink);
    cpm::md::MdClient client(loopback_options(port, kCreds, 30), logger);
    cpm::md::MdCallbacks callbacks;
    std::atomic<int> published{0};
    callbacks.volume = [&](const cpm::md::Volume&) { ++published; };
    callbacks.decode_error = [&](const cpm::md::DecodeError& e) { ADD_FAILURE() << e.reason << " in " << e.raw; };
    client.on_market_data(callbacks);
    client.start();
    ASSERT_TRUE(client.wait_logged_on(5s)) << sink.str();

    // Settles on new instruments going quiet, well inside the timeout.
    const auto list = client.list_instruments(300ms, 5s);
    EXPECT_TRUE(list.reject.empty()) << list.reject;
    ASSERT_EQ(list.instruments.size(), 2u) << sink.str();
    EXPECT_EQ(list.instruments[0].symbol, "BTCD15M-BTC26OCT260515-0");
    EXPECT_EQ(list.instruments[0].security_id, "240338655317590016");
    EXPECT_EQ(list.instruments[1].symbol, "ETHD15M-ETH26OCT260515-0");
    // Discovery's own traffic is internal: it is not published to callbacks.
    EXPECT_EQ(published.load(), 0);
    client.stop();
}

TEST(SessionLoopback, InstrumentListReportsReject) {
    const auto port = free_port();
    Gateway gateway(port, kCreds);
    gateway.app.reject_reason = "9";

    std::ostringstream sink;
    cpm::Logger logger(sink);
    cpm::md::MdClient client(loopback_options(port, kCreds, 30), logger);
    client.start();
    ASSERT_TRUE(client.wait_logged_on(5s)) << sink.str();

    const auto list = client.list_instruments(300ms, 5s);
    EXPECT_EQ(list.reject, "MarketDataRequestReject 262=volume-oi-1 281=9: test reject");
    EXPECT_TRUE(list.instruments.empty());
    // No unsubscribe after a reject.
    EXPECT_EQ(gateway.app.requests().size(), 1u);
    client.stop();
}

TEST(SessionLoopback, ListInstrumentsRequiresLogon) {
    std::ostringstream sink;
    cpm::Logger logger(sink);
    cpm::md::MdClient client(loopback_options(free_port(), kCreds, 30), logger);
    EXPECT_THROW(client.list_instruments(100ms, 100ms), std::runtime_error);
}

// Spec §4 subscribe shapes for one instrument, and unsubscribes that repeat them.
TEST(SessionLoopback, SubscribesAndUnsubscribesPerSpec) {
    const auto port = free_port();
    Gateway gateway(port, kCreds);

    std::ostringstream sink;
    cpm::Logger logger(sink);
    cpm::md::MdClient client(loopback_options(port, kCreds, 30), logger);
    client.start();
    ASSERT_TRUE(client.wait_logged_on(5s)) << sink.str();

    const std::vector<std::string> sym{"BTCD15M-TEST-0"};
    EXPECT_EQ(client.subscribe({cpm::md::Channel::Book, sym, 10}), "book-1");
    EXPECT_EQ(client.subscribe({cpm::md::Channel::Trades, sym, 0}), "trades-2");
    EXPECT_EQ(client.subscribe({cpm::md::Channel::Lifecycle, sym, 0}), "lifecycle-3");
    EXPECT_TRUE(client.unsubscribe("trades-2"));
    EXPECT_FALSE(client.unsubscribe("trades-2"));
    client.unsubscribe_all();
    ASSERT_TRUE(eventually([&] { return gateway.app.requests().size() == 6; }, 3s));
    client.stop();

    const auto log = sink.str();
    for (const char* action : {"1", "2"}) {
        const std::string a = action;
        EXPECT_TRUE(contains(log, "|262=book-1|263=" + a + "|264=10|266=Y|267=2|269=0|269=1|146=1|55=BTCD15M-TEST-0|10="))
            << log;
        EXPECT_TRUE(contains(log, "|262=trades-2|263=" + a + "|267=1|269=2|146=1|55=BTCD15M-TEST-0|10=")) << log;
        EXPECT_TRUE(contains(log, "|262=lifecycle-3|263=" + a + "|267=1|269=L|146=1|55=BTCD15M-TEST-0|10=")) << log;
    }
}

// Spec §4 all-instrument example: 146=0 and no Symbol.
TEST(SessionLoopback, SubscribesAllInstruments) {
    const auto port = free_port();
    Gateway gateway(port, kCreds);

    std::ostringstream sink;
    cpm::Logger logger(sink);
    cpm::md::MdClient client(loopback_options(port, kCreds, 30), logger);
    client.start();
    ASSERT_TRUE(client.wait_logged_on(5s)) << sink.str();

    EXPECT_EQ(client.subscribe({cpm::md::Channel::Lifecycle, {}, 0}), "lifecycle-1");
    EXPECT_EQ(client.subscribe({cpm::md::Channel::Trades, {}, 0}), "trades-2");
    ASSERT_TRUE(eventually([&] { return gateway.app.requests().size() == 2; }, 3s));
    client.stop();

    const auto log = sink.str();
    EXPECT_TRUE(contains(log, "|262=lifecycle-1|263=1|267=1|269=L|146=0|10=")) << log;
    EXPECT_TRUE(contains(log, "|262=trades-2|263=1|267=1|269=2|146=0|10=")) << log;
}

TEST(SessionLoopback, SubscribeRejectsRequestsTheSpecForbids) {
    std::ostringstream sink;
    cpm::Logger logger(sink);
    cpm::md::MdClient client(loopback_options(free_port(), kCreds, 30), logger);
    using cpm::md::Channel;
    EXPECT_THROW(client.subscribe({Channel::Book, {}, 10}), std::invalid_argument);         // 281=9
    EXPECT_THROW(client.subscribe({Channel::Book, {"X"}, 0}), std::invalid_argument);       // 281=5
    EXPECT_THROW(client.subscribe({Channel::Book, {"X"}, 5}), std::invalid_argument);       // 281=5
    EXPECT_THROW(client.subscribe({Channel::Trades, {"X"}, 1}), std::invalid_argument);     // 281=5
    EXPECT_THROW(client.subscribe({Channel::Trades, {""}, 0}), std::invalid_argument);
    EXPECT_THROW(client.subscribe({Channel::Trades, {"X"}, 0}), std::runtime_error);        // not logged on
}

TEST(SessionLoopback, RefusesNonSandboxHost) {
    std::ostringstream sink;
    cpm::Logger logger(sink);
    auto options = loopback_options(6120, kCreds, 30);
    options.config.endpoint = {"fix-marketdata.example.com", 6120, true, "/etc/ssl/certs/ca-certificates.crt"};
    EXPECT_THROW(cpm::md::MdClient(std::move(options), logger), cpm::ConfigError);
}

}  // namespace
