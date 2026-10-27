// md_client against md_sim over loopback: logon, subscription validation,
// snapshot-then-incremental in the sandbox's layouts, lifecycle rolls, and the
// three fault flags. Assertions read the client's raw message log.
#include "cpm/md/md_client.hpp"
#include "cpm/md/sim/simulator.hpp"

#include "test_util.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <iterator>
#include <regex>

namespace {

using namespace std::chrono_literals;
using cpm::md::Channel;
using cpm::md::test::contains;
using cpm::md::test::eventually;
using cpm::md::test::free_port;
using cpm::md::test::SharedSink;

const cpm::Credentials kCreds{"sim-test-key", "sim-pass", "c2ltLXRlc3Qtc2VjcmV0LW5vdC1yZWFs"};
const std::string kHom = "SIMGAME-26NOV01HOMAWY-HOM";

int count(const std::string& text, const std::string& needle) {
    int n = 0;
    for (auto pos = text.find(needle); pos != std::string::npos; pos = text.find(needle, pos + 1)) ++n;
    return n;
}

int count_regex(const std::string& text, const std::string& pattern) {
    const std::regex re(pattern);
    return static_cast<int>(std::distance(std::sregex_iterator(text.begin(), text.end(), re), std::sregex_iterator()));
}

// One simulator and one client, each with its own log.
struct Rig {
    explicit Rig(cpm::md::sim::SimOptions sim_options = {}, cpm::Credentials client_creds = kCreds)
        : port(free_port()),
          sim_logger(sim_log),
          client_logger(client_log) {
        sim_options.credentials = kCreds;
        sim_options.port = port;
        sim_options.dict_dir = CPM_DICT_DIR;
        if (sim_options.events_per_sec == 5.0) sim_options.events_per_sec = 50;
        sim = std::make_unique<cpm::md::sim::Simulator>(std::move(sim_options), sim_logger);
        sim->start();

        cpm::md::MdClientOptions o;
        o.config.credentials = std::move(client_creds);
        o.config.endpoint = {"127.0.0.1", port, false, ""};
        o.config.heartbeat_secs = 30;
        o.dict_dir = CPM_DICT_DIR;
        o.reconnect_interval_secs = 1;
        client = std::make_unique<cpm::md::MdClient>(std::move(o), client_logger);
        client->start();
    }
    ~Rig() {
        client->stop();
        sim->stop();
    }

    // Wait for `needle` in the client log; on timeout, fail with the log.
    bool saw(const std::string& needle, std::chrono::milliseconds timeout = 5s) {
        return eventually([&] { return contains(client_log.text(), needle); }, timeout);
    }

    std::uint16_t port;
    SharedSink sim_log;
    SharedSink client_log;
    cpm::Logger sim_logger;
    cpm::Logger client_logger;
    std::unique_ptr<cpm::md::sim::Simulator> sim;
    std::unique_ptr<cpm::md::MdClient> client;
};

TEST(SimSession, LogsOnWithSignedLogon) {
    Rig rig;
    ASSERT_TRUE(rig.client->wait_logged_on(5s)) << rig.client_log.text();
    EXPECT_TRUE(rig.sim->wait_logged_on(1s));
    // Server header as on the sandbox [OBS]: SenderSubID 50=TEST.
    EXPECT_TRUE(contains(rig.client_log.text(), "|50=TEST|"));
    // Logout reply as on the sandbox [OBS].
    rig.client->stop();
    EXPECT_EQ(rig.client->last_logout_reason(), "CLIENT_LOGOUT");
}

TEST(SimSession, RejectsBadSignature) {
    auto wrong = kCreds;
    wrong.secret_b64 = "d3Jvbmctc2VjcmV0";
    Rig rig({}, wrong);
    EXPECT_FALSE(rig.client->wait_logged_on(2s));
    EXPECT_TRUE(eventually([&] { return !rig.client->last_logout_reason().empty(); }, 3s));
    EXPECT_TRUE(contains(rig.client->last_logout_reason(), "invalid signature")) << rig.client->last_logout_reason();
    EXPECT_EQ(rig.sim->logons(), 0);
}

// Book L2: a W with levels on both sides, then X deltas in the observed layout:
// 262|268|279|269|55|48|[1023]|270|271 ... |60 [OBS O3].
TEST(SimSession, BookSnapshotThenIncrementals) {
    Rig rig;
    ASSERT_TRUE(rig.client->wait_logged_on(5s));
    rig.client->subscribe({Channel::Book, {kHom}, 10});
    ASSERT_TRUE(rig.saw("|35=X|")) << rig.client_log.text();
    ASSERT_TRUE(eventually([&] { return count(rig.client_log.text(), "|262=book-1|268=") >= 6; }, 5s));

    const auto log = rig.client_log.text();
    const auto w = log.find("|35=W|");
    ASSERT_NE(w, std::string::npos);
    EXPECT_LT(w, log.find("|35=X|")) << "W must precede X";
    EXPECT_TRUE(std::regex_search(log, std::regex("\\|35=W\\|.*\\|262=book-1\\|55=" + kHom +
                                                  "\\|48=700000000000000001\\|268=[0-9]+\\|269=0\\|270=0\\.[0-9]+"
                                                  "\\|271=[0-9.]+\\|1023=1\\|.*\\|269=1\\|.*\\|60=[0-9]{8}-[0-9:.]{15}\\|10=")))
        << log;
    EXPECT_GT(count_regex(log, "\\|262=book-1\\|268=1\\|279=[01]\\|269=[01]\\|55=" + kHom +
                                   "\\|48=700000000000000001\\|1023=[0-9]+\\|270=0\\.[0-9]+\\|271=[0-9.]+\\|60="),
              0)
        << log;
    // No session-level rejects from the client: every message passed its dictionary.
    EXPECT_FALSE(contains(log, "|35=3|"));
}

// L1 only ever talks about level 1, and a moved best price is Delete + New.
TEST(SimSession, L1BookStaysAtLevelOne) {
    Rig rig;
    ASSERT_TRUE(rig.client->wait_logged_on(5s));
    rig.client->subscribe({Channel::Book, {kHom}, 1});
    ASSERT_TRUE(eventually([&] { return count(rig.client_log.text(), "|262=book-1|268=") >= 8; }, 8s));
    const auto log = rig.client_log.text();
    EXPECT_EQ(count_regex(log, "\\|262=book-1\\|.*\\|1023=[02-9]"), 0) << log;
    EXPECT_TRUE(std::regex_search(log, std::regex("\\|35=W\\|.*\\|262=book-1\\|.*\\|268=[0-2]\\|")));
}

TEST(SimSession, RejectsUnknownSymbolWithSandboxText) {
    Rig rig;
    ASSERT_TRUE(rig.client->wait_logged_on(5s));
    rig.client->subscribe({Channel::Book, {"NOPE-0"}, 10});
    EXPECT_TRUE(rig.saw("MarketDataRequestReject 262=book-1 281=0: unknown symbol=NOPE-0")) << rig.client_log.text();
    rig.client->unsubscribe("book-1");
    EXPECT_TRUE(rig.saw("MarketDataRequestReject 262=book-1 281=0: no subscription for mdReqId=book-1"));
}

// Trades: no snapshot, then 279=0|269=2|55|48|270|271|278|2446 [OBS O4].
// Volume/OI: a W per instrument (spec §4.2), then B/C deltas.
TEST(SimSession, TradesAndVolumeForAllInstruments) {
    Rig rig;
    ASSERT_TRUE(rig.client->wait_logged_on(5s));
    rig.client->subscribe({Channel::Trades, {}, 0});
    rig.client->subscribe({Channel::VolumeOi, {}, 0});
    ASSERT_TRUE(eventually([&] { return count(rig.client_log.text(), "|262=trades-1|") >= 3; }, 8s));
    ASSERT_TRUE(rig.saw("|262=volume-oi-2|268=2|279=1|269=B|"));
    const auto log = rig.client_log.text();
    EXPECT_GT(count_regex(log, "\\|262=trades-1\\|268=1\\|279=0\\|269=2\\|55=SIMGAME-[A-Z0-9-]+\\|48=[0-9]+"
                               "\\|270=0\\.[0-9]+\\|271=[0-9.]+\\|278=[0-9]+\\|2446=[12]\\|60="),
              0)
        << log;
    EXPECT_EQ(count(log, "|35=W|"), 3);  // one per instrument, none for trades
    EXPECT_TRUE(contains(log, "|262=volume-oi-2|55=SIMGAME-26NOV01HOMAWY-HOM|48=700000000000000001|268=2|269=B|"));
}

// Rolling window: Closed, Determined, next Listed and Open, then Settled,
// as f 55|48|326|60 [OBS O6]; the settled instrument becomes unknown.
TEST(SimSession, LifecycleRoll) {
    cpm::md::sim::SimOptions options;
    options.roll_secs = 1;
    options.settle_delay = 300ms;
    Rig rig(options);
    ASSERT_TRUE(rig.client->wait_logged_on(5s));
    rig.client->subscribe({Channel::Lifecycle, {}, 0});
    ASSERT_TRUE(rig.saw("|326=28|", 5s)) << rig.client_log.text();

    const auto log = rig.client_log.text();
    const std::regex status("\\|35=f\\|.*?\\|55=(SIMROLL-SIM[0-9A-Z]+-0)\\|48=[0-9]+\\|326=([0-9]+)\\|60=");
    std::vector<std::pair<std::string, std::string>> seen;
    for (auto it = std::sregex_iterator(log.begin(), log.end(), status); it != std::sregex_iterator(); ++it) {
        seen.emplace_back((*it)[1], (*it)[2]);
    }
    ASSERT_GE(seen.size(), 5u) << log;
    const auto first = seen[0].first;
    EXPECT_EQ(seen[0], std::make_pair(first, std::string("18")));
    EXPECT_EQ(seen[1], std::make_pair(first, std::string("27")));
    EXPECT_NE(seen[2].first, first);
    EXPECT_EQ(seen[2].second, "21");
    EXPECT_EQ(seen[3].second, "17");
    const auto settled = std::find(seen.begin(), seen.end(), std::make_pair(first, std::string("28")));
    EXPECT_NE(settled, seen.end());

    rig.client->subscribe({Channel::Book, {first}, 10});
    EXPECT_TRUE(rig.saw("unknown symbol=" + first));
}

// --drop-seq: the client sees a gap and asks for a resend; the simulator
// answers with a gap fill (nothing persisted).
TEST(SimSession, DropSeqLeavesGap) {
    cpm::md::sim::SimOptions options;
    options.drop_seq = 6;
    Rig rig(options);
    ASSERT_TRUE(rig.client->wait_logged_on(5s));
    rig.client->subscribe({Channel::Book, {kHom}, 10});
    ASSERT_TRUE(rig.saw("|35=2|")) << rig.client_log.text();  // ResendRequest from the client
    EXPECT_TRUE(rig.saw("|35=4|"));                             // SequenceReset (gap fill)
    EXPECT_TRUE(eventually([&] { return contains(rig.sim_log.text(), "dropped outbound MsgSeqNum 6"); }, 1s));
    EXPECT_TRUE(rig.client->logged_on());
}

// --disconnect-after: the connection drops without a Logout; the client
// reconnects and logs on again.
TEST(SimSession, DisconnectAfterDropsConnection) {
    cpm::md::sim::SimOptions options;
    options.disconnect_after = 5;
    Rig rig(options);
    ASSERT_TRUE(rig.client->wait_logged_on(5s));
    rig.client->subscribe({Channel::Book, {kHom}, 10});
    ASSERT_TRUE(rig.saw("session logged out: session disconnected")) << rig.client_log.text();
    EXPECT_TRUE(eventually([&] { return rig.sim->logons() >= 2; }, 5s));
    EXPECT_EQ(count(rig.client_log.text(), "|35=5|"), 0) << "no Logout on a dropped connection";
}

// --stale-after: existing subscriptions go silent while heartbeats continue;
// a new subscription gets a fresh snapshot and data again.
TEST(SimSession, StaleAfterSilencesExistingSubscriptions) {
    cpm::md::sim::SimOptions options;
    options.stale_after_secs = 1;
    Rig rig(options);
    ASSERT_TRUE(rig.client->wait_logged_on(5s));
    rig.client->subscribe({Channel::Book, {kHom}, 10});
    ASSERT_TRUE(eventually([&] { return contains(rig.sim_log.text(), "feed stale"); }, 3s));
    std::this_thread::sleep_for(100ms);
    const int before = count(rig.client_log.text(), "|262=book-1|");
    std::this_thread::sleep_for(1s);
    EXPECT_EQ(count(rig.client_log.text(), "|262=book-1|"), before) << "stale subscription still receiving";
    EXPECT_TRUE(rig.client->logged_on());

    rig.client->subscribe({Channel::Book, {kHom}, 10});
    EXPECT_TRUE(rig.saw("|35=W|"));
    EXPECT_TRUE(eventually([&] { return count(rig.client_log.text(), "|262=book-2|268=") >= 3; }, 5s));
}

}  // namespace
