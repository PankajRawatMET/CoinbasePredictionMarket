#include "cpm/binance/history.hpp"

#include <gtest/gtest.h>

namespace {
using namespace std::chrono_literals;
using cpm::binance::HistoryClock;
using cpm::binance::ReferenceHistory;

TEST(ReferenceHistory, KeepsOpeningCloseAndExtremaWithoutFillingGaps) {
    ReferenceHistory history;
    const auto start = HistoryClock::time_point{} + 2h;
    history.observe(100, start + 100ms);
    const auto snapshot = history.view();
    history.observe(150, start + 500ms);
    history.observe(90, start + 700ms);
    history.observe(110, start + 900ms);
    history.observe(120, start + 3s);
    ASSERT_EQ(history.view()->size(), 2u);
    const auto& s = history.view()->front();
    EXPECT_EQ(s.first, 100);
    EXPECT_EQ(s.close, 110);
    EXPECT_EQ(s.low, 90);
    EXPECT_EQ(s.high, 150);
    EXPECT_EQ(snapshot->front().close, 100);  // immutable snapshot
}

TEST(ReferenceHistory, BoundsMemoryAndIgnoresBackwardsTimeAndInvalidPrices) {
    ReferenceHistory history;
    const auto start = HistoryClock::time_point{} + 2h;
    for (int i = 0; i < 7200; ++i) history.observe(100 + i, start + std::chrono::seconds(i));
    EXPECT_LE(history.view()->size(), ReferenceHistory::kMaxSamples);
    const auto size = history.view()->size();
    history.observe(50, start);
    history.observe(0, start + 3h);
    EXPECT_EQ(history.view()->size(), size);
    EXPECT_EQ(history.view()->back().close, 7299);
}

TEST(ReferenceHistory, RestoresRecentOrderedSamplesAndRejectsCorruptOrFutureOnes) {
    ReferenceHistory source, restored;
    const auto at = HistoryClock::time_point{} + 2h;
    source.observe(100, at);
    const auto sample = source.view()->front();
    EXPECT_TRUE(restored.restore(sample, at + 1s));
    EXPECT_FALSE(restored.restore(sample, at + 1s));
    ReferenceHistory bad;
    EXPECT_FALSE(bad.restore(sample, at - 1s));
    EXPECT_FALSE(bad.restore(sample, at + 2h));
    auto corrupt = sample;
    corrupt.low = 101;
    EXPECT_FALSE(bad.restore(corrupt, at + 1s));
}
}  // namespace
