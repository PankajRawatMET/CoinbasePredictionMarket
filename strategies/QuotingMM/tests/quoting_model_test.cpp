#include "cpm/strategy/quoting_model.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

namespace {
using namespace std::chrono_literals;
using namespace cpm::strategy;
using cpm::binance::HistoryClock;

TEST(QuotingModel, StudentT4MatchesKnownValuesAndSymmetry) {
    EXPECT_DOUBLE_EQ(student_t4_cdf(0), 0.5);
    // z=2 -> u=1/sqrt(2), CDF=1/2 + 5/(8*sqrt(2)).
    EXPECT_NEAR(student_t4_cdf(2), 0.9419417382415922, 1e-14);
    for (double z : {0.1, 1.0, 2.0, 10.0, 100.0}) {
        EXPECT_NEAR(student_t4_cdf(z) + student_t4_cdf(-z), 1, 1e-14);
        EXPECT_GT(student_t4_cdf(z), 0.5);
        EXPECT_LT(student_t4_cdf(-z), 0.5);
    }
    EXPECT_DOUBLE_EQ(student_t4_cdf(std::numeric_limits<double>::infinity()), 1);
    EXPECT_DOUBLE_EQ(student_t4_cdf(-std::numeric_limits<double>::infinity()), 0);
}

TEST(QuotingModel, AtTheOpeningPriceFairValueIsHalf) {
    ASSERT_TRUE(quoting_probability(100000, 100000, 0.001, 900, 1));
    EXPECT_DOUBLE_EQ(*quoting_probability(100000, 100000, 0.001, 900, 1), 0.5);
}

TEST(QuotingModel, UsesLogMoneynessTimeAndCorrectionInCompatibleUnits) {
    const auto p = quoting_probability(110000, 100000, 0.01, 100, 2);
    ASSERT_TRUE(p);
    EXPECT_NEAR(*p, student_t4_cdf(std::log(1.1) / 0.2), 1e-14);
    EXPECT_GT(*quoting_probability(110000, 100000, 0.01, 25, 2), *p);
    EXPECT_LT(*quoting_probability(110000, 100000, 0.01, 100, 4), *p);
    EXPECT_GT(*quoting_probability(110000, 100000, 0.01, 100, 2, TScale::UnitVariance), *p);
}

TEST(QuotingModel, RejectsUnusableInputsInsteadOfInventingFairValue) {
    EXPECT_FALSE(quoting_probability(0, 100, 0.01, 900, 1));
    EXPECT_FALSE(quoting_probability(100, 0, 0.01, 900, 1));
    EXPECT_FALSE(quoting_probability(100, 100, 0, 900, 1));
    EXPECT_FALSE(quoting_probability(100, 100, 0.01, 0, 1));
    EXPECT_FALSE(quoting_probability(100, 100, 0.01, 900, -1));
    EXPECT_FALSE(quoting_probability(100, 100, std::numeric_limits<double>::quiet_NaN(), 900, 1));
}

TEST(QuotingModel, VolatilityUsesCompletedOneSecondSampleReturns) {
    cpm::binance::ReferenceHistory history;
    const auto start = HistoryClock::time_point{} + 2h;
    history.observe(100000, start);
    history.observe(110000, start + 1s);
    history.observe(100000, start + 2s);
    history.observe(999999, start + 3s);  // current partial second must be ignored
    const auto sigma = reference_volatility(*history.view(), 2, start + 3500ms);
    ASSERT_TRUE(sigma);
    EXPECT_NEAR(*sigma, std::sqrt(2.0) * std::log(1.1), 1e-14);
}

TEST(QuotingModel, GapsShortHistoryAndZeroVarianceAreNotReady) {
    cpm::binance::ReferenceHistory history;
    const auto start = HistoryClock::time_point{} + 2h;
    history.observe(100000, start);
    history.observe(110000, start + 2s);
    history.observe(100000, start + 3s);
    EXPECT_FALSE(reference_volatility(*history.view(), 3, start + 4s));
    EXPECT_FALSE(reference_volatility(*history.view(), 1200, start + 4s));
    cpm::binance::ReferenceHistory flat;
    for (int i = 0; i < 4; ++i) flat.observe(100000, start + std::chrono::seconds(i));
    EXPECT_FALSE(reference_volatility(*flat.view(), 2, start + 4s));
}

TEST(QuotingModel, OpeningRequiresAnObservationInTheActualOpeningSecond) {
    cpm::binance::ReferenceHistory history;
    const auto start = HistoryClock::time_point{} + 2h;
    history.observe(100000, start + 100ms);
    history.observe(110000, start + 900ms);
    history.observe(120000, start + 1s);
    ASSERT_TRUE(reference_opening(*history.view(), start));
    EXPECT_EQ(*reference_opening(*history.view(), start), 100000);
    EXPECT_FALSE(reference_opening(*history.view(), start - 1s));
}

TEST(QuotingModel, MaximumLookbackStillHasItsCompletedBoundaryPrice) {
    cpm::binance::ReferenceHistory history;
    const auto start = HistoryClock::time_point{} + 2h;
    for (int i = 0; i <= 7200; ++i) {
        history.observe(i % 2 ? 100000 : 110000, start + std::chrono::seconds(i));
    }
    EXPECT_TRUE(reference_volatility(*history.view(), 3600, start + 7200s));
}

TEST(QuotingModel, WarmupProgressCountsReturnsRatherThanPricesAndExcludesThePartialSecond) {
    cpm::binance::ReferenceHistory history;
    const auto start = HistoryClock::time_point{} + 2h;
    EXPECT_EQ(contiguous_reference_returns(*history.view(), 1200, start), 0);
    for (int i = 0; i <= 61; ++i) {
        history.observe(i % 2 ? 100000 : 110000, start + std::chrono::seconds(i));
    }
    EXPECT_EQ(contiguous_reference_returns(*history.view(), 1200, start + 61s), 60);
    EXPECT_EQ(contiguous_reference_returns(*history.view(), 10, start + 61s), 10);
    EXPECT_TRUE(reference_volatility(*history.view(), 60, start + 61s));
    EXPECT_FALSE(reference_volatility(*history.view(), 1200, start + 61s));
}

TEST(QuotingModel, WarmupProgressRestartsAtAGapAndRequiresTheLatestCompletedSecond) {
    cpm::binance::ReferenceHistory history;
    const auto start = HistoryClock::time_point{} + 2h;
    for (int i : {0, 1, 4, 5, 6, 7}) history.observe(100000 + i, start + std::chrono::seconds(i));
    EXPECT_EQ(contiguous_reference_returns(*history.view(), 1200, start + 7s), 2);
    EXPECT_EQ(contiguous_reference_returns(*history.view(), 1200, start + 8s), 3);
    EXPECT_EQ(contiguous_reference_returns(*history.view(), 1200, start + 9s), 0);
}
}  // namespace
