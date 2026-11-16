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

// What the old one-second path produced from the synthetic history below -
// ten alternating returns of equal size - so every fair value in these tests
// is the number it always was. Sigma now arrives from the window's warmup
// instead of being recomputed on each pass.
const double kSigma = std::log(10001.0 / 9999.0) * std::sqrt(10.0 / 9.0);

QuotingMmParams parameters() {
    QuotingMmParams p;
    p.symbol = kSymbol;
    p.correction = 1;
    p.sigma = kSigma;
    p.warmup_opening = kSpot;
    return p;
}

// The venue, with no socket: 22 one-minute bars ending at whatever window
// the factory asks about, alternating by a tenth of a percent so there is a
// variance to find.
std::string klines_json(std::int64_t end_ms) {
    const auto quoted = [](std::int64_t scaled) {
        return "\"" + cpm::format_scaled(scaled, kKlineScale) + "\"";
    };
    std::string out = "[";
    for (int i = 21; i >= 0; --i) {
        const std::int64_t open_ms = end_ms - static_cast<std::int64_t>(i) * 60000;
        const std::int64_t price = kSpot + (i % 2 ? kSpot / 1000 : 0);
        if (out.size() > 1) out += ",";
        out += "[" + std::to_string(open_ms) + "," + quoted(price) + "," + quoted(price) + "," +
               quoted(price) + "," + quoted(price) + ",\"1\"," + std::to_string(open_ms + 59999) +
               ",\"1\",1,\"0\",\"0\",\"0\"]";
    }
    return out + "]";
}

QuotingMmVenue venue_for_tests() {
    QuotingMmVenue venue;
    cpm::binance::Mapping mapping;
    mapping.symbol = "BTCUSDT";
    mapping.tick_size = 10000000;  // 0.1
    venue.symbols = {{"BTCD15M", mapping}};
    venue.fetch = [](const cpm::HttpsRequest& http) {
        const auto at = http.query.find("endTime=");
        if (at == std::string::npos) return cpm::HttpsResponse{400, "", "no endTime"};
        return cpm::HttpsResponse{200, klines_json(std::stoll(http.query.substr(at + 8))), ""};
    };
    return venue;
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
        hooks.log = [this](const std::string& line) { lines.push_back(line); };
        strategy = std::make_unique<QuotingMm>(p, hooks);
        now = cpm::md::parse_ticker(kSymbol)->window_end - 600s;
    }
    void pass(std::int64_t spot = kSpot) {
        strategy->on_reference(reference(now, spot), now);
        strategy->on_market(book, position, now);
    }
    // The last line the strategy wrote about its pricing, which is what
    // strategy.log carries.
    std::string last_model_line() const {
        for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
            if (it->rfind("fair ", 0) == 0) return *it;
        }
        return {};
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
    std::vector<std::string> lines;
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

TEST(QuotingMmQuotes, KeepsTheWidthAndStandsAsideRatherThanCrossATouch) {
    // Fair 0.62 makes a 0.60 / 0.64 market on this grid. A bid of 0.65 in
    // the book is above our offer, so posting it would be taking rather
    // than making - and widening past the obligation is not available.
    auto b = empty_book();
    b.yes.push_back({650, 100});
    auto q = quoting_mm_quotes(0.62, b, Position{}, parameters());
    EXPECT_FALSE(q.bid);
    EXPECT_FALSE(q.ask);
    EXPECT_NE(q.reason.find("best bid"), std::string::npos) << q.reason;

    // The same book one tick out of the way, and the market is made.
    b.yes.front().price = 630;
    q = quoting_mm_quotes(0.62, b, Position{}, parameters());
    ASSERT_TRUE(q.bid);
    ASSERT_TRUE(q.ask);
    EXPECT_EQ(q.bid->price, 600);
    EXPECT_EQ(q.ask->price, 640);

    // And the other side: a NO bid of 0.45 is a YES offer at 0.55, through
    // which our 0.60 bid may not reach.
    b.yes.clear();
    b.no.push_back({450, 100});
    q = quoting_mm_quotes(0.62, b, Position{}, parameters());
    EXPECT_FALSE(q.bid);
    EXPECT_NE(q.reason.find("best offer"), std::string::npos) << q.reason;
}

TEST(QuotingMmQuotes, NeverShowsAMarketWiderThanTheOneItWasAskedFor) {
    // The bug this replaced: fair is almost never on a grid point, and
    // rounding each leg outwards to reach one added a tick to every quote.
    // 0.528176 and an empty book came out 0.50 / 0.55 - five ticks, from a
    // four-tick parameter (logs/20261116/064526).
    auto p = parameters();
    const auto q = quoting_mm_quotes(0.528176, empty_book(), Position{}, p);
    ASSERT_TRUE(q.bid);
    ASSERT_TRUE(q.ask);
    EXPECT_EQ(q.bid->price, 510);
    EXPECT_EQ(q.ask->price, 550);

    for (int width : {1, 2, 3, 4, 7, 20}) {
        p.spread_ticks = width;
        {
            for (int i = 0; i <= 10000; ++i) {
                const auto market = quoting_mm_quotes(i / 10000.0, empty_book(), Position{}, p);
                if (!market.bid || !market.ask) continue;
                // Exactly `width` grid steps - never one more, whatever the
                // model says and wherever the taper changes the step size.
                int steps = 0;
                for (cpm::Price at = market.bid->price; at < market.ask->price; ++steps) {
                    at = cpm::md::price_above(at);
                }
                EXPECT_EQ(steps, width) << "fair " << i / 10000.0;
                // And it straddles what it is pricing.
                EXPECT_LT(market.bid->price, i / 10.0);
                EXPECT_GT(market.ask->price, i / 10.0);
            }
        }
    }
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
    {
        auto p = parameters();
        for (int i = 0; i <= 1000; ++i) {
            const auto q = quoting_mm_quotes(i / 1000.0, empty_book(), Position{}, p);
            for (const auto& side : {q.bid, q.ask}) {
                if (!side) continue;
                EXPECT_GT(side->price, 0);
                EXPECT_LT(side->price, 1000);
                EXPECT_EQ(side->price, cpm::md::snap_down(side->price));
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
    p.mid_move_threshold_ticks = 0;
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
    p.mid_move_threshold_ticks = 0;
    p.fair_move_ticks = 10;
    Fixture f(p);
    f.pass();
    f.pass(kSpot + 1000000);
    f.pass(kSpot + 2000000);
    EXPECT_TRUE(f.cancels.empty());
    f.pass(kSpot + 100000000);
    EXPECT_EQ(f.cancels.size(), 2u);
}

TEST(QuotingMm, RefusesToExistWithoutTheInputsItPricesWith) {
    // There is no "warming" state any more: an instance either has the
    // window's sigma and opening from its warmup, or it is never built.
    auto no_sigma = parameters();
    no_sigma.sigma = 0;
    EXPECT_THROW(Fixture{no_sigma}, std::invalid_argument);

    auto no_opening = parameters();
    no_opening.warmup_opening = 0;
    EXPECT_THROW(Fixture{no_opening}, std::invalid_argument);

    auto correction = parameters();
    correction.correction = 0;
    EXPECT_THROW(Fixture{correction}, std::invalid_argument);
}

TEST(QuotingMm, QuotesOnItsFirstPassWithNoWarmupAtAll) {
    // The point of the change: a strategy built mid-window quotes at once
    // rather than watching the stream for twenty minutes first.
    Fixture f;
    f.pass();
    EXPECT_EQ(f.sent.size(), 2u);
    EXPECT_TRUE(f.strategy->status().reason.empty()) << f.strategy->status().reason;
}

TEST(QuotingMm, LogsEverythingItPricedWith) {
    Fixture f;
    f.pass();
    ASSERT_EQ(f.sent.size(), 2u);

    // A finished run has to be re-priceable from its own log: the strike,
    // the underlying against it, the time left and the frozen sigma.
    const auto line = f.last_model_line();
    ASSERT_FALSE(line.empty());
    const auto at = cpm::format_scaled(kSpot, cpm::binance::kScale);
    EXPECT_NE(line.find("open " + at), std::string::npos) << line;
    EXPECT_NE(line.find("spot " + at), std::string::npos) << line;
    EXPECT_NE(line.find("z 0.000000"), std::string::npos) << line;      // spot == strike
    EXPECT_NE(line.find("ln(S/K) 0.00000000"), std::string::npos) << line;
    EXPECT_NE(line.find("fair 0.500000"), std::string::npos) << line;
    EXPECT_NE(line.find("left "), std::string::npos) << line;
    EXPECT_NE(line.find("sigma "), std::string::npos) << line;

    // And it is no longer a number to read off the operator's row.
    EXPECT_EQ(f.strategy->status().diagnostics.count("Opening price"), 0u);
    EXPECT_EQ(f.strategy->status().diagnostics.count("Opening source"), 0u);
}

TEST(QuotingMm, PricesBelowAnEvenChanceWhenTheUnderlyingIsUnderTheStrike) {
    auto below = parameters();
    below.warmup_opening = kSpot + 1000000;  // the window opened above where spot is now
    Fixture o(below);
    o.pass();

    const auto line = o.last_model_line();
    ASSERT_FALSE(line.empty());
    EXPECT_NE(line.find("open " + cpm::format_scaled(kSpot + 1000000, cpm::binance::kScale)),
              std::string::npos)
        << line;
    EXPECT_NE(line.find("z -"), std::string::npos) << line;
    EXPECT_LT(std::stod(o.strategy->status().diagnostics.at("Fair YES (%)")), 50.0);
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
    p.mid_move_threshold_ticks = 5;
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
    p.mid_move_threshold_ticks = 5;
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
    p.mid_move_threshold_ticks = 5;
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

TEST(QuotingMm, DriftAcrossUpdatesIsNotABreach) {
    // PricingMidMoveThreshold asks whether the market JUST jumped, not
    // whether a resting quote has aged. Six ticks of drift arriving two at
    // a time is not a breach, and the quotes stay where they are.
    auto p = parameters();
    p.mid_move_threshold_ticks = 5;
    p.fair_move_ticks = 500;  // hold the target still, so only the guard can act
    Fixture f(p);
    f.pass();
    ASSERT_EQ(f.sent.size(), 2u);

    for (const std::int64_t step : {2000000, 4000000, 6000000}) {  // two ticks at a time
        f.strategy->on_reference(reference(f.now, kSpot + step), f.now);
    }
    EXPECT_TRUE(f.cancels.empty()) << "drift is not a jump";

    // The same six ticks in one update is.
    f.strategy->on_reference(reference(f.now, kSpot + 16000000), f.now);
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

TEST(QuotingMmSchema, DefaultsTheCorrectionToOneAndRejectsUnknownOrInvalidInputs) {
    StrategyRegistry registry;
    register_quoting_mm(registry, venue_for_tests());
    // A correction of 1 prices straight off the measured sigma, so a row can
    // be created without one. Zero is still refused: it would divide by it.
    const auto bare = registry.create({"QuotingMM", {{"symbol", kSymbol}}}, {});
    ASSERT_TRUE(bare.ok()) << bare.error;
    EXPECT_EQ(bare.values.at("correction-factor"), "1");
    EXPECT_FALSE(registry.create({"QuotingMM", {{"symbol", kSymbol}, {"correction-factor", "0"}}}, {}).ok());
    EXPECT_FALSE(registry.create({"QuotingMM", {{"symbol", "SIMGAME-123-YES"}, {"correction-factor", "1"}}}, {}).ok());
    auto result = registry.create({"QuotingMM", {{"symbol", kSymbol}, {"correction-factor", "1.25"}}}, {});
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.values.at("withdraw-before"), "25");
    EXPECT_EQ(result.values.at("vol-lookback-minutes"), "20");
}

TEST(QuotingMmSchema, RefusesAnInstrumentTheReferenceVenueDoesNotPrice) {
    StrategyRegistry registry;
    register_quoting_mm(registry, {});  // no binance.conf mapping at all
    const auto result =
        registry.create({"QuotingMM", {{"symbol", kSymbol}, {"correction-factor", "1"}}}, {});
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("binance.conf"), std::string::npos) << result.error;
}

TEST(QuotingMmSchema, RefusesToBuildWhenTheVenueCannotBeReached) {
    StrategyRegistry registry;
    auto venue = venue_for_tests();
    venue.fetch = [](const cpm::HttpsRequest&) {
        return cpm::HttpsResponse{0, "", "connection refused"};
    };
    register_quoting_mm(registry, std::move(venue));
    const auto result =
        registry.create({"QuotingMM", {{"symbol", kSymbol}, {"correction-factor", "1"}}}, {});
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("connection refused"), std::string::npos) << result.error;
}

TEST(QuotingMmSchema, TakesItsSigmaAndOpeningFromTheWindowsKlines) {
    StrategyRegistry registry;
    register_quoting_mm(registry, venue_for_tests());
    const auto result =
        registry.create({"QuotingMM", {{"symbol", kSymbol}, {"correction-factor", "1"}}}, {});
    ASSERT_TRUE(result.ok()) << result.error;
    const auto status = result.strategy->status();
    // 12:15 window end, so the window opened at 12:00.
    EXPECT_EQ(status.diagnostics.at("Sigma source"),
              "20m of 1m klines to 2026-11-11 12:00:00Z");
    EXPECT_NE(status.diagnostics.at("Sigma / sqrt(second)"), "0.00000000");
}

TEST(QuotingMmHost, UsesTheExistingHostWithAnEmptyBookAndResetsWindowOverridesOnRoll) {
    Fixture venue;
    StrategyRegistry registry;
    register_quoting_mm(registry, venue_for_tests());
    StrategyHost::Feeds feeds;
    feeds.book = [](const std::string&) { return std::optional<InstrumentView>{empty_book()}; };
    feeds.reference = [&](const std::string&) { return reference(venue.now); };
    std::ostringstream log;
    cpm::Logger logger(log);
    StrategyHost host({}, registry, venue.hooks, feeds, logger);
    std::string id;
    ASSERT_TRUE(host.create({"QuotingMM", {{"symbol", kSymbol}, {"correction-factor", "1"}}},
                            id, "BTCD15M").empty());
    ASSERT_TRUE(host.start(id).empty());
    host.tick(venue.now);
    EXPECT_TRUE(venue.sent.empty());  // starts disarmed
    ASSERT_TRUE(host.arm(id).empty());
    host.tick(venue.now);
    EXPECT_EQ(venue.sent.size(), 2u);
    EXPECT_FALSE(host.list().front().requires_book_updates);
    ASSERT_TRUE(host.roll(id, "BTCD15M-BTC26NOV111230-0").empty());
    EXPECT_TRUE(host.list().front().armed);
    // The rolled instance prices from its own window's warmup, fetched as
    // it was built: a roll must not impose a 20-minute delay before the new
    // window can be quoted.
    venue.now += 15min;
    host.tick(venue.now);
    EXPECT_EQ(venue.sent.size(), 4u);
    EXPECT_EQ(host.list().front().status.diagnostics.at("Sigma source"),
              "20m of 1m klines to 2026-11-11 12:15:00Z");
}
}  // namespace
