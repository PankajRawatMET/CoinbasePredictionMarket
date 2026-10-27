// Per-instrument state built from decoded events: which subscription owns a
// book (O7), trade dedup, volume and open interest, and lifecycle per Q13/Q14.
#include "cpm/md/market_state.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using cpm::md::Aggressor;
using cpm::md::BookAction;
using cpm::md::BookAnomaly;
using cpm::md::BookLevel;
using cpm::md::BookSide;
using cpm::md::BookSnapshot;
using cpm::md::BookUpdate;
using cpm::md::InstrumentLifecycle;
using cpm::md::LifecycleStatus;
using cpm::md::MarketState;
using cpm::md::OpenInterest;
using cpm::md::Trade;
using cpm::md::Volume;

const std::string kSym = "BTCD15M-BTC26OCT260545-0";
const std::string kId = "240346204372729859";

cpm::md::Clock::time_point at(int secs) {
    return cpm::md::Clock::time_point{} + std::chrono::seconds(secs);
}

BookSnapshot snapshot(const std::string& req_id, std::vector<BookLevel> yes, std::vector<BookLevel> no) {
    BookSnapshot s;
    s.md_req_id = req_id;
    s.symbol = kSym;
    s.security_id = kId;
    s.yes = std::move(yes);
    s.no = std::move(no);
    s.transact_time = at(1);
    return s;
}

BookUpdate update(const std::string& req_id, std::vector<cpm::md::BookLevelChange> changes) {
    BookUpdate u;
    u.md_req_id = req_id;
    u.symbol = kSym;
    u.security_id = kId;
    u.changes = std::move(changes);
    u.transact_time = at(2);
    return u;
}

Trade trade(const std::string& req_id, const std::string& entry_id, cpm::Price price = 500) {
    Trade t;
    t.md_req_id = req_id;
    t.symbol = kSym;
    t.security_id = kId;
    t.yes_price = price;
    t.qty = 200;
    t.aggressor = Aggressor::YesTaker;
    t.entry_id = entry_id;
    t.transact_time = at(3);
    return t;
}

InstrumentLifecycle lifecycle(LifecycleStatus status, int code, int secs) {
    InstrumentLifecycle e;
    e.symbol = kSym;
    e.security_id = kId;
    e.status = status;
    e.status_code = code;
    e.transact_time = at(secs);
    return e;
}

// O7: the gateway sends every update once per MDReqID, so a client holding
// both an L2 and an L1 book subscription receives each change twice.
TEST(MarketStateTest, OnlyTheSubscriptionThatSnapshottedTheSymbolUpdatesItsBook) {
    MarketState state;
    state.apply(snapshot("book-1", {{500, 500}, {490, 600}}, {{480, 300}}));
    state.apply(update("book-1", {{BookSide::Yes, BookAction::Set, 495, 100, 2, true}}));

    // The L1 subscription's copy: "delete the old best, here is the new one".
    // Applied to the L2 book it would wrongly drop a level that still rests.
    state.apply(update("book-2", {{BookSide::Yes, BookAction::Delete, 500, 0, 0, false},
                                  {BookSide::Yes, BookAction::Set, 510, 100, 1, true}}));

    const auto view = state.instrument(kSym);
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view->book_md_req_id, "book-1");
    EXPECT_EQ(view->yes, (std::vector<BookLevel>{{500, 500}, {495, 100}, {490, 600}}));
    EXPECT_EQ(state.ignored_foreign_updates(), 1u);
    EXPECT_EQ(view->book_counters.anomalies, 0u);
}

// The L2 and the L1 subscription both snapshot the same instrument (O7). The
// first one keeps the book; the other would shrink it to its top level.
TEST(MarketStateTest, ASnapshotFromAnotherSubscriptionDoesNotTakeOverTheBook) {
    MarketState state;
    state.apply(snapshot("book-1", {{500, 500}, {490, 600}}, {{480, 300}}));
    state.apply(snapshot("book-2", {{500, 500}}, {{480, 300}}));

    const auto view = state.instrument(kSym);
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view->book_md_req_id, "book-1");
    EXPECT_EQ(view->yes.size(), 2u);
    EXPECT_EQ(state.ignored_foreign_snapshots(), 1u);
}

// Stage 5 invalidates, then resubscribes with a fresh MDReqID, and that
// subscription's snapshot takes ownership.
TEST(MarketStateTest, InvalidatingUnbindsSoAResubscribeTakesOver) {
    MarketState state;
    state.apply(snapshot("book-1", {{500, 500}}, {}));
    state.invalidate_books();
    state.apply(snapshot("book-7", {{490, 100}, {480, 200}}, {{470, 100}}));
    state.apply(update("book-7", {{BookSide::Yes, BookAction::Set, 490, 300, 1, false}}));

    const auto view = state.instrument(kSym);
    EXPECT_EQ(view->book_md_req_id, "book-7");
    EXPECT_EQ(view->yes, (std::vector<BookLevel>{{490, 300}, {480, 200}}));
    EXPECT_EQ(state.ignored_foreign_updates(), 0u);
    EXPECT_EQ(state.ignored_foreign_snapshots(), 0u);
}

TEST(MarketStateTest, AnomaliesReachTheHandlerWithoutTheStateLocked) {
    MarketState state;
    std::vector<BookAnomaly> seen;
    state.on_anomaly([&](const BookAnomaly& anomaly) {
        seen.push_back(anomaly);
        // Reading the state back from the handler must not deadlock.
        EXPECT_TRUE(state.instrument(kSym).has_value());
    });
    state.apply(snapshot("book-1", {{500, 100}}, {}));
    state.apply(update("book-1", {{BookSide::Yes, BookAction::Delete, 430, 0, 0, false}}));
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen.front().kind, BookAnomaly::Kind::DeleteOfMissingPrice);
    EXPECT_EQ(seen.front().price, 430);
}

// An update with no book to apply it to is ordinary: a subscription being set
// up, or Stage 5 recovery where in-flight updates outlive an unsubscribe.
TEST(MarketStateTest, UpdatesWithNoBookYetAreCountedNotFlagged) {
    MarketState state;
    std::vector<BookAnomaly> seen;
    state.on_anomaly([&](const BookAnomaly& anomaly) { seen.push_back(anomaly); });

    state.apply(update("book-1", {{BookSide::Yes, BookAction::Set, 500, 100, 1, true}}));
    EXPECT_TRUE(seen.empty());
    EXPECT_EQ(state.updates_awaiting_snapshot(), 1u);
    EXPECT_FALSE(state.instrument(kSym)->book_valid);
    EXPECT_EQ(state.totals().anomalies, 0u);

    // And again after a recovery invalidates the book.
    state.apply(snapshot("book-1", {{500, 100}}, {}));
    state.invalidate_books();
    state.apply(update("book-1", {{BookSide::Yes, BookAction::Set, 500, 200, 1, false}}));
    EXPECT_TRUE(seen.empty());
    EXPECT_EQ(state.updates_awaiting_snapshot(), 2u);
    EXPECT_FALSE(state.instrument(kSym)->book_valid);
}

// The same execution arrives under both the per-instrument and the
// all-instrument trade subscription (O7), so 278 is the dedup key.
TEST(MarketStateTest, TheSameTradeUnderTwoSubscriptionsCountsOnce) {
    MarketState state;
    state.apply(trade("trades-3", "900000000000000017", 530));
    state.apply(trade("trades-5", "900000000000000017", 530));
    state.apply(trade("trades-3", "900000000000000018", 540));

    const auto view = state.instrument(kSym);
    ASSERT_TRUE(view->last_trade.has_value());
    EXPECT_EQ(view->trades, 2u);
    EXPECT_EQ(view->duplicate_trades, 1u);
    EXPECT_EQ(view->last_trade->yes_price, 540);
    EXPECT_EQ(view->last_trade->entry_id, "900000000000000018");
    EXPECT_EQ(view->last_trade->aggressor, Aggressor::YesTaker);
    EXPECT_EQ(view->last_trade->time, at(3));
}

TEST(MarketStateTest, ATradeWithoutAnEntryIdCannotBeDedupedAndIsCounted) {
    MarketState state;
    state.apply(trade("trades-3", "", 500));
    state.apply(trade("trades-3", "", 500));
    const auto view = state.instrument(kSym);
    EXPECT_EQ(view->trades, 2u);
    EXPECT_EQ(view->trades_without_id, 2u);
    EXPECT_EQ(view->duplicate_trades, 0u);
}

TEST(MarketStateTest, TheDedupWindowIsBounded) {
    MarketState state;
    const auto first = std::to_string(1);
    for (std::size_t i = 1; i <= MarketState::kTradeDedupWindow + 1; ++i) {
        state.apply(trade("trades-3", std::to_string(i)));
    }
    // The oldest id has been forgotten, so it is accepted a second time.
    state.apply(trade("trades-3", first));
    const auto view = state.instrument(kSym);
    EXPECT_EQ(view->duplicate_trades, 0u);
    EXPECT_EQ(view->trades, MarketState::kTradeDedupWindow + 2);
}

TEST(MarketStateTest, VolumeAndOpenInterestAreAbsoluteAndStartUnset) {
    MarketState state;
    state.apply(snapshot("book-1", {{500, 100}}, {}));
    auto view = state.instrument(kSym);
    EXPECT_FALSE(view->volume.has_value());
    EXPECT_FALSE(view->open_interest.has_value());

    Volume volume;
    volume.symbol = kSym;
    volume.security_id = kId;
    volume.contracts = 7400;
    state.apply(volume);
    volume.contracts = 10300;
    state.apply(volume);

    OpenInterest oi;
    oi.symbol = kSym;
    oi.contracts = 1000;
    state.apply(oi);
    oi.contracts = 900;  // open interest may fall
    state.apply(oi);

    view = state.instrument(kSym);
    EXPECT_EQ(view->volume, 10300);
    EXPECT_EQ(view->open_interest, 900);
}

// Q13: unknown until the first f. Q14: CPM meanings, Amended is an event,
// every transition recorded and none rejected.
TEST(MarketStateTest, LifecycleRecordsEveryTransition) {
    MarketState state;
    state.apply(snapshot("book-1", {}, {}));
    EXPECT_EQ(state.instrument(kSym)->status, LifecycleStatus::Unknown);

    state.apply(lifecycle(LifecycleStatus::Listed, 21, 10));
    state.apply(lifecycle(LifecycleStatus::Open, 17, 11));
    state.apply(lifecycle(LifecycleStatus::Amended, 26, 12));
    state.apply(lifecycle(LifecycleStatus::Closed, 18, 13));
    state.apply(lifecycle(LifecycleStatus::Determined, 27, 14));
    state.apply(lifecycle(LifecycleStatus::Settled, 28, 15));

    const auto view = state.instrument(kSym);
    EXPECT_EQ(view->status, LifecycleStatus::Settled);
    EXPECT_EQ(view->status_code, 28);
    EXPECT_EQ(view->status_time, at(15));
    EXPECT_EQ(view->amendments, 1u);
    ASSERT_EQ(view->history.size(), 6u);
    EXPECT_EQ(view->history[2].first, LifecycleStatus::Amended) << "recorded even though it is not a state";
    EXPECT_EQ(view->history.back().second, at(15));
}

// O10: a per-instrument and an all-instrument lifecycle subscription both
// deliver the roll, and f carries no id, so the same 326 at the same
// TransactTime is the same event rather than a second transition.
TEST(MarketStateTest, TheSameTransitionUnderTwoSubscriptionsIsRecordedOnce) {
    MarketState state;
    state.apply(lifecycle(LifecycleStatus::Closed, 18, 10));
    state.apply(lifecycle(LifecycleStatus::Closed, 18, 10));  // the second subscription's copy
    state.apply(lifecycle(LifecycleStatus::Determined, 27, 11));
    state.apply(lifecycle(LifecycleStatus::Determined, 27, 11));
    state.apply(lifecycle(LifecycleStatus::Settled, 28, 12));

    const auto view = state.instrument(kSym);
    EXPECT_EQ(view->status, LifecycleStatus::Settled);
    EXPECT_EQ(view->duplicate_status_events, 2u);
    ASSERT_EQ(view->history.size(), 3u);
    EXPECT_EQ(view->history[0].first, LifecycleStatus::Closed);
    EXPECT_EQ(view->history[1].first, LifecycleStatus::Determined);
    EXPECT_EQ(view->history[2].first, LifecycleStatus::Settled);
}

// A genuine repeat of a status at a different time is a transition (Q14).
TEST(MarketStateTest, TheSameStatusAtADifferentTimeIsANewTransition) {
    MarketState state;
    state.apply(lifecycle(LifecycleStatus::Pause, 2, 10));
    state.apply(lifecycle(LifecycleStatus::Open, 17, 11));
    state.apply(lifecycle(LifecycleStatus::Pause, 2, 12));

    const auto view = state.instrument(kSym);
    EXPECT_EQ(view->duplicate_status_events, 0u);
    EXPECT_EQ(view->history.size(), 3u);
    EXPECT_EQ(view->status, LifecycleStatus::Pause);
    EXPECT_EQ(view->status_time, at(12));
}

TEST(MarketStateTest, AmendedDoesNotChangeTheTradingState) {
    MarketState state;
    state.apply(lifecycle(LifecycleStatus::Open, 17, 10));
    state.apply(lifecycle(LifecycleStatus::Amended, 26, 11));
    const auto view = state.instrument(kSym);
    EXPECT_EQ(view->status, LifecycleStatus::Open);
    EXPECT_EQ(view->status_code, 17);
    EXPECT_EQ(view->status_time, at(10));
    EXPECT_EQ(view->amendments, 1u);
}

TEST(MarketStateTest, AnUndefinedStatusKeepsTheLastKnownState) {
    MarketState state;
    state.apply(lifecycle(LifecycleStatus::Open, 17, 10));
    state.apply(lifecycle(LifecycleStatus::Unknown, 99, 11));  // the decoder already reported it
    const auto view = state.instrument(kSym);
    EXPECT_EQ(view->status, LifecycleStatus::Open);
    EXPECT_EQ(view->unknown_status_events, 1u);
    EXPECT_EQ(view->history.size(), 2u);
}

TEST(MarketStateTest, HistoryIsBounded) {
    MarketState state;
    for (std::size_t i = 0; i < MarketState::kHistoryLimit + 5; ++i) {
        state.apply(lifecycle(LifecycleStatus::Open, 17, static_cast<int>(i)));
    }
    const auto view = state.instrument(kSym);
    EXPECT_EQ(view->history.size(), MarketState::kHistoryLimit);
    EXPECT_EQ(view->history.front().second, at(5)) << "the oldest entries are dropped";
}

// Stage 5: books are rebuilt from a fresh snapshot; the channels that have no
// snapshot keep what they have.
TEST(MarketStateTest, InvalidateBooksKeepsTradesVolumeAndLifecycle) {
    MarketState state;
    state.apply(snapshot("book-1", {{500, 100}}, {{480, 100}}));
    state.apply(trade("trades-3", "900000000000000017"));
    state.apply(lifecycle(LifecycleStatus::Open, 17, 10));
    Volume volume;
    volume.symbol = kSym;
    volume.contracts = 500;
    state.apply(volume);

    state.invalidate_books();
    const auto view = state.instrument(kSym);
    EXPECT_FALSE(view->book_valid);
    EXPECT_TRUE(view->yes.empty());
    EXPECT_TRUE(view->no.empty());
    EXPECT_TRUE(view->last_trade.has_value());
    EXPECT_EQ(view->volume, 500);
    EXPECT_EQ(view->status, LifecycleStatus::Open);
}

// A long run must not accumulate every instrument it ever saw: the sandbox
// lists about a hundred rolling windows a day (O2/O6).
TEST(MarketStateTest, SettledInstrumentsAreEvictedAfterAGracePeriod) {
    MarketState state;
    state.apply(snapshot("book-1", {{500, 100}}, {}));
    state.apply(trade("trades-2", "900000000000000017"));
    state.apply(lifecycle(LifecycleStatus::Settled, 28, 10));

    // Too soon: the grace period has not passed.
    EXPECT_EQ(state.evict_finished(std::chrono::seconds(60), at(30)), 0u);
    EXPECT_TRUE(state.instrument(kSym).has_value());

    EXPECT_EQ(state.evict_finished(std::chrono::seconds(60), at(120)), 1u);
    EXPECT_FALSE(state.instrument(kSym).has_value());
    EXPECT_TRUE(state.symbols().empty());
}

TEST(MarketStateTest, OnlyInstrumentsThatHaveFinishedTradingAreEvicted) {
    MarketState state;
    const auto keep = [&](LifecycleStatus status) {
        MarketState fresh;
        fresh.apply(snapshot("book-1", {{500, 100}}, {}));
        fresh.apply(lifecycle(status, 0, 10));
        return fresh.evict_finished(std::chrono::seconds(1), at(600)) == 0u;
    };
    // Closed and Determined still settle later, so they stay.
    EXPECT_TRUE(keep(LifecycleStatus::Open));
    EXPECT_TRUE(keep(LifecycleStatus::Closed));
    EXPECT_TRUE(keep(LifecycleStatus::Determined));
    EXPECT_TRUE(keep(LifecycleStatus::Pause));
    EXPECT_FALSE(keep(LifecycleStatus::Settled));
    EXPECT_FALSE(keep(LifecycleStatus::Delisted));

    // An instrument with no lifecycle event at all has nothing to age out,
    // whatever its book says.
    state.apply(snapshot("book-1", {{500, 100}}, {}));
    EXPECT_EQ(state.evict_finished(std::chrono::seconds(1), at(600)), 0u);
    EXPECT_TRUE(state.instrument(kSym).has_value());
}

TEST(MarketStateTest, CallbacksFeedEveryChannel) {
    MarketState state;
    const auto callbacks = state.callbacks();
    ASSERT_TRUE(callbacks.book_snapshot && callbacks.book_update && callbacks.trade && callbacks.volume &&
                callbacks.open_interest && callbacks.lifecycle);

    callbacks.book_snapshot(snapshot("book-1", {{500, 100}}, {{480, 200}}));
    callbacks.book_update(update("book-1", {{BookSide::Yes, BookAction::Set, 500, 300, 1, false}}));
    callbacks.trade(trade("trades-2", "900000000000000017"));
    Volume volume;
    volume.symbol = kSym;
    volume.contracts = 1200;
    callbacks.volume(volume);
    OpenInterest oi;
    oi.symbol = kSym;
    oi.contracts = 400;
    callbacks.open_interest(oi);
    callbacks.lifecycle(lifecycle(LifecycleStatus::Open, 17, 10));

    const auto view = state.instrument(kSym);
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view->yes, (std::vector<BookLevel>{{500, 300}}));
    EXPECT_EQ(view->implied_yes_ask, 520);
    EXPECT_EQ(view->trades, 1u);
    EXPECT_EQ(view->volume, 1200);
    EXPECT_EQ(view->open_interest, 400);
    EXPECT_EQ(view->status, LifecycleStatus::Open);
    EXPECT_EQ(view->security_id, kId);
}

TEST(MarketStateTest, TracksEveryInstrumentSeparately) {
    MarketState state;
    state.apply(snapshot("book-1", {{500, 100}}, {}));
    BookSnapshot other = snapshot("book-2", {{300, 100}}, {});
    other.symbol = "ETHD15M-ETH26OCT260545-0";
    other.security_id = "240338655015600128";
    state.apply(other);

    EXPECT_EQ(state.symbols(), (std::vector<std::string>{"BTCD15M-BTC26OCT260545-0", "ETHD15M-ETH26OCT260545-0"}));
    EXPECT_EQ(state.instruments().size(), 2u);
    EXPECT_FALSE(state.instrument("NOPE-0").has_value());
    const auto totals = state.totals();
    EXPECT_EQ(totals.snapshots, 2u);
    EXPECT_EQ(totals.anomalies, 0u);
}

}  // namespace
