// The per-instrument book of design §8: a snapshot replaces it, deltas set
// absolute sizes at a price, and everything that contradicts the spec or the
// book's own state is reported rather than thrown.
#include "cpm/md/book.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using cpm::md::BookAction;
using cpm::md::BookAnomaly;
using cpm::md::BookLevel;
using cpm::md::BookLevelChange;
using cpm::md::BookSide;
using cpm::md::BookSnapshot;
using cpm::md::BookUpdate;
using cpm::md::OrderBook;
using Kind = cpm::md::BookAnomaly::Kind;

const std::string kSymbol = "BTCD15M-BTC26OCT260545-0";

BookSnapshot snapshot(std::vector<BookLevel> yes, std::vector<BookLevel> no) {
    BookSnapshot s;
    s.md_req_id = "book-1";
    s.symbol = kSymbol;
    s.security_id = "240346204372729859";
    s.yes = std::move(yes);
    s.no = std::move(no);
    s.transact_time = cpm::md::Clock::time_point{} + std::chrono::seconds(1);
    return s;
}

BookUpdate update(std::vector<BookLevelChange> changes) {
    BookUpdate u;
    u.md_req_id = "book-1";
    u.symbol = kSymbol;
    u.changes = std::move(changes);
    u.transact_time = cpm::md::Clock::time_point{} + std::chrono::seconds(2);
    return u;
}

// A set that claims to be a New (279=0), with the position it should land at.
BookLevelChange new_level(BookSide side, cpm::Price price, cpm::Qty qty, int level) {
    return {side, BookAction::Set, price, qty, level, true};
}
BookLevelChange change_level(BookSide side, cpm::Price price, cpm::Qty qty, int level) {
    return {side, BookAction::Set, price, qty, level, false};
}
BookLevelChange delete_level(BookSide side, cpm::Price price) {
    return {side, BookAction::Delete, price, 0, 0, false};
}

// A book with the O9 snapshot already applied, and every anomaly collected.
struct Rig {
    Rig() {
        book.on_anomaly([this](const BookAnomaly& a) { anomalies.push_back(a); });
    }
    void load() {
        book.apply(snapshot({{500, 500}, {490, 600}, {480, 200}}, {{480, 300}, {470, 200}, {460, 300}, {450, 100}}));
        anomalies.clear();
    }
    std::size_t count(Kind kind) const { return book.counters().count(kind); }

    OrderBook book;
    std::vector<BookAnomaly> anomalies;
};

TEST(Book, SnapshotReplacesBothSidesAndValidatesTheBook) {
    Rig rig;
    EXPECT_FALSE(rig.book.valid());
    rig.load();
    EXPECT_TRUE(rig.book.valid());
    EXPECT_EQ(rig.book.symbol(), kSymbol);
    EXPECT_EQ(rig.book.md_req_id(), "book-1");
    EXPECT_EQ(rig.book.levels(BookSide::Yes), (std::vector<BookLevel>{{500, 500}, {490, 600}, {480, 200}}));
    EXPECT_EQ(rig.book.depth(BookSide::No), 4u);
    EXPECT_EQ(rig.book.best(BookSide::Yes), (BookLevel{500, 500}));
    EXPECT_EQ(rig.book.counters().anomalies, 0u);

    // A second snapshot replaces what was there; it never merges.
    rig.book.apply(snapshot({{600, 100}}, {}));
    EXPECT_EQ(rig.book.levels(BookSide::Yes), (std::vector<BookLevel>{{600, 100}}));
    EXPECT_TRUE(rig.book.levels(BookSide::No).empty());
    EXPECT_FALSE(rig.book.best(BookSide::No).has_value());
}

TEST(Book, SnapshotIsKeptBestFirstEvenIfTheFeedIsNot) {
    Rig rig;
    rig.book.apply(snapshot({{480, 200}, {500, 500}, {490, 600}}, {}));
    EXPECT_EQ(rig.book.levels(BookSide::Yes), (std::vector<BookLevel>{{500, 500}, {490, 600}, {480, 200}}));
}

TEST(Book, DeltaSetsTheAbsoluteSizeAtAPrice) {
    Rig rig;
    rig.load();
    // O4: a Change carries the new size, not a difference.
    rig.book.apply(update({change_level(BookSide::Yes, 490, 250, 2)}));
    EXPECT_EQ(rig.book.levels(BookSide::Yes)[1], (BookLevel{490, 250}));
    // A New lands in price order, whatever position it is sent in.
    rig.book.apply(update({new_level(BookSide::Yes, 495, 100, 2)}));
    EXPECT_EQ(rig.book.levels(BookSide::Yes),
              (std::vector<BookLevel>{{500, 500}, {495, 100}, {490, 250}, {480, 200}}));
    EXPECT_EQ(rig.book.counters().anomalies, 0u) << rig.anomalies.size();
}

TEST(Book, DeleteAndZeroSizeBothRemoveTheLevel) {
    Rig rig;
    rig.load();
    rig.book.apply(update({delete_level(BookSide::No, 470)}));
    EXPECT_EQ(rig.book.levels(BookSide::No), (std::vector<BookLevel>{{480, 300}, {460, 300}, {450, 100}}));
    // design §8: a Set with size 0 removes the level too.
    rig.book.apply(update({change_level(BookSide::No, 460, 0, 2)}));
    EXPECT_EQ(rig.book.levels(BookSide::No), (std::vector<BookLevel>{{480, 300}, {450, 100}}));
    EXPECT_EQ(rig.book.counters().anomalies, 0u);
}

TEST(Book, DeleteOfAPriceTheBookDoesNotHoldIsReported) {
    Rig rig;
    rig.load();
    rig.book.apply(update({delete_level(BookSide::Yes, 430)}));
    EXPECT_EQ(rig.count(Kind::DeleteOfMissingPrice), 1u);
    ASSERT_EQ(rig.anomalies.size(), 1u);
    EXPECT_EQ(rig.anomalies.front().kind, Kind::DeleteOfMissingPrice);
    EXPECT_EQ(rig.anomalies.front().price, 430);
    EXPECT_EQ(rig.anomalies.front().symbol, kSymbol);
    EXPECT_EQ(rig.book.depth(BookSide::Yes), 3u);  // nothing was removed
}

// O9 found no violation of either rule in 722 live entries, so both are worth
// watching: a New should name a price the book lacks, a Change one it holds.
TEST(Book, ActionThatContradictsTheBookIsReportedButStillApplied) {
    Rig rig;
    rig.load();
    rig.book.apply(update({new_level(BookSide::Yes, 490, 700, 2)}));
    EXPECT_EQ(rig.count(Kind::NewAtExistingPrice), 1u);
    EXPECT_EQ(rig.book.levels(BookSide::Yes)[1], (BookLevel{490, 700})) << "the feed is still the authority";

    rig.book.apply(update({change_level(BookSide::No, 440, 100, 5)}));
    EXPECT_EQ(rig.count(Kind::ChangeAtMissingPrice), 1u);
    EXPECT_EQ(rig.book.depth(BookSide::No), 5u);
}

TEST(Book, PriceLevelIsCheckedAgainstThePositionAfterTheChange) {
    Rig rig;
    rig.load();
    // Q7, confirmed by O9: 1023 is the position after the change.
    rig.book.apply(update({new_level(BookSide::Yes, 495, 100, 2)}));
    EXPECT_EQ(rig.book.counters().price_levels_checked, 1u);
    EXPECT_EQ(rig.count(Kind::PriceLevelMismatch), 0u);

    rig.book.apply(update({new_level(BookSide::Yes, 475, 100, 2)}));  // really lands 5th
    EXPECT_EQ(rig.count(Kind::PriceLevelMismatch), 1u);
    ASSERT_FALSE(rig.anomalies.empty());
    EXPECT_NE(rig.anomalies.back().detail.find("1023=2 but the price sits at 5 of 5"), std::string::npos)
        << rig.anomalies.back().detail;
}

TEST(Book, DeltaWithoutAPriceLevelIsNotChecked) {
    Rig rig;
    rig.load();
    rig.book.apply(update({{BookSide::Yes, BookAction::Set, 495, 100, 0, true}}));
    EXPECT_EQ(rig.book.counters().price_levels_checked, 0u);
    EXPECT_EQ(rig.book.counters().anomalies, 0u);
}

TEST(Book, MoreThanTenLevelsOnASideIsReported) {
    Rig rig;
    std::vector<BookLevel> yes;
    for (cpm::Price price = 500; price > 490; --price) yes.push_back({price, 100});
    rig.book.apply(snapshot(yes, {}));  // exactly 10
    EXPECT_EQ(rig.book.counters().anomalies, 0u);
    rig.anomalies.clear();

    rig.book.apply(update({new_level(BookSide::Yes, 489, 100, 11)}));
    EXPECT_EQ(rig.count(Kind::DepthExceeded), 1u);
    EXPECT_EQ(rig.book.depth(BookSide::Yes), 11u) << "the level is kept; the anomaly is the signal";
}

TEST(Book, PriceOutsideTheTradableRangeIsDropped) {
    Rig rig;
    rig.load();
    rig.book.apply(update({new_level(BookSide::Yes, 0, 100, 1), new_level(BookSide::Yes, 1000, 100, 1)}));
    EXPECT_EQ(rig.count(Kind::PriceOutOfRange), 2u);
    EXPECT_EQ(rig.book.depth(BookSide::Yes), 3u) << "neither level entered the book";
}

// Q9: both sides are bids, so their tops must sum to less than 1.000.
TEST(Book, CrossedAndLockedBooksAreDetected) {
    Rig rig;
    rig.load();
    EXPECT_FALSE(rig.book.crossed_or_locked());
    EXPECT_EQ(rig.book.best(BookSide::Yes)->price + rig.book.best(BookSide::No)->price, 980);

    // Locked: 0.520 + 0.480 = 1.000 exactly.
    rig.book.apply(update({new_level(BookSide::Yes, 520, 100, 1)}));
    EXPECT_TRUE(rig.book.crossed_or_locked());
    EXPECT_EQ(rig.count(Kind::CrossedOrLocked), 1u);
    EXPECT_NE(rig.anomalies.back().detail.find("best YES 0.52 + best NO 0.48 >= 1.000"), std::string::npos)
        << rig.anomalies.back().detail;

    // Crossed: 0.600 + 0.480 = 1.080.
    rig.book.apply(update({new_level(BookSide::Yes, 600, 100, 1)}));
    EXPECT_TRUE(rig.book.crossed_or_locked());
    EXPECT_EQ(rig.count(Kind::CrossedOrLocked), 2u);

    // And it clears when the far side goes away.
    rig.book.apply(update({delete_level(BookSide::No, 480)}));
    EXPECT_EQ(rig.book.best(BookSide::No)->price, 470);
    EXPECT_TRUE(rig.book.crossed_or_locked());  // 0.600 + 0.470 still >= 1.000
    rig.book.apply(update({delete_level(BookSide::Yes, 600)}));
    EXPECT_FALSE(rig.book.crossed_or_locked());
}

TEST(Book, ImpliedAsksComeFromTheOppositeBid) {
    Rig rig;
    rig.load();
    // spec §5: "best NO bid 0.380 -> yes_ask = 0.620".
    EXPECT_EQ(rig.book.implied_yes_ask(), 520);  // 1.000 - 0.480
    EXPECT_EQ(rig.book.implied_no_ask(), 500);   // 1.000 - 0.500

    rig.book.apply(snapshot({{500, 100}}, {}));
    EXPECT_FALSE(rig.book.implied_yes_ask().has_value()) << "no NO bid, no implied YES ask";
    EXPECT_EQ(rig.book.implied_no_ask(), 500);
}

TEST(Book, UpdateBeforeAnySnapshotIsRefused) {
    Rig rig;
    rig.book.apply(update({new_level(BookSide::Yes, 500, 100, 1)}));
    EXPECT_FALSE(rig.book.valid());
    EXPECT_TRUE(rig.book.levels(BookSide::Yes).empty());
    EXPECT_EQ(rig.count(Kind::UpdateBeforeSnapshot), 1u);
    EXPECT_EQ(rig.book.counters().updates, 0u);
}

TEST(Book, InvalidateEmptiesTheBookUntilTheNextSnapshot) {
    Rig rig;
    rig.load();
    rig.book.invalidate();
    EXPECT_FALSE(rig.book.valid());
    EXPECT_TRUE(rig.book.levels(BookSide::Yes).empty());
    EXPECT_TRUE(rig.book.levels(BookSide::No).empty());

    rig.book.apply(update({new_level(BookSide::Yes, 500, 100, 1)}));
    EXPECT_EQ(rig.count(Kind::UpdateBeforeSnapshot), 1u);
    rig.load();
    EXPECT_TRUE(rig.book.valid());
    EXPECT_EQ(rig.book.depth(BookSide::Yes), 3u);
}

// Q8, confirmed by O9: an L1 feed deletes the old best and sends the new one.
TEST(Book, L1PatternKeepsOneLevelPerSide) {
    Rig rig;
    rig.book.apply(snapshot({{500, 500}}, {{480, 300}}));
    rig.anomalies.clear();

    rig.book.apply(update({change_level(BookSide::Yes, 500, 300, 1)}));  // size change
    EXPECT_EQ(rig.book.levels(BookSide::Yes), (std::vector<BookLevel>{{500, 300}}));

    rig.book.apply(update({delete_level(BookSide::Yes, 500), new_level(BookSide::Yes, 510, 200, 1)}));
    EXPECT_EQ(rig.book.levels(BookSide::Yes), (std::vector<BookLevel>{{510, 200}}));
    EXPECT_EQ(rig.book.counters().anomalies, 0u) << to_string(rig.anomalies.front().kind);
    EXPECT_EQ(rig.book.last_update(), cpm::md::Clock::time_point{} + std::chrono::seconds(2));
}

TEST(Book, CountersTrackWhatWasApplied) {
    Rig rig;
    rig.load();
    rig.book.apply(update({change_level(BookSide::Yes, 500, 100, 1), delete_level(BookSide::No, 450)}));
    const auto& counters = rig.book.counters();
    EXPECT_EQ(counters.snapshots, 1u);
    EXPECT_EQ(counters.updates, 1u);
    EXPECT_EQ(counters.level_changes, 2u);
    EXPECT_EQ(counters.price_levels_checked, 1u);  // the delete carried no 1023
    EXPECT_EQ(counters.anomalies, 0u);
}

}  // namespace
