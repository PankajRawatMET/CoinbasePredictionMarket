#include "cpm/strategy/quoting_mm.hpp"
#include "cpm/strategy/host.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <sstream>

namespace {
using namespace std::chrono_literals;
using namespace cpm::strategy;
using cpm::oms::ExecType;
using cpm::oms::Side;
constexpr std::int64_t kSpot = 10000000000;  // 100.00 at the reference scale
const std::string kSymbol = "BTCD15M-BTC26NOV111215-0";

QuotingMmParams parameters() {
    QuotingMmParams p;
    p.symbol = kSymbol;
    p.correction = 1;
    p.vol_lookback_seconds = 10;
    p.opening_price = kSpot;
    return p;
}

InstrumentView empty_book() {
    InstrumentView b;
    b.symbol = kSymbol;
    b.book_valid = true;
    b.status = cpm::md::LifecycleStatus::Open;
    return b;
}

cpm::binance::ReferenceView reference(Clock::time_point now, std::int64_t spot = kSpot, int seconds = 1202) {
    cpm::binance::ReferenceHistory history;
    const auto end = std::chrono::floor<std::chrono::seconds>(now);
    for (int i = seconds; i >= 0; --i) {
        history.observe(kSpot + (i % 2 ? 1000000 : -1000000), end - std::chrono::seconds(i));
    }
    cpm::binance::ReferenceView r;
    r.present = true;
    r.stale = false;
    r.mid = spot;
    r.tick_size = 1000000;  // 0.01
    r.history = history.view();
    return r;
}

struct Fixture {
    explicit Fixture(QuotingMmParams p = parameters()) {
        hooks.can_submit = [this] { return allowed; };
        hooks.send = [this](const NewOrder& order) {
            if (refuse) throw std::runtime_error("submission refused");
            sent.push_back(order);
            return order.cl_ord_id;
        };
        hooks.cancel = [this](const CancelOrder& order) { cancels.push_back(order); return true; };
        hooks.next_cl_ord_id = [this] { return "q" + std::to_string(++counter); };
        hooks.stop_me = [this](const std::string& why) { stop_reason = why; };
        strategy = std::make_unique<QuotingMm>(p, hooks);
        now = cpm::md::parse_ticker(kSymbol)->window_end - 600s;
    }
    void pass(std::int64_t spot = kSpot) {
        strategy->on_reference(reference(now, spot), now);
        strategy->on_market(book, position, now);
    }
    void confirm_cancels() {
        for (const auto& cancel : cancels) {
            ExecutionReport r;
            r.cl_ord_id = cancel.cl_ord_id;
            r.orig_cl_ord_id = cancel.orig_cl_ord_id;
            r.exec_type = ExecType::Canceled;
            strategy->on_report(r);
        }
    }
    StrategyHooks hooks;
    Clock::time_point now;
    InstrumentView book = empty_book();
    Position position;
    bool allowed = true, refuse = false;
    int counter = 0;
    std::string stop_reason;
    std::vector<NewOrder> sent;
    std::vector<CancelOrder> cancels;
    std::unique_ptr<QuotingMm> strategy;
};

TEST(QuotingMmQuotes, MakesTheMarketWithNoOtherParticipants) {
    const auto q = quoting_mm_quotes(0.62, empty_book(), Position{}, parameters());
    ASSERT_TRUE(q.bid);
    ASSERT_TRUE(q.ask);
    EXPECT_EQ(q.bid->price, 600);
    EXPECT_EQ(q.ask->price, 640);
    EXPECT_EQ(q.bid->quantity, 100);
}

TEST(QuotingMmQuotes, AcceptsOneSideAndDoesNotCrossIt) {
    auto b = empty_book();
    b.yes.push_back({650, 100});
    auto q = quoting_mm_quotes(0.62, b, Position{}, parameters());
    ASSERT_TRUE(q.ask);
    EXPECT_GT(q.ask->price, 650);
    b.yes.clear();
    b.no.push_back({450, 100});  // YES ask .55
    q = quoting_mm_quotes(0.62, b, Position{}, parameters());
    ASSERT_TRUE(q.bid);
    EXPECT_LT(q.bid->price, 550);
}

TEST(QuotingMmQuotes, InvalidAndCrossedBooksAreDifferentFromEmptyBooks) {
    auto b = empty_book();
    b.book_valid = false;
    EXPECT_FALSE(quoting_mm_quotes(0.5, b, Position{}, parameters()).bid);
    b.book_valid = true;
    b.yes.push_back({600, 100});
    b.no.push_back({500, 100});
    EXPECT_FALSE(quoting_mm_quotes(0.5, b, Position{}, parameters()).bid);
}

TEST(QuotingMmQuotes, AllTargetsStayOnGridAndWithinOrderBounds) {
    for (auto grid : {cpm::md::TickStructure::LinearCent, cpm::md::TickStructure::DeciCent,
                      cpm::md::TickStructure::TaperedDeciCent, cpm::md::TickStructure::TaperedCent}) {
        auto p = parameters();
        p.ticks = grid;
        for (int i = 0; i <= 1000; ++i) {
            const auto q = quoting_mm_quotes(i / 1000.0, empty_book(), Position{}, p);
            for (const auto& side : {q.bid, q.ask}) {
                if (!side) continue;
                EXPECT_GT(side->price, 0);
                EXPECT_LT(side->price, 1000);
                EXPECT_EQ(side->price, cpm::md::snap_down(side->price, grid));
            }
        }
    }
}

TEST(QuotingMmQuotes, CapsWorstCaseExposureAndStopsAtTheLimitEvenWithoutABook) {
    Position position;
    position.net = 475;
    auto q = quoting_mm_quotes(0.5, empty_book(), position, parameters());
    ASSERT_TRUE(q.bid);
    EXPECT_EQ(q.bid->quantity, 25);
    EXPECT_EQ(q.ask->quantity, 100);
    position.net = -475;
    q = quoting_mm_quotes(0.5, empty_book(), position, parameters());
    EXPECT_EQ(q.ask->quantity, 25);
    position.net = 500;
    auto b = empty_book();
    b.book_valid = false;
    EXPECT_TRUE(quoting_mm_quotes(0.5, b, position, parameters()).stop);
}

TEST(QuotingMm, PlacesTwoPostOnlyQuotesOnAnEmptyBook) {
    Fixture f;
    f.pass();
    ASSERT_EQ(f.sent.size(), 2u);
    EXPECT_EQ(f.sent[0].price, 480);
    EXPECT_EQ(f.sent[1].price, 520);
    EXPECT_EQ(f.sent[0].post_only, cpm::oms::PostOnly::Slide);
    EXPECT_EQ(f.sent[0].time_in_force, cpm::oms::TimeInForce::Gtc);
    EXPECT_EQ(f.strategy->status().diagnostics.at("Fair YES (%)"), "50.000");
    EXPECT_FALSE(f.strategy->requires_book_updates());
}

TEST(QuotingMm, DoesNotRequirePredictionMarketMidToReprice) {
    auto p = parameters();
    p.sharp_move_ticks = 0;
    Fixture f(p);
    f.pass();
    f.now += 1s;
    f.pass(kSpot + 20000000);
    EXPECT_EQ(f.cancels.size(), 2u);
    EXPECT_EQ(f.sent.size(), 2u);
    f.confirm_cancels();
    f.pass(kSpot + 20000000);
    ASSERT_EQ(f.sent.size(), 4u);
    EXPECT_GT(f.sent[2].price, 480);
}

TEST(QuotingMm, IgnoresSmallFairMovesAndKeepsItsOriginalAnchor) {
    auto p = parameters();
    p.sharp_move_ticks = 0;
    p.fair_move_ticks = 10;
    Fixture f(p);
    f.pass();
    f.pass(kSpot + 1000000);
    f.pass(kSpot + 2000000);
    EXPECT_TRUE(f.cancels.empty());
    f.pass(kSpot + 100000000);
    EXPECT_EQ(f.cancels.size(), 2u);
}

TEST(QuotingMm, RequiresReferenceOpeningAndVolatilityInsteadOfGuessing) {
    Fixture f;
    f.strategy->on_market(f.book, f.position, f.now);
    EXPECT_TRUE(f.sent.empty());
    f.strategy->on_reference(reference(f.now, kSpot, 3), f.now);
    f.strategy->on_market(f.book, f.position, f.now);
    EXPECT_TRUE(f.sent.empty());
    EXPECT_NE(f.strategy->status().reason.find("warming"), std::string::npos);
    auto p = parameters();
    p.opening_price = 0;
    Fixture late(p);
    late.strategy->on_reference(reference(late.now, kSpot, 20), late.now);
    late.strategy->on_market(late.book, late.position, late.now);
    EXPECT_TRUE(late.sent.empty());
    EXPECT_NE(late.strategy->status().reason.find("opening"), std::string::npos);
}

TEST(QuotingMm, CapturesTheWindowOpeningFromSharedHistory) {
    auto p = parameters();
    p.opening_price = 0;
    Fixture f(p);
    f.pass();
    ASSERT_EQ(f.sent.size(), 2u);
    EXPECT_NE(f.strategy->status().diagnostics.at("Opening price"), "unavailable");
}

TEST(QuotingMm, WithdrawsOnStaleReferenceEvenWithoutAnotherMarketPass) {
    Fixture f;
    f.pass();
    auto r = reference(f.now);
    r.stale = true;
    f.strategy->on_reference(r, f.now);
    EXPECT_EQ(f.cancels.size(), 2u);
}

TEST(QuotingMm, SharpMoveCancelsAndWaitsForQuietAndCancelConfirmations) {
    auto p = parameters();
    p.sharp_move_ticks = 5;
    Fixture f(p);
    f.pass();
    const auto moved = kSpot + 10000000;
    f.strategy->on_reference(reference(f.now, moved), f.now);
    ASSERT_EQ(f.cancels.size(), 2u);
    f.now += 3s;
    f.pass(moved);
    EXPECT_EQ(f.sent.size(), 2u);  // still waiting for cancellations
    f.confirm_cancels();
    f.pass(moved);
    EXPECT_EQ(f.sent.size(), 4u);  // no prediction book movement required
}

TEST(QuotingMm, DetectsAJumpAndReversalBetweenHostPasses) {
    auto p = parameters();
    p.sharp_move_ticks = 5;
    Fixture f(p);
    f.pass();
    auto r = reference(f.now);
    auto samples = std::make_shared<cpm::binance::HistoryView>(*r.history);
    samples->back().high = kSpot + 10000000;
    r.history = samples;
    f.strategy->on_reference(r, f.now + 100ms);
    EXPECT_EQ(f.cancels.size(), 2u);
}

TEST(QuotingMm, DetectsARoundTripEvenIfTheHostWasDelayedSeveralSeconds) {
    auto p = parameters();
    p.sharp_move_ticks = 5;
    Fixture f(p);
    f.pass();
    const auto previous = f.now;
    f.now += 5s;
    auto r = reference(f.now);
    auto samples = std::make_shared<cpm::binance::HistoryView>(*r.history);
    for (auto& sample : *samples) {
        if (sample.second == previous + 1s) sample.high = kSpot + 10000000;
    }
    r.history = samples;
    f.strategy->on_reference(r, f.now);
    EXPECT_EQ(f.cancels.size(), 2u);
}

TEST(QuotingMm, ReplenishingOneSideDoesNotResetTheOlderSidesGuard) {
    auto p = parameters();
    p.sharp_move_ticks = 5;
    p.fair_move_ticks = 500;  // keep the unfilled ask at its original target
    Fixture f(p);
    f.pass();
    ExecutionReport fill;
    fill.cl_ord_id = f.sent.front().cl_ord_id;
    fill.exec_type = ExecType::Trade;
    fill.leaves_quantity = 0;
    f.strategy->on_report(fill);
    f.pass(kSpot + 4000000);  // replenish the bid below the sharp threshold
    ASSERT_EQ(f.sent.size(), 3u);
    f.strategy->on_reference(reference(f.now, kSpot + 6000000), f.now);
    // The old ask has moved six ticks, though the new bid moved only two.
    EXPECT_EQ(f.cancels.size(), 2u);
}

TEST(QuotingMm, CutoffCancelsAndWindowCloseAbandonsWithoutCancelRaces) {
    Fixture f;
    f.pass();
    f.now = cpm::md::parse_ticker(kSymbol)->window_end - 25s;
    f.pass();
    EXPECT_EQ(f.cancels.size(), 2u);
    EXPECT_EQ(f.sent.size(), 2u);
    Fixture closed;
    closed.pass();
    closed.now = cpm::md::parse_ticker(kSymbol)->window_end;
    closed.pass();
    EXPECT_TRUE(closed.cancels.empty());
    EXPECT_TRUE(closed.strategy->quiescent());
}

TEST(QuotingMm, FillsReplenishAndCancelReportsCanMatchOriginalIds) {
    Fixture f;
    f.pass();
    ExecutionReport r;
    r.cl_ord_id = f.sent[0].cl_ord_id;
    r.exec_type = ExecType::Trade;
    r.leaves_quantity = 0;
    f.strategy->on_report(r);
    f.pass();
    EXPECT_EQ(f.sent.size(), 3u);
    f.strategy->withdraw("stop");
    EXPECT_EQ(f.cancels.size(), 2u);
    f.confirm_cancels();
    EXPECT_TRUE(f.strategy->quiescent());
}

TEST(QuotingMm, InventoryStopsEvenWhenReferenceIsNotReady) {
    Fixture f;
    f.position.net = 500;
    f.strategy->on_market(f.book, f.position, f.now);
    EXPECT_FALSE(f.stop_reason.empty());
    EXPECT_TRUE(f.sent.empty());
}

TEST(QuotingMm, SubmissionGateAndPreWireRefusalsAreRespected) {
    Fixture f;
    f.allowed = false;
    f.pass();
    EXPECT_TRUE(f.sent.empty());
    f.allowed = true;
    f.refuse = true;
    EXPECT_NO_THROW(f.pass());
    EXPECT_TRUE(f.sent.empty());
    f.refuse = false;
    f.pass();
    EXPECT_EQ(f.sent.size(), 2u);
}

TEST(QuotingMmSchema, RequiresCalibrationAndRejectsUnknownOrInvalidInputs) {
    StrategyRegistry registry;
    register_quoting_mm(registry);
    EXPECT_FALSE(registry.create({"QuotingMM", {{"symbol", kSymbol}}}, {}).ok());
    EXPECT_FALSE(registry.create({"QuotingMM", {{"symbol", kSymbol}, {"correction-factor", "0"}}}, {}).ok());
    EXPECT_FALSE(registry.create({"QuotingMM", {{"symbol", "SIMGAME-123-YES"}, {"correction-factor", "1"}}}, {}).ok());
    auto result = registry.create({"QuotingMM", {{"symbol", kSymbol}, {"correction-factor", "1.25"}}}, {});
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.values.at("withdraw-before"), "25");
    EXPECT_EQ(result.values.at("vol-lookback-seconds"), "1200");
}

TEST(QuotingMmHost, UsesTheExistingHostWithAnEmptyBookAndResetsWindowOverridesOnRoll) {
    Fixture venue;
    StrategyRegistry registry;
    register_quoting_mm(registry);
    StrategyHost::Feeds feeds;
    feeds.book = [](const std::string&) { return std::optional<InstrumentView>{empty_book()}; };
    feeds.reference = [&](const std::string&) { return reference(venue.now); };
    std::ostringstream log;
    cpm::Logger logger(log);
    StrategyHost host({}, registry, venue.hooks, feeds, logger);
    std::string id;
    ASSERT_TRUE(host.create({"QuotingMM", {{"symbol", kSymbol}, {"correction-factor", "1"},
                         {"opening-price", "100"}}}, id, "BTCD15M").empty());
    ASSERT_TRUE(host.start(id).empty());
    host.tick(venue.now);
    EXPECT_TRUE(venue.sent.empty());  // starts disarmed
    ASSERT_TRUE(host.arm(id).empty());
    host.tick(venue.now);
    EXPECT_EQ(venue.sent.size(), 2u);
    EXPECT_FALSE(host.list().front().requires_book_updates);
    ASSERT_TRUE(host.roll(id, "BTCD15M-BTC26NOV111230-0").empty());
    EXPECT_EQ(host.list().front().params.at("opening-price"), "0");
    EXPECT_TRUE(host.list().front().armed);
}
}  // namespace
