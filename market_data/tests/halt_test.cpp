// Stage 5 end to end: each md_sim fault flag must stop the run with the right
// reason, and a window roll must NOT. Nothing is ever resubscribed.
#include "cpm/md/feed_monitor.hpp"
#include "cpm/md/market_state.hpp"
#include "cpm/md/md_client.hpp"
#include "cpm/md/sim/simulator.hpp"

#include "test_util.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace {

using namespace std::chrono_literals;
using cpm::md::Channel;
using cpm::md::FeedHalt;
using cpm::md::FeedMonitor;
using cpm::md::LifecycleStatus;
using cpm::md::test::eventually;
using cpm::md::test::free_port;
using cpm::md::test::SharedSink;
using Reason = cpm::md::FeedHalt::Reason;

const cpm::Credentials kCreds{"sim-test-key", "sim-pass", "c2ltLXRlc3Qtc2VjcmV0LW5vdC1yZWFs"};
const std::string kHom = "SIMGAME-26NOV01HOMAWY-HOM";

// Simulator, client, state and monitor, wired the way md_client wires them.
// The halt handler does what the owner of the feed is meant to do: invalidate
// every book, then stop the session so QuickFIX cannot log back on (Q28).
struct Rig {
    explicit Rig(cpm::md::sim::SimOptions sim_options = {},
                 FeedMonitor::Options monitor_options = {1s, 50ms})
        : port(free_port()), sim_logger(sim_log), client_logger(client_log) {
        sim_options.credentials = kCreds;
        sim_options.port = port;
        sim_options.dict_dir = CPM_DICT_DIR;
        if (sim_options.events_per_sec == 5.0) sim_options.events_per_sec = 50;
        sim = std::make_unique<cpm::md::sim::Simulator>(std::move(sim_options), sim_logger);
        sim->start();

        monitor = std::make_unique<FeedMonitor>(monitor_options, [this](const FeedHalt& halt) {
            {
                std::lock_guard lock(mutex);
                halts.push_back(halt);
            }
            state.invalidate_books();
            client->stop();
        });

        cpm::md::MdClientOptions o;
        o.config.credentials = kCreds;
        o.config.endpoint = {"127.0.0.1", port, false, ""};
        o.config.heartbeat_secs = 30;
        o.dict_dir = CPM_DICT_DIR;
        o.reconnect_interval_secs = 1;
        client = std::make_unique<cpm::md::MdClient>(std::move(o), client_logger);
        client->on_session_event(monitor->session_callback());
        client->on_market_data(cpm::md::combine(state.callbacks(), monitor->callbacks()));
        monitor->start();
        client->start();
    }

    ~Rig() {
        monitor->stop();  // before the client, so our own logout is not a halt
        client->stop();
        sim->stop();
    }

    // Subscribes and registers it with the monitor, as an owner must.
    std::string subscribe(const cpm::md::ChannelRequest& request) {
        const auto id = client->subscribe(request);
        monitor->track(id, request);
        return id;
    }

    bool halted(std::chrono::milliseconds within = 10s) {
        return eventually([&] { return monitor->halted(); }, within);
    }
    std::optional<FeedHalt> halt() const {
        std::lock_guard lock(mutex);
        return halts.empty() ? std::nullopt : std::optional<FeedHalt>(halts.front());
    }
    std::size_t halt_count() const {
        std::lock_guard lock(mutex);
        return halts.size();
    }

    std::uint16_t port;
    SharedSink sim_log;
    SharedSink client_log;
    cpm::Logger sim_logger;
    cpm::Logger client_logger;
    cpm::md::MarketState state;
    mutable std::mutex mutex;
    std::vector<FeedHalt> halts;
    std::unique_ptr<FeedMonitor> monitor;
    std::unique_ptr<cpm::md::sim::Simulator> sim;
    std::unique_ptr<cpm::md::MdClient> client;
};

// --drop-seq: a MsgSeqNum is skipped, so market data was lost (Q24).
TEST(Halt, ASequenceGapStopsTheRun) {
    cpm::md::sim::SimOptions options;
    options.drop_seq = 6;
    Rig rig(options);
    ASSERT_TRUE(rig.client->wait_logged_on(5s));
    rig.subscribe({Channel::Book, {kHom}, 10});

    ASSERT_TRUE(rig.halted()) << rig.client_log.text();
    const auto halt = rig.halt();
    ASSERT_TRUE(halt.has_value());
    EXPECT_EQ(halt->reason, Reason::Gap);
    EXPECT_NE(halt->detail.find("sequence gap"), std::string::npos) << halt->detail;
    EXPECT_EQ(rig.halt_count(), 1u);

    // The owner invalidated the books, so nothing reads stale state after.
    const auto view = rig.state.instrument(kHom);
    if (view) {
        EXPECT_FALSE(view->book_valid);
    }
    // The handler stops the session; its logout takes a moment to complete.
    EXPECT_TRUE(eventually([&] { return !rig.client->logged_on(); }, 10s));
}

// --disconnect-after: the TCP connection drops with no Logout (Q28).
TEST(Halt, ADisconnectStopsTheRun) {
    cpm::md::sim::SimOptions options;
    options.disconnect_after = 4;
    Rig rig(options);
    ASSERT_TRUE(rig.client->wait_logged_on(5s));
    rig.subscribe({Channel::Book, {kHom}, 10});

    ASSERT_TRUE(rig.halted()) << rig.client_log.text();
    const auto halt = rig.halt();
    ASSERT_TRUE(halt.has_value());
    // A dropped connection can surface as the gap it leaves or as the logout
    // itself, depending on which arrives first; both end the run.
    EXPECT_TRUE(halt->reason == Reason::Disconnect || halt->reason == Reason::Gap)
        << to_string(halt->reason) << ": " << halt->detail;
    EXPECT_EQ(rig.halt_count(), 1u);
    EXPECT_TRUE(eventually([&] { return !rig.client->logged_on(); }, 10s));
}

// --stale-after: the subscription goes silent while the session stays healthy.
TEST(Halt, AStaleBookStopsTheRun) {
    cpm::md::sim::SimOptions options;
    options.stale_after_secs = 1;
    Rig rig(options);
    ASSERT_TRUE(rig.client->wait_logged_on(5s));
    const auto book = rig.subscribe({Channel::Book, {kHom}, 10});

    ASSERT_TRUE(rig.halted()) << rig.client_log.text();
    const auto halt = rig.halt();
    ASSERT_TRUE(halt.has_value());
    EXPECT_EQ(halt->reason, Reason::StaleFeed);
    EXPECT_EQ(halt->md_req_id, book);
    EXPECT_EQ(halt->symbol, kHom);
    EXPECT_NE(halt->detail.find("no book update for"), std::string::npos) << halt->detail;
}

// Q27 and Q29: any reject stops the run, including on the first subscribe.
TEST(Halt, AnUnknownSymbolStopsTheRun) {
    Rig rig;
    ASSERT_TRUE(rig.client->wait_logged_on(5s));
    rig.subscribe({Channel::Book, {"NOPE-0"}, 10});

    ASSERT_TRUE(rig.halted(5s)) << rig.client_log.text();
    const auto halt = rig.halt();
    ASSERT_TRUE(halt.has_value());
    EXPECT_EQ(halt->reason, Reason::Reject);
    EXPECT_NE(halt->detail.find("281=0: unknown symbol=NOPE-0"), std::string::npos) << halt->detail;
}

// The regression test that matters most (O10): an instrument that closes is
// silent by design, so a window roll must not stop the strategy.
TEST(Halt, AWindowRollDoesNotStopTheRun) {
    cpm::md::sim::SimOptions options;
    options.roll_secs = 3;
    options.settle_delay = 300ms;
    Rig rig(options);
    ASSERT_TRUE(rig.client->wait_logged_on(5s));
    rig.subscribe({Channel::Lifecycle, {}, 0});

    // Find the rolling instrument while it is still open, and follow its book.
    std::string rolling;
    ASSERT_TRUE(eventually(
        [&] {
            for (const auto& view : rig.state.instruments()) {
                if (view.symbol.rfind("SIMROLL-", 0) == 0 && view.status == LifecycleStatus::Open) {
                    rolling = view.symbol;
                    return true;
                }
            }
            return false;
        },
        10s))
        << rig.client_log.text();
    ASSERT_FALSE(rig.monitor->halted()) << to_string(rig.halt()->reason);
    rig.subscribe({Channel::Book, {rolling}, 10});

    // Wait for that instrument to finish trading.
    ASSERT_TRUE(eventually(
        [&] {
            const auto view = rig.state.instrument(rolling);
            return view && (view->status == LifecycleStatus::Settled || view->status == LifecycleStatus::Closed ||
                            view->status == LifecycleStatus::Determined);
        },
        10s))
        << rig.client_log.text();

    // Its book is now silent for far longer than the 1 s threshold, and that
    // is expected: no halt.
    EXPECT_FALSE(eventually([&] { return rig.monitor->halted(); }, 3s))
        << "halted on a closed instrument: " << (rig.halt() ? to_string(rig.halt()->reason) : "?") << " "
        << (rig.halt() ? rig.halt()->detail : "");
    EXPECT_EQ(rig.halt_count(), 0u);
    EXPECT_TRUE(rig.client->logged_on());
}

// A healthy feed on a live instrument keeps running.
TEST(Halt, AHealthyFeedKeepsRunning) {
    Rig rig({}, FeedMonitor::Options{2s, 50ms});
    ASSERT_TRUE(rig.client->wait_logged_on(5s));
    rig.subscribe({Channel::Book, {kHom}, 10});
    rig.subscribe({Channel::Trades, {}, 0});

    EXPECT_FALSE(eventually([&] { return rig.monitor->halted(); }, 5s))
        << (rig.halt() ? to_string(rig.halt()->reason) : "?") << " " << (rig.halt() ? rig.halt()->detail : "");
    const auto view = rig.state.instrument(kHom);
    ASSERT_TRUE(view.has_value());
    EXPECT_TRUE(view->book_valid);
    EXPECT_EQ(view->book_counters.anomalies, 0u);
}

}  // namespace
