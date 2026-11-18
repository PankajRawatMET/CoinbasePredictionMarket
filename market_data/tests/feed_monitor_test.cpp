// The fail-stop detector of Stage 5 (project instructions amendment 4):
// a gap, a stale book, any reject or a disconnect ends the run, and nothing is
// ever resubscribed. Driven by hand on a fake clock, so nothing sleeps.
#include "cpm/md/feed_monitor.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using cpm::md::Channel;
using cpm::md::ChannelRequest;
using cpm::md::FeedHalt;
using cpm::md::FeedMonitor;
using cpm::md::LifecycleStatus;
using cpm::md::SessionEvent;
using Reason = cpm::md::FeedHalt::Reason;

const std::string kSym = "BTCD15M-BTC26OCT260615-0";

// A monitor on a clock the test moves by hand.
struct Rig {
    explicit Rig(bool halt_on_stale = true) {
        monitor = std::make_unique<FeedMonitor>(
            FeedMonitor::Options{30s, 250ms, halt_on_stale},
            [this](const FeedHalt& h) { halts.push_back(h); },
            [this] { return now; });
        session = monitor->session_callback();
        feed = monitor->callbacks();
    }

    // Logged on, with a book subscription and an all-instrument trade one.
    void subscribe_book() {
        monitor->track("book-1", ChannelRequest{Channel::Book, {kSym}, 10});
        monitor->track("trades-2", ChannelRequest{Channel::Trades, {}, 0});
        session({SessionEvent::Kind::LoggedOn, {}});
    }
    void advance(std::chrono::seconds by) { now += by; }
    void book_update(const std::string& req_id = "book-1") {
        cpm::md::BookUpdate update;
        update.md_req_id = req_id;
        update.symbol = kSym;
        feed.book_update(update);
    }
    void status(LifecycleStatus state, const std::string& symbol = kSym) {
        cpm::md::InstrumentLifecycle event;
        event.symbol = symbol;
        event.status = state;
        feed.lifecycle(event);
    }

    cpm::md::Clock::time_point now = cpm::md::Clock::time_point{} + 1h;
    std::vector<FeedHalt> halts;
    std::unique_ptr<FeedMonitor> monitor;
    cpm::md::MdClient::SessionCallback session;
    cpm::md::MdCallbacks feed;
};

TEST(FeedMonitorTest, AQuietBookHaltsOnceTheThresholdPasses) {
    Rig rig;
    rig.subscribe_book();
    rig.advance(29s);
    rig.monitor->tick(rig.now);
    EXPECT_FALSE(rig.monitor->halted());

    rig.advance(2s);
    rig.monitor->tick(rig.now);
    ASSERT_TRUE(rig.monitor->halted());
    ASSERT_EQ(rig.halts.size(), 1u);
    EXPECT_EQ(rig.halts.front().reason, Reason::StaleFeed);
    EXPECT_EQ(rig.halts.front().md_req_id, "book-1");
    EXPECT_EQ(rig.halts.front().symbol, kSym);
    EXPECT_NE(rig.halts.front().detail.find("no book update for 31s"), std::string::npos)
        << rig.halts.front().detail;
}

TEST(FeedMonitorTest, EachUpdateResetsTheClock) {
    Rig rig;
    rig.subscribe_book();
    for (int i = 0; i < 5; ++i) {
        rig.advance(20s);
        rig.book_update();
        rig.monitor->tick(rig.now);
    }
    EXPECT_FALSE(rig.monitor->halted());
    rig.advance(31s);
    rig.monitor->tick(rig.now);
    EXPECT_TRUE(rig.monitor->halted());
}

TEST(FeedMonitorTest, BookStalenessCanRecoverWithoutMaskingSessionFailure) {
    Rig rig(false);
    rig.subscribe_book();
    rig.advance(30s);
    rig.monitor->tick(rig.now);
    EXPECT_EQ(rig.monitor->stale_book(kSym, rig.now), 30s);
    EXPECT_FALSE(rig.monitor->stale_book("another-symbol", rig.now));
    EXPECT_FALSE(rig.monitor->halted());

    rig.book_update("unrelated-book");
    EXPECT_EQ(rig.monitor->stale_book(kSym, rig.now), 30s);
    rig.book_update();
    EXPECT_FALSE(rig.monitor->stale_book(kSym, rig.now));

    rig.advance(31s);
    EXPECT_EQ(rig.monitor->stale_book(kSym, rig.now), 31s);
    rig.session({SessionEvent::Kind::LoggedOut, "HEARTBEAT_TIMEOUT"});
    ASSERT_EQ(rig.halts.size(), 1u);
    EXPECT_EQ(rig.halts.front().reason, Reason::Disconnect);
}

TEST(FeedMonitorTest, ModelMakerCanUseAQuietBookButDisconnectStillHalts) {
    Rig rig;
    rig.subscribe_book();
    rig.monitor->allow_quiet_books({kSym});
    rig.advance(300s);
    rig.monitor->tick(rig.now);
    EXPECT_FALSE(rig.monitor->halted());
    rig.session({SessionEvent::Kind::LoggedOut, "connection lost"});
    ASSERT_TRUE(rig.monitor->halted());
    EXPECT_EQ(rig.halts.front().reason, Reason::Disconnect);
}

TEST(FeedMonitorTest, QuietBookExemptionDoesNotCoverOtherSymbolsOrSurviveRemoval) {
    Rig other;
    other.subscribe_book();
    other.monitor->allow_quiet_books({"another-symbol"});
    other.advance(31s);
    other.monitor->tick(other.now);
    EXPECT_TRUE(other.monitor->halted());

    Rig removed;
    removed.subscribe_book();
    removed.monitor->allow_quiet_books({kSym});
    removed.advance(31s);
    removed.monitor->tick(removed.now);
    EXPECT_FALSE(removed.monitor->halted());
    removed.monitor->allow_quiet_books({});
    removed.monitor->tick(removed.now);
    EXPECT_TRUE(removed.monitor->halted());
}

// O10: an instrument that has closed is silent by design. Without this the
// strategy would halt at every 15-minute window roll.
TEST(FeedMonitorTest, AFinishedInstrumentIsNotAStaleFeed) {
    Rig rig;
    rig.subscribe_book();
    for (const auto state : {LifecycleStatus::Closed, LifecycleStatus::Determined, LifecycleStatus::Settled}) {
        rig.status(state);
        rig.advance(60s);
        rig.monitor->tick(rig.now);
        EXPECT_FALSE(rig.monitor->halted()) << "halted after " << to_string(state);
    }
}

TEST(FeedMonitorTest, AStillOpenInstrumentIsStaleAsUsual) {
    Rig rig;
    rig.subscribe_book();
    rig.status(LifecycleStatus::Open);
    rig.advance(31s);
    rig.monitor->tick(rig.now);
    ASSERT_TRUE(rig.monitor->halted());
    EXPECT_EQ(rig.halts.front().reason, Reason::StaleFeed);
}

// A book covering several instruments is only exempt once they have all
// finished; one live instrument still has to keep speaking.
TEST(FeedMonitorTest, AMultiInstrumentBookNeedsEveryInstrumentFinished) {
    Rig rig;
    rig.monitor->track("book-1", ChannelRequest{Channel::Book, {kSym, "ETHD15M-ETH26OCT260615-0"}, 10});
    rig.session({SessionEvent::Kind::LoggedOn, {}});
    rig.status(LifecycleStatus::Settled);
    rig.advance(31s);
    rig.monitor->tick(rig.now);
    EXPECT_TRUE(rig.monitor->halted()) << "the second instrument is still live";
}

// Trades can be silent for minutes on a thin market (O3), and the lifecycle
// channel only speaks at window boundaries (O6).
TEST(FeedMonitorTest, OnlyBookSubscriptionsAreCheckedForSilence) {
    Rig rig;
    rig.monitor->track("trades-1", ChannelRequest{Channel::Trades, {kSym}, 0});
    rig.monitor->track("lifecycle-2", ChannelRequest{Channel::Lifecycle, {}, 0});
    rig.monitor->track("volume-oi-3", ChannelRequest{Channel::VolumeOi, {}, 0});
    rig.session({SessionEvent::Kind::LoggedOn, {}});
    rig.advance(10min);
    rig.monitor->tick(rig.now);
    EXPECT_FALSE(rig.monitor->halted());
}

TEST(FeedMonitorTest, SilenceBeforeLogonDoesNotCount) {
    Rig rig;
    rig.monitor->track("book-1", ChannelRequest{Channel::Book, {kSym}, 10});
    rig.advance(10min);
    rig.monitor->tick(rig.now);
    EXPECT_FALSE(rig.monitor->halted()) << "a disconnect is its own trigger";
}

TEST(FeedMonitorTest, AnUnsubscribedBookIsNoLongerWatched) {
    Rig rig;
    rig.subscribe_book();
    rig.monitor->forget("book-1");
    rig.advance(10min);
    rig.monitor->tick(rig.now);
    EXPECT_FALSE(rig.monitor->halted());
}

// Q24: a gap is lost data; nothing is resent and the strategy stops.
TEST(FeedMonitorTest, ASequenceGapHalts) {
    Rig rig;
    rig.subscribe_book();
    rig.session({SessionEvent::Kind::DataLoss, "sequence gap: expected MsgSeqNum 42, received 44"});
    ASSERT_EQ(rig.halts.size(), 1u);
    EXPECT_EQ(rig.halts.front().reason, Reason::Gap);
    EXPECT_NE(rig.halts.front().detail.find("received 44"), std::string::npos);
}

// Q28: QuickFIX would log back on with no subscriptions, so this must halt.
TEST(FeedMonitorTest, ADisconnectHalts) {
    Rig rig;
    rig.subscribe_book();
    rig.session({SessionEvent::Kind::LoggedOut, "session disconnected"});
    ASSERT_EQ(rig.halts.size(), 1u);
    EXPECT_EQ(rig.halts.front().reason, Reason::Disconnect);
    EXPECT_EQ(rig.halts.front().detail, "session disconnected");
}

// Q27: any reject, whatever the code, and Q29: including the first subscribe.
TEST(FeedMonitorTest, AnyRejectHalts) {
    Rig rig;
    rig.subscribe_book();
    cpm::md::MdReject reject;
    reject.kind = cpm::md::MdReject::Kind::MarketDataRequest;
    reject.md_req_id = "book-1";
    reject.reason_code = "0";
    reject.text = "unknown symbol=" + kSym;
    rig.feed.reject(reject);

    ASSERT_EQ(rig.halts.size(), 1u);
    EXPECT_EQ(rig.halts.front().reason, Reason::Reject);
    EXPECT_EQ(rig.halts.front().md_req_id, "book-1");
    EXPECT_NE(rig.halts.front().detail.find("281=0: unknown symbol"), std::string::npos)
        << rig.halts.front().detail;
}

TEST(FeedMonitorTest, ABusinessRejectHalts) {
    Rig rig;
    rig.subscribe_book();
    cpm::md::MdReject reject;
    reject.kind = cpm::md::MdReject::Kind::BusinessMessage;
    reject.reason_code = "3";
    reject.text = "unsupported message type";
    rig.feed.reject(reject);
    ASSERT_EQ(rig.halts.size(), 1u);
    EXPECT_NE(rig.halts.front().detail.find("BusinessMessageReject 380=3"), std::string::npos);
}

TEST(FeedMonitorTest, OnlyTheFirstConditionIsReported) {
    Rig rig;
    rig.subscribe_book();
    rig.session({SessionEvent::Kind::DataLoss, "sequence gap: expected 10, received 12"});
    rig.session({SessionEvent::Kind::LoggedOut, "session disconnected"});
    rig.advance(10min);
    rig.monitor->tick(rig.now);

    ASSERT_EQ(rig.halts.size(), 1u) << "the handler runs once";
    EXPECT_EQ(rig.halts.front().reason, Reason::Gap);
    EXPECT_EQ(rig.monitor->halt()->reason, Reason::Gap);
}

TEST(FeedMonitorTest, LoggingOnAgainRestartsEverySubscriptionsClock) {
    Rig rig;
    rig.subscribe_book();
    rig.advance(25s);
    // A fresh logon means nothing has been missed yet on any subscription.
    rig.session({SessionEvent::Kind::LoggedOn, {}});
    rig.advance(20s);
    rig.monitor->tick(rig.now);
    EXPECT_FALSE(rig.monitor->halted());
}

}  // namespace
