// The P&L of a position (GUI-Q15): realized always, unrealized only against a
// mark, and the two adding up to the mark-to-market of the whole thing.
#include "cpm/oms/pnl.hpp"

#include <gtest/gtest.h>

namespace {

using cpm::Amount;
using cpm::Price;
using cpm::Qty;
using cpm::oms::Pnl;
using cpm::oms::Position;
using cpm::oms::pnl_of;

// One contract is 100, one thousandth of a dollar is 1, and an amount is at
// six places: a contract bought at 0.52 is 0.520000 of cash.
constexpr Amount kOne = 1000000;  // 1.000000, for reading the numbers below

Amount value(Qty quantity, Price price) { return quantity * price * 10; }

// A position built the way OrderState builds one, so the invariant the tests
// lean on - cash is the two values plus the fees - holds by construction
// rather than by hand.
Position traded(Qty bought, Price bought_at, Qty sold, Price sold_at, Amount fees = 0) {
    Position position;
    position.symbol = "BTCD15M-BTC26OCT300545-0";
    position.bought = bought;
    position.sold = sold;
    position.bought_value = value(bought, bought_at);
    position.sold_value = value(sold, sold_at);
    position.net = bought - sold;
    position.fees = fees;
    position.cash = position.bought_value - position.sold_value + fees;
    return position;
}

// What the whole position is worth at a mark, which realized and unrealized
// must always add up to.
Amount mark_to_market(const Position& position, Price mark) {
    return value(position.net, mark) - position.cash;
}

}  // namespace

TEST(PnlTest, AFlatPositionIsAllRealizedAndNeedsNoMark) {
    // Bought 2 at 0.52, sold 2 at 0.55, a cent of fees.
    const auto position = traded(200, 520, 200, 550, 10000);
    const auto pnl = pnl_of(position, std::nullopt);
    EXPECT_EQ(pnl.realized, 60000 - 10000) << "0.06 on the round turn, less 0.01 of fees";
    EXPECT_EQ(pnl.unrealized, 0) << "nothing is open, so zero is the truth, not null";
    EXPECT_EQ(pnl.total, 50000);
    EXPECT_EQ(pnl.realized, -position.cash);
}

TEST(PnlTest, SplitsALongPositionAtTheAverageCost) {
    // Bought 2 at 0.52, sold 1 at 0.55, a cent of fees: 1 contract left.
    const auto position = traded(200, 520, 100, 550, 10000);
    ASSERT_EQ(position.net, 100);

    const auto pnl = pnl_of(position, 540);
    EXPECT_EQ(pnl.realized, 30000 - 10000) << "0.03 on the one closed, less the fees";
    EXPECT_EQ(pnl.unrealized, 20000) << "the one held is 0.02 up at 0.540";
    EXPECT_EQ(pnl.total, 40000);
    EXPECT_EQ(pnl.total, mark_to_market(position, 540));
}

TEST(PnlTest, SplitsAShortPositionAtTheAverageSale) {
    // Sold 2 at 0.55, bought 1 back at 0.52: 1 contract short.
    const auto position = traded(100, 520, 200, 550, 10000);
    ASSERT_EQ(position.net, -100);

    const auto pnl = pnl_of(position, 540);
    EXPECT_EQ(pnl.realized, 30000 - 10000) << "0.03 on the one covered, less the fees";
    EXPECT_EQ(pnl.unrealized, 10000) << "short at 0.550 against a 0.540 mark is 0.01 up";
    EXPECT_EQ(pnl.total, 30000);
    EXPECT_EQ(pnl.total, mark_to_market(position, 540));
}

TEST(PnlTest, AnOpenPositionWithNoMarkHasNoUnrealized) {
    const auto position = traded(200, 520, 100, 550);
    const auto pnl = pnl_of(position, std::nullopt);
    EXPECT_EQ(pnl.realized, 30000) << "what closed is known either way";
    EXPECT_FALSE(pnl.unrealized.has_value()) << "no mid, no mark, and no guess at one";
    EXPECT_FALSE(pnl.total.has_value()) << "a total with a hole in it is not a total";
}

// Nothing has traded yet, which is every position's first moment.
TEST(PnlTest, AnUntouchedPositionIsZero) {
    const Position position;
    const auto pnl = pnl_of(position, 500);
    EXPECT_EQ(pnl.realized, 0);
    EXPECT_EQ(pnl.unrealized, 0);
    EXPECT_EQ(pnl.total, 0);
}

// A position that has only ever bought has nothing realized to show, and a
// mark is the whole of its P&L.
TEST(PnlTest, AOneSidedPositionIsAllUnrealized) {
    const auto position = traded(300, 520, 0, 0, 10000);
    const auto pnl = pnl_of(position, 530);
    EXPECT_EQ(pnl.realized, -10000) << "only the fees have been paid";
    EXPECT_EQ(pnl.unrealized, 30000) << "3 contracts a cent up";
    EXPECT_EQ(pnl.total, mark_to_market(position, 530));
}

// Stage 6: the payout closed the position, so all of it is realized whatever
// the book says, and a mark is not wanted.
TEST(PnlTest, ASettledPositionIsRealizedInFull) {
    auto position = traded(200, 520, 0, 0, 10000);
    // YES settled at 1.000, so two contracts pay out 2.000000.
    position.settlement_price = cpm::kPriceOne;
    position.settlement_payout = 2 * kOne;

    for (const auto mark : {std::optional<Price>{}, std::optional<Price>{1}}) {
        const auto pnl = pnl_of(position, mark);
        EXPECT_EQ(pnl.realized, 2 * kOne - position.cash) << "the payout against what it cost";
        EXPECT_EQ(pnl.realized, 950000) << "2.00 back for 1.04 and a cent of fees";
        EXPECT_EQ(pnl.unrealized, 0);
        EXPECT_EQ(pnl.total, pnl.realized);
    }
}

TEST(PnlTest, ASettledPositionCanHaveLostEverything) {
    auto position = traded(200, 520, 0, 0);
    position.settlement_price = 0;  // the YES side did not happen
    position.settlement_payout = 0;
    const auto pnl = pnl_of(position, std::nullopt);
    EXPECT_EQ(pnl.realized, -value(200, 520)) << "the whole of what it cost";
}

// The identity the split rests on, over a sweep: realized and unrealized add
// up to the mark-to-market of the position, exactly, with no rounding dust
// left between them. The averages do not divide evenly here, which is the
// case that would show dust if the arithmetic let any in.
TEST(PnlTest, RealizedAndUnrealizedAlwaysAddUpToTheMarkToMarket) {
    for (const Qty bought : {100, 300, 700, 1234}) {
        for (const Qty sold : {0, 100, 299, 700, 1234}) {
            for (const Price mark : {1, 170, 503, 999}) {
                const auto position = traded(bought, 517, sold, 683, 1234);
                const auto pnl = pnl_of(position, mark);
                ASSERT_TRUE(pnl.total.has_value());
                EXPECT_EQ(*pnl.total, mark_to_market(position, mark))
                    << "bought " << bought << " sold " << sold << " mark " << mark;
            }
        }
    }
}
