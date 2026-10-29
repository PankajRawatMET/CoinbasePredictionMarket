// The price grid an instrument trades on: which prices exist at all. It
// decides what an order may be priced at and what rungs a ladder has, so it
// is worth being exact about where the tapering happens (O5).
#include "cpm/md/ticks.hpp"

#include <gtest/gtest.h>

#include <algorithm>

namespace {

using cpm::md::price_grid;
using cpm::md::tick_at;
using cpm::md::TickStructure;

TEST(Ticks, TheTaperChangesAtATenthAndNineTenths) {
    const auto tapered = TickStructure::TaperedDeciCent;
    EXPECT_EQ(tick_at(0, tapered), 1);
    EXPECT_EQ(tick_at(99, tapered), 1);
    // 0.100 and 0.900 are themselves on the coarse grid.
    EXPECT_EQ(tick_at(100, tapered), 10);
    EXPECT_EQ(tick_at(500, tapered), 10);
    EXPECT_EQ(tick_at(900, tapered), 10);
    EXPECT_EQ(tick_at(901, tapered), 1);
    EXPECT_EQ(tick_at(1000, tapered), 1);
}

TEST(Ticks, TheGridRunsEndToEndWithoutDrifting) {
    for (const auto structure : {TickStructure::LinearCent, TickStructure::DeciCent,
                                 TickStructure::TaperedDeciCent,
                                 TickStructure::TaperedCent}) {
        const auto grid = price_grid(structure);
        ASSERT_FALSE(grid.empty());
        EXPECT_EQ(grid.front(), 0) << "the ladder must start at zero";
        EXPECT_EQ(grid.back(), cpm::kPriceOne) << "and end at one";
        // Strictly ascending, in steps of one of the two tick sizes. Not
        // necessarily tick_at(the previous rung): membership and stepping
        // differ at the boundaries, which is what 0.901 below is about.
        for (std::size_t i = 1; i < grid.size(); ++i) {
            const auto step = grid[i] - grid[i - 1];
            EXPECT_GT(step, 0);
            EXPECT_TRUE(step == 1 || step == 10) << "a step of " << step << " at " << grid[i];
        }
    }
}

TEST(Ticks, ThePercentageGridIsAPointAtATimeWithTenthsAtBothEnds) {
    // What the venue does, read as a percentage chance: a tenth of a point
    // inside the first and last point, one point between.
    //   0, 0.1, 0.2 ... 0.9, 1, 2, 3 ... 98, 99, 99.1 ... 99.9, 100
    const auto grid = price_grid(TickStructure::TaperedCent);
    ASSERT_EQ(grid.size(), 119u) << "11 tenths, 98 points, 10 more tenths";
    EXPECT_EQ(grid.front(), 0);           // 0%
    EXPECT_EQ(grid[1], 1);                // 0.1%
    EXPECT_EQ(grid[9], 9);                // 0.9%
    EXPECT_EQ(grid[10], 10);              // 1%
    EXPECT_EQ(grid[11], 20);              // 2%
    EXPECT_EQ(grid[108], 990);            // 99%
    EXPECT_EQ(grid[109], 991);            // 99.1%
    EXPECT_EQ(grid.back(), cpm::kPriceOne);  // 100%

    // Inside the first and last point the step is a tenth; 1% and 99%
    // themselves are on the coarse grid, as the sibling structure has it.
    EXPECT_EQ(tick_at(0, TickStructure::TaperedCent), 1);
    EXPECT_EQ(tick_at(9, TickStructure::TaperedCent), 1);
    EXPECT_EQ(tick_at(10, TickStructure::TaperedCent), 10);
    EXPECT_EQ(tick_at(500, TickStructure::TaperedCent), 10);
    EXPECT_EQ(tick_at(990, TickStructure::TaperedCent), 10);
    EXPECT_EQ(tick_at(991, TickStructure::TaperedCent), 1);

    // Stepping is grid-aware whatever tick_at says at a boundary.
    EXPECT_EQ(cpm::md::price_above(990, TickStructure::TaperedCent), 991);
    EXPECT_EQ(cpm::md::price_below(10, TickStructure::TaperedCent), 9);
    EXPECT_EQ(cpm::md::price_above(9, TickStructure::TaperedCent), 10);

    // And it reads as a percentage without any arithmetic: the same integer
    // with one implied decimal instead of three.
    EXPECT_EQ(cpm::format_percent(0), "0");
    EXPECT_EQ(cpm::format_percent(10), "1");
    EXPECT_EQ(cpm::format_percent(599), "59.9");
    EXPECT_EQ(cpm::format_percent(991), "99.1");
    EXPECT_EQ(cpm::format_percent(cpm::kPriceOne), "100");
}

TEST(Ticks, TheEndsAreOnTheGridButAreNotOrderPrices) {
    // "Order prices exclude 0 and 1; settlement values may equal either" (O5).
    EXPECT_FALSE(cpm::md::is_quotable(0));
    EXPECT_FALSE(cpm::md::is_quotable(cpm::kPriceOne));
    EXPECT_TRUE(cpm::md::is_quotable(1));
    EXPECT_TRUE(cpm::md::is_quotable(999));
    for (const auto structure : {TickStructure::LinearCent, TickStructure::DeciCent,
                                 TickStructure::TaperedDeciCent,
                                 TickStructure::TaperedCent}) {
        const auto grid = price_grid(structure);
        EXPECT_EQ(grid.front(), 0);
        EXPECT_EQ(grid.back(), cpm::kPriceOne);
    }
}

TEST(Ticks, EachStructureHasTheRungsItShould) {
    // 0.00 to 1.00 inclusive.
    EXPECT_EQ(price_grid(TickStructure::LinearCent).size(), 101u);
    // 0.000 to 1.000 inclusive.
    EXPECT_EQ(price_grid(TickStructure::DeciCent).size(), 1001u);
    // 100 fine rungs below 0.100, 80 coarse ones to 0.900, 100 fine above,
    // and 1.000 itself.
    EXPECT_EQ(price_grid(TickStructure::TaperedDeciCent).size(), 281u);

    const auto tapered = price_grid(TickStructure::TaperedDeciCent);
    const auto has = [&tapered](cpm::Price price) {
        return std::find(tapered.begin(), tapered.end(), price) != tapered.end();
    };
    EXPECT_TRUE(has(1));     // 0.001, in the fine region
    EXPECT_TRUE(has(100));   // 0.100, the boundary
    EXPECT_FALSE(has(105));  // not on the coarse grid between the boundaries
    EXPECT_TRUE(has(110));
    EXPECT_TRUE(has(900));   // 0.900, the other boundary
    // An order can rest at 0.901 even though a strategy stepping up from
    // 0.900 would land on 0.910, so the ladder has a rung for it.
    EXPECT_TRUE(has(901));
    EXPECT_TRUE(has(99));    // and the mirror of it below 0.100
    EXPECT_EQ(tick_at(900, TickStructure::TaperedDeciCent), 10);
}

TEST(Ticks, TheSpellingSurvivesTheRoundTrip) {
    for (const auto structure : {TickStructure::LinearCent, TickStructure::DeciCent,
                                 TickStructure::TaperedDeciCent,
                                 TickStructure::TaperedCent}) {
        const auto name = cpm::md::to_string(structure);
        const auto parsed = cpm::md::tick_structure_from(name);
        ASSERT_TRUE(parsed.has_value()) << name;
        EXPECT_EQ(*parsed, structure);
    }
    EXPECT_FALSE(cpm::md::tick_structure_from("penny").has_value());
}

}  // namespace
