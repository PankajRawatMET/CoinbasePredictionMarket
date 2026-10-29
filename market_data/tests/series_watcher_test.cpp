// Which window of a series is trading now, from the lifecycle feed alone.
//
// The sequence driven here is the one in
// docs/spec/capture-sandbox-20261026T0607-roll.log, with its real tickers and
// its real order: Closed, Determined, then the next window Listed and Open,
// and Settled about a minute later.
#include "cpm/md/series_watcher.hpp"

#include <gtest/gtest.h>

namespace {

using cpm::md::Clock;
using cpm::md::InstrumentLifecycle;
using cpm::md::LifecycleStatus;
using cpm::md::parse_ticker;
using cpm::md::SeriesWatcher;

InstrumentLifecycle event(const std::string& symbol, LifecycleStatus status,
                          Clock::time_point when = Clock::now()) {
    InstrumentLifecycle out;
    out.symbol = symbol;
    out.security_id = "id-" + symbol;
    out.status = status;
    out.transact_time = when;
    return out;
}

Clock::time_point ends(const std::string& symbol) {
    return parse_ticker(symbol)->window_end;
}

const std::string kBtc0615 = "BTCD15M-BTC26OCT260615-0";
const std::string kBtc0630 = "BTCD15M-BTC26OCT260630-0";
const std::string kEth0615 = "ETHD15M-ETH26OCT260615-0";
const std::string kEth0630 = "ETHD15M-ETH26OCT260630-0";

TEST(SeriesWatcher, KnowsNothingUntilTheFeedSaysSomething) {
    SeriesWatcher watcher;
    EXPECT_FALSE(watcher.current("BTCD15M", Clock::now()).has_value());
    EXPECT_TRUE(watcher.series().empty());
}

TEST(SeriesWatcher, AnOpenWindowIsTheCurrentOneAndListedIsNot) {
    SeriesWatcher watcher;
    const auto inside = ends(kBtc0615) - std::chrono::minutes(7);

    // Listed is "publicly visible but not tradeable" - it is not yet an
    // answer to "what do I quote".
    watcher.apply(event(kBtc0615, LifecycleStatus::Listed, inside));
    EXPECT_FALSE(watcher.current("BTCD15M", inside).has_value());
    EXPECT_EQ(watcher.series(), std::vector<std::string>{"BTCD15M"});

    watcher.apply(event(kBtc0615, LifecycleStatus::Open, inside));
    const auto now = watcher.current("BTCD15M", inside);
    ASSERT_TRUE(now.has_value());
    EXPECT_EQ(now->symbol, kBtc0615);
    EXPECT_EQ(now->security_id, "id-" + kBtc0615);
    EXPECT_EQ(now->window_end, ends(kBtc0615));
    EXPECT_TRUE(now->tradable());
}

TEST(SeriesWatcher, FollowsTheRollExactlyAsTheCaptureHasIt) {
    SeriesWatcher watcher;
    const auto boundary = ends(kBtc0615);  // 06:15:00Z
    const auto before = boundary - std::chrono::minutes(5);

    watcher.apply(event(kBtc0615, LifecycleStatus::Open, before));
    watcher.apply(event(kEth0615, LifecycleStatus::Open, before));
    ASSERT_EQ(watcher.current("BTCD15M", before)->symbol, kBtc0615);
    ASSERT_EQ(watcher.current("ETHD15M", before)->symbol, kEth0615);

    // 06:15:00.406 Closed, .590 Determined, .757 Listed, .997 Open - and the
    // two series 50 ms apart, which is why they are tracked independently.
    watcher.apply(event(kBtc0615, LifecycleStatus::Closed, boundary + std::chrono::milliseconds(406)));
    watcher.apply(event(kEth0615, LifecycleStatus::Closed, boundary + std::chrono::milliseconds(455)));
    watcher.apply(event(kBtc0615, LifecycleStatus::Determined, boundary + std::chrono::milliseconds(590)));
    watcher.apply(event(kEth0615, LifecycleStatus::Determined, boundary + std::chrono::milliseconds(674)));

    // Between the close and the next open there is nothing to quote, and
    // saying so is the right answer rather than holding on to the old one.
    const auto between = boundary + std::chrono::milliseconds(700);
    EXPECT_FALSE(watcher.current("BTCD15M", between).has_value());
    EXPECT_FALSE(watcher.current("ETHD15M", between).has_value());

    watcher.apply(event(kBtc0630, LifecycleStatus::Listed, boundary + std::chrono::milliseconds(757)));
    watcher.apply(event(kEth0630, LifecycleStatus::Listed, boundary + std::chrono::milliseconds(773)));
    EXPECT_FALSE(watcher.current("BTCD15M", boundary + std::chrono::milliseconds(800)).has_value());

    watcher.apply(event(kBtc0630, LifecycleStatus::Open, boundary + std::chrono::milliseconds(997)));
    watcher.apply(event(kEth0630, LifecycleStatus::Open, boundary + std::chrono::milliseconds(998)));

    const auto after = boundary + std::chrono::seconds(1);
    ASSERT_TRUE(watcher.current("BTCD15M", after).has_value());
    EXPECT_EQ(watcher.current("BTCD15M", after)->symbol, kBtc0630);
    EXPECT_EQ(watcher.current("ETHD15M", after)->symbol, kEth0630);
    EXPECT_EQ(watcher.current("BTCD15M", after)->window_end, boundary + std::chrono::minutes(15));

    // What the roll left behind is still there to be asked about.
    const auto gone = watcher.previous("BTCD15M");
    ASSERT_TRUE(gone.has_value());
    EXPECT_EQ(gone->symbol, kBtc0615);
    EXPECT_EQ(gone->status, LifecycleStatus::Determined);

    // Settled, a minute later, does not change which window is current.
    watcher.apply(event(kBtc0615, LifecycleStatus::Settled, boundary + std::chrono::seconds(60)));
    EXPECT_EQ(watcher.current("BTCD15M", boundary + std::chrono::seconds(61))->symbol, kBtc0630);
    EXPECT_EQ(watcher.previous("BTCD15M")->status, LifecycleStatus::Settled);
}

TEST(SeriesWatcher, AWindowWhoseEndHasPassedIsNotCurrentEvenIfNothingSaidSo) {
    // The feed is changes-only, so a Closed that never arrived - a dropped
    // message, a reconnect across the boundary - would otherwise leave a dead
    // window looking live. Quoting one is a rejected order, which halts.
    SeriesWatcher watcher;
    watcher.apply(event(kBtc0615, LifecycleStatus::Open, ends(kBtc0615) - std::chrono::minutes(5)));

    EXPECT_TRUE(watcher.current("BTCD15M", ends(kBtc0615) - std::chrono::seconds(1)).has_value());
    // Still inside the grace, because a clock is not a promise (R-N3).
    EXPECT_TRUE(watcher.current("BTCD15M", ends(kBtc0615) + std::chrono::seconds(5)).has_value());
    EXPECT_FALSE(watcher.current("BTCD15M", ends(kBtc0615) + std::chrono::minutes(1)).has_value());
}

TEST(SeriesWatcher, TwoWindowsOpenAtOnceResolvesToTheLaterOne) {
    // It should not happen - the capture shows no overlap - but if it does,
    // the window that ends later is the one with trading left in it, and the
    // one that ends sooner may be a Closed that was missed.
    SeriesWatcher watcher;
    const auto when = ends(kBtc0615) - std::chrono::minutes(1);
    watcher.apply(event(kBtc0615, LifecycleStatus::Open, when));
    watcher.apply(event(kBtc0630, LifecycleStatus::Open, when));
    EXPECT_EQ(watcher.current("BTCD15M", when)->symbol, kBtc0630);
}

TEST(SeriesWatcher, IgnoresNamesThatAreNotDatedWindows) {
    // A sports market is a real instrument that never rolls, and a simulator
    // symbol is not a series at all.
    SeriesWatcher watcher;
    watcher.apply(event("NFLGAME-26OCT25LVLAR-LV", LifecycleStatus::Open));
    watcher.apply(event("SIMGAME-26NOV01HOMAWY-HOM", LifecycleStatus::Open));
    EXPECT_TRUE(watcher.series().empty());
    EXPECT_FALSE(watcher.current("NFLGAME", Clock::now()).has_value());
}

TEST(SeriesWatcher, SeriesDoNotInterfere) {
    SeriesWatcher watcher;
    const auto when = ends(kBtc0615) - std::chrono::minutes(5);
    watcher.apply(event(kBtc0615, LifecycleStatus::Open, when));
    watcher.apply(event(kEth0615, LifecycleStatus::Open, when));
    watcher.apply(event(kBtc0615, LifecycleStatus::Closed, when + std::chrono::minutes(1)));

    EXPECT_FALSE(watcher.current("BTCD15M", when + std::chrono::minutes(1)).has_value());
    EXPECT_EQ(watcher.current("ETHD15M", when + std::chrono::minutes(1))->symbol, kEth0615);
    EXPECT_EQ(watcher.series(), (std::vector<std::string>{"BTCD15M", "ETHD15M"}));
}

TEST(SeriesWatcher, ADuplicateEventChangesNothing) {
    // Overlapping subscriptions deliver the same f twice ([MD] O7), and the
    // roll capture shows exactly that.
    SeriesWatcher watcher;
    const auto when = ends(kBtc0615) - std::chrono::minutes(5);
    watcher.apply(event(kBtc0615, LifecycleStatus::Open, when));
    watcher.apply(event(kBtc0615, LifecycleStatus::Open, when));
    EXPECT_EQ(watcher.windows("BTCD15M").size(), 1u);
    EXPECT_EQ(watcher.current("BTCD15M", when)->symbol, kBtc0615);
}

TEST(SeriesWatcher, AnAmendedDoesNotChangeTheState) {
    // Amended(26) is an event, not a state ([MD] Q14): something about the
    // instrument changed, not that it stopped trading.
    SeriesWatcher watcher;
    const auto when = ends(kBtc0615) - std::chrono::minutes(5);
    watcher.apply(event(kBtc0615, LifecycleStatus::Open, when));
    watcher.apply(event(kBtc0615, LifecycleStatus::Amended, when + std::chrono::seconds(1)));
    ASSERT_TRUE(watcher.current("BTCD15M", when + std::chrono::seconds(1)).has_value());
    EXPECT_EQ(watcher.current("BTCD15M", when + std::chrono::seconds(1))->status,
              LifecycleStatus::Open);
}

TEST(SeriesWatcher, ForgetsWindowsItNoLongerNeeds) {
    // 96 windows a day per D15M series ([MD] O2/O6) is why this exists at
    // all - the same reason MarketState::evict_finished does.
    SeriesWatcher::Options options;
    options.remember_finished = std::chrono::minutes(5);
    SeriesWatcher watcher(options);

    const auto boundary = ends(kBtc0615);
    watcher.apply(event(kBtc0615, LifecycleStatus::Settled, boundary + std::chrono::seconds(60)));
    watcher.apply(event(kBtc0630, LifecycleStatus::Open, boundary + std::chrono::seconds(1)));

    EXPECT_EQ(watcher.evict(boundary + std::chrono::minutes(1)), 0u);
    EXPECT_EQ(watcher.windows("BTCD15M").size(), 2u);

    // The settled one goes; the open one stays, however old it is.
    EXPECT_EQ(watcher.evict(boundary + std::chrono::minutes(10)), 1u);
    EXPECT_EQ(watcher.windows("BTCD15M").size(), 1u);
    EXPECT_EQ(watcher.windows("BTCD15M").front().symbol, kBtc0630);
    EXPECT_FALSE(watcher.previous("BTCD15M").has_value());

    // And a series with nothing left in it stops being a series it knows.
    watcher.apply(event(kBtc0630, LifecycleStatus::Settled, boundary + std::chrono::minutes(16)));
    EXPECT_EQ(watcher.evict(boundary + std::chrono::hours(1)), 1u);
    EXPECT_TRUE(watcher.series().empty());
}

TEST(SeriesWatcher, ListsItsWindowsNewestFirst) {
    SeriesWatcher watcher;
    const auto when = ends(kBtc0615) - std::chrono::minutes(5);
    watcher.apply(event(kBtc0630, LifecycleStatus::Listed, when));
    watcher.apply(event(kBtc0615, LifecycleStatus::Open, when));

    const auto windows = watcher.windows("BTCD15M");
    ASSERT_EQ(windows.size(), 2u);
    EXPECT_EQ(windows[0].symbol, kBtc0630);
    EXPECT_EQ(windows[1].symbol, kBtc0615);
}

}  // namespace
