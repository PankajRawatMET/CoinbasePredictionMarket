#include "cpm/strategy/quoting_mm.hpp"
#include "cpm/strategy/host.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <sstream>
#include <cstdio>
#include <thread>

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

std::string utc(PricingSecond t) {
    const auto day = std::chrono::floor<std::chrono::days>(t);
    const std::chrono::year_month_day date{day};
    const std::chrono::hh_mm_ss time{t - day};
    char out[40];
    std::snprintf(out, sizeof(out), "%04d-%02u-%02uT%02d:%02d:%02dZ",
                  int(date.year()), unsigned(date.month()), unsigned(date.day()),
                  int(time.hours().count()), int(time.minutes().count()), int(time.seconds().count()));
    return out;
}

QuotingMmVenue venue_for_tests(PricingSecond as_of = std::chrono::floor<std::chrono::seconds>(
                                   cpm::md::parse_ticker(kSymbol)->window_end - 600s),
                             std::string reference_symbol = "BTCUSDT") {
    QuotingMmVenue venue;
    venue.symbols = {{"BTCD15M", {"BTCUSDT", 1000000}}};
    venue.history.host = "history";
    venue.fetch = [as_of, symbol = std::move(reference_symbol)](const cpm::HttpsRequest& http) {
        if (http.path == "/v1/midpoints") {
            const auto start = std::chrono::floor<std::chrono::minutes>(as_of);
            if (http.query != "symbol=" + symbol + "&start=" + utc(start) + "&end=" + utc(as_of))
                return cpm::HttpsResponse{400, "", "unexpected midpoint request"};
            std::ostringstream out;
            out << R"({"symbol":")" << symbol << R"(","start":")" << utc(start)
                << R"(","end_exclusive":")" << utc(as_of) << R"(","samples":[)";
            for (auto time = PricingSecond(start); time < as_of; time += 1s) {
                if (time != start) out << ',';
                out << R"({"time":")" << utc(time) << R"(","mid":100,"quote_age_seconds":0.5})";
            }
            out << "]}";
            return cpm::HttpsResponse{200, out.str(), ""};
        }
        const auto window = http.query.find("window_open=");
        if (http.path != "/v1/features" || window == std::string::npos ||
            !http.query.starts_with("symbol=" + symbol + "&as_of=" + utc(as_of)))
            return cpm::HttpsResponse{400, "", "unexpected request"};
        std::ostringstream out;
        out << R"({"symbol":")" << symbol
            << R"(","feature_version":"mid-boundary-v1","history_complete":true,)"
            << R"("max_quote_age_seconds":5,"fast_variance":0.00000004,"last_second_mid":100,)"
            << R"("as_of":")" << utc(as_of) << R"(","last_second_timestamp":")" << utc(as_of - 1s)
            << R"(","fast_variance_as_of":")" << utc(as_of - 1s)
            << R"(","reference":{"window_open":")" << http.query.substr(window + 12)
            << R"(","price":100,"valid":true,"valid_sample_count":60},"gk_minute_variances":[)";
        const auto minute = std::chrono::floor<std::chrono::minutes>(as_of);
        for (int i = 120; i > 0; --i) {
            if (i != 120) out << ',';
            out << R"({"variance":0.0000024,"valid_sample_count":60,"minute_start":")"
                << utc(minute - std::chrono::minutes(i)) << R"("})";
        }
        out << ']';
        if (as_of != minute) {
            out << R"(,"partial_minute":{"minute_start":")" << utc(minute)
                << R"(","open":100,"high":100,"low":100,"close":100,"valid_sample_count":)"
                << (as_of - minute).count() << '}';
        }
        out << '}';
        return cpm::HttpsResponse{200, out.str(), ""};
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

TEST(QuotingMmSchema, AcceptsLegacySettingsAndRejectsInvalidInputs) {
    StrategyRegistry registry;
    register_quoting_mm(registry, venue_for_tests());
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

TEST(QuotingMmHistory, RejectsAnIncompleteResponse) {
    const auto as_of = std::chrono::floor<std::chrono::seconds>(
        cpm::md::parse_ticker(kSymbol)->window_end - 600s);
    const auto result = fetch_pricing_history({}, [](const cpm::HttpsRequest&) {
        return cpm::HttpsResponse{200, R"({"history_complete":false})", ""};
    }, "BTCUSDT", as_of, as_of - 300s, as_of + 540s);
    EXPECT_NE(result.error.find("expected history_complete=true"), std::string::npos) << result.error;
}

TEST(QuotingMmHistory, SeedsVolatilityAndUsesSettlementAverageWithoutOffset) {
    const auto end = std::chrono::floor<std::chrono::seconds>(cpm::md::parse_ticker(kSymbol)->window_end);
    const auto as_of = end - 60s;
    const auto venue = venue_for_tests(as_of);
    auto result = fetch_pricing_history(venue.history, venue.fetch, "BTCUSDT", as_of, end - 900s, as_of);
    ASSERT_TRUE(result.error.empty()) << result.error;
    auto& state = result.state;
    EXPECT_NEAR(state.sigma(), 0.0002, 1e-12);
    const auto before = state.z(100, 61, -1, 0.95, 0.001);
    ASSERT_TRUE(before);
    EXPECT_NEAR(*before, 0.001 / (0.95 * 0.0002 * std::sqrt(21.0)), 1e-9);
    state.add(as_of, 101, as_of);
    const auto transition = state.z(100, 60, 0, 0.95, 0.001);
    ASSERT_TRUE(transition);
    EXPECT_DOUBLE_EQ(*transition, 0);
    const auto after = state.z(100, 59, 1, 0.95, 0.001);
    ASSERT_TRUE(after);
    EXPECT_GT(*after, 0);
    EXPECT_FALSE(state.z(100, 57, 3, 0.95, 0));
}

TEST(QuotingMmHistory, BackfillsElapsedSettlementSecondsForAnotherCoin) {
    const auto end = std::chrono::floor<std::chrono::seconds>(cpm::md::parse_ticker(kSymbol)->window_end);
    const auto as_of = end - 32s;
    const auto venue = venue_for_tests(as_of, "ETHUSDT");
    const auto result = fetch_pricing_history(venue.history, venue.fetch, "ETHUSDT", as_of,
                                              end - 900s, end - 60s);
    ASSERT_TRUE(result.error.empty()) << result.error;
    EXPECT_EQ(result.state.settlement_count, 28);
    EXPECT_DOUBLE_EQ(result.state.settlement_sum, 2800);
    EXPECT_EQ(result.state.minute_count, 28);
    const auto z = result.state.z(100, 32, 28, 0.95, 0.002);
    ASSERT_TRUE(z);
    EXPECT_DOUBLE_EQ(*z, 0);
}

TEST(QuotingMmCalibration, UsesTheMedianAfterTwoHundredDistinctSecondsAndExpiresIt) {
    PricingCalibration calibration;
    const auto start = std::chrono::floor<std::chrono::seconds>(cpm::md::parse_ticker(kSymbol)->window_end);
    for (int i = 0; i < 199; ++i) {
        calibration.update(start + std::chrono::seconds(i), 0.001, 0.001, student_t4_cdf(0.5));
    }
    EXPECT_DOUBLE_EQ(calibration.correction, 0.95);
    calibration.update(start + 198s, 0.001, 0.001, student_t4_cdf(0.5));
    EXPECT_EQ(calibration.corrections.size(), 199u);
    calibration.update(start + 199s, 0.001, 0.001, student_t4_cdf(0.5));
    EXPECT_NEAR(calibration.correction, 2, 1e-10);
    calibration.expire(start + 2h);
    EXPECT_DOUBLE_EQ(calibration.correction, 0.95);
}

TEST(QuotingMmHost, WithdrawsOnMissingLiveBoundaryAndBootstrapsAgainOnRecoveryAndRoll) {
    Fixture fixture;
    StrategyRegistry registry;
    const auto as_of = std::chrono::floor<std::chrono::seconds>(fixture.now);
    auto venue = venue_for_tests(as_of);
    venue.fetch = [as_of, first = venue.fetch,
                   rolled = venue_for_tests(as_of + 15min).fetch](const cpm::HttpsRequest& request) {
        if (request.query.find("&as_of=" + utc(as_of + 15min)) != std::string::npos) return rolled(request);
        return first(request);
    };
    register_quoting_mm(registry, std::move(venue));
    StrategyHost::Feeds feeds;
    feeds.book = [](const std::string& symbol) {
        auto book = empty_book();
        book.symbol = symbol;
        return std::optional<InstrumentView>{book};
    };
    bool has_history = true;
    feeds.reference = [&](const std::string&) {
        auto view = reference(fixture.now);
        if (!has_history) view.history.reset();
        return view;
    };
    std::ostringstream log;
    cpm::Logger logger(log);
    StrategyHost host({}, registry, fixture.hooks, feeds, logger);
    std::string id;
    ASSERT_TRUE(host.create({"QuotingMM", {{"symbol", kSymbol}}}, id, "BTCD15M").empty());
    ASSERT_TRUE(host.start(id).empty());
    host.tick(fixture.now);
    EXPECT_TRUE(fixture.sent.empty());
    ASSERT_TRUE(host.arm(id).empty());
    const auto tick_until_sent = [&](std::size_t count) {
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (fixture.sent.size() < count && std::chrono::steady_clock::now() < deadline) {
            host.tick(fixture.now);
            std::this_thread::sleep_for(1ms);
        }
    };
    tick_until_sent(2);
    ASSERT_EQ(fixture.sent.size(), 2u);
    EXPECT_FALSE(host.list().front().requires_book_updates);
    EXPECT_EQ(host.list().front().status.diagnostics.at("Sigma source"),
              "historical API and live midpoint samples");
    has_history = false;
    fixture.now += 1s;
    host.tick(fixture.now);
    ASSERT_EQ(fixture.cancels.size(), 2u);
    EXPECT_NE(host.list().front().status.reason.find("missing Binance midpoint"), std::string::npos);
    has_history = true;
    for (const auto& cancel : fixture.cancels) {
        ExecutionReport report;
        report.cl_ord_id = cancel.cl_ord_id;
        report.orig_cl_ord_id = cancel.orig_cl_ord_id;
        report.exec_type = ExecType::Canceled;
        host.on_report(report);
    }
    tick_until_sent(4);
    ASSERT_EQ(fixture.sent.size(), 4u);
    ASSERT_TRUE(host.roll(id, "BTCD15M-BTC26NOV111230-0").empty());
    fixture.now = as_of + 15min;
    tick_until_sent(6);
    EXPECT_EQ(fixture.sent.size(), 6u);
}

TEST(QuotingMmHistory, SeedsFromPublishedMinutesAndRequiresLiveCoverage) {
    const auto as_of = std::chrono::floor<std::chrono::seconds>(
        cpm::md::parse_ticker(kSymbol)->window_end - 600s);
    for (const bool publication_delayed : {false, true}) {
        SCOPED_TRACE(publication_delayed);
        auto params = parameters();
        params.history_symbol = "BTCUSDT";
        params.calibration = std::make_shared<PricingCalibration>();
        const auto published = venue_for_tests(as_of - (publication_delayed ? 1min : 0min));
        params.history_fetch = [as_of, publication_delayed, fetch = published.fetch](
                                   const cpm::HttpsRequest& request) {
            if (publication_delayed && request.query.find("&as_of=" + utc(as_of)) != std::string::npos)
                return cpm::HttpsResponse{200, R"({"history_complete":false})", ""};
            return fetch(request);
        };
        Fixture fixture(params);
        fixture.now = as_of + 28s;
        fixture.strategy->on_reference(reference(fixture.now, kSpot, 10), fixture.now);
        fixture.strategy->on_market(fixture.book, fixture.position, fixture.now);
        EXPECT_TRUE(fixture.sent.empty());
        EXPECT_NE(fixture.strategy->status().reason.find("minute boundary"), std::string::npos);
        const auto deadline = std::chrono::steady_clock::now() + 1s;
        while (fixture.sent.empty() && std::chrono::steady_clock::now() < deadline) {
            fixture.pass();
            std::this_thread::sleep_for(1ms);
        }
        EXPECT_EQ(fixture.sent.size(), 2u) << fixture.strategy->status().reason;
        EXPECT_FALSE(fixture.last_model_line().empty());
    }
}
}  // namespace
