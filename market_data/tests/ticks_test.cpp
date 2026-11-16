#include "cpm/md/ticks.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <set>

namespace {
using namespace cpm::md;

// One grid, the venue's: TAPERED_DECI_CENT. 0.001 ticks below 0.100 and
// above 0.900, 0.01 between, with both boundaries valid (spec section 6.4,
// docs/open-questions.txt O5).

TEST(Ticks, TheTaperChangesAtATenthAndNineTenths) {
    EXPECT_EQ(tick_at(0), 1);
    EXPECT_EQ(tick_at(99), 1);
    // The boundaries themselves step on the coarse grid: one above 0.100 is
    // 0.110, not 0.101.
    EXPECT_EQ(tick_at(100), 10);
    EXPECT_EQ(tick_at(500), 10);
    EXPECT_EQ(tick_at(900), 10);
    EXPECT_EQ(tick_at(901), 1);
    EXPECT_EQ(tick_at(999), 1);
}

TEST(Ticks, TheGridRunsEndToEndWithoutDrifting) {
    const auto grid = price_grid();
    ASSERT_FALSE(grid.empty());
    EXPECT_EQ(grid.front(), 0);
    EXPECT_EQ(grid.back(), cpm::kPriceOne);
    EXPECT_TRUE(std::is_sorted(grid.begin(), grid.end()));
    EXPECT_EQ(std::set<cpm::Price>(grid.begin(), grid.end()).size(), grid.size())
        << "a price appears once, including the shared boundaries";

    // 101 rungs to 0.100, 80 more to 0.900, 100 more to 1.000.
    EXPECT_EQ(grid.size(), 281u);

    for (const auto price : grid) {
        EXPECT_EQ(snap_down(price), price);
        EXPECT_EQ(snap_up(price), price) << "a grid price snaps to itself";
    }
}

TEST(Ticks, ReadAsAPercentageItIsAPointAtATimeWithTenthsAtBothEnds) {
    const auto grid = price_grid();
    const auto has = [&grid](cpm::Price price) {
        return std::binary_search(grid.begin(), grid.end(), price);
    };
    // Tenths of a point out to 10%...
    EXPECT_TRUE(has(1));    // 0.1%
    EXPECT_TRUE(has(21));   // 2.1%
    EXPECT_TRUE(has(99));   // 9.9%
    // ... whole points between ...
    EXPECT_TRUE(has(110));  // 11%
    EXPECT_FALSE(has(111)) << "11.1% is not a price between the boundaries";
    EXPECT_FALSE(has(523));
    // ... and tenths again from 90%.
    EXPECT_TRUE(has(901));  // 90.1%
    EXPECT_TRUE(has(975));  // 97.5%, which the live book has used
    EXPECT_TRUE(has(989));  // 98.9%
}

TEST(Ticks, TheEndsAreOnTheGridButAreNotOrderPrices) {
    EXPECT_FALSE(is_quotable(0));
    EXPECT_FALSE(is_quotable(cpm::kPriceOne));
    EXPECT_TRUE(is_quotable(1));
    EXPECT_TRUE(is_quotable(cpm::kPriceOne - 1));
}

TEST(Ticks, SteppingNeverLandsOffTheGrid) {
    const auto grid = price_grid();
    const auto on_grid = [&grid](cpm::Price price) {
        return std::binary_search(grid.begin(), grid.end(), price);
    };
    // Including from prices that are not on it, and across both boundaries,
    // which is where `price - tick_at(price)` would go wrong.
    for (cpm::Price price = 0; price <= cpm::kPriceOne; ++price) {
        EXPECT_TRUE(on_grid(snap_down(price)));
        EXPECT_TRUE(on_grid(snap_up(price)));
        EXPECT_TRUE(on_grid(price_below(price)));
        EXPECT_TRUE(on_grid(price_above(price)));
        if (price > 0) EXPECT_LT(price_below(price), price);
        if (price < cpm::kPriceOne) EXPECT_GT(price_above(price), price);
    }
    // Across the boundaries, where `price - tick_at(price)` would be wrong.
    // Above 90% the step is a tenth of a point, so one below 99% is 98.9%.
    EXPECT_EQ(price_below(990), 989);
    EXPECT_EQ(price_above(990), 991);
    // 0.100 and 0.900 are on both sides of their boundary: stepping up from
    // 0.100 takes the coarse step, stepping up from 0.900 the fine one,
    // because that is where each region's next price actually is.
    EXPECT_EQ(price_above(100), 110);
    EXPECT_EQ(price_below(100), 99);
    EXPECT_EQ(price_above(900), 901);
    EXPECT_EQ(price_below(910), 909);
    EXPECT_EQ(price_below(1000), 999);
}

}  // namespace
