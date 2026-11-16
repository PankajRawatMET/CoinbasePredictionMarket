#include "cpm/strategy/quoting_warmup.hpp"

#include "cpm/common/fixed.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

namespace cpm::strategy {
namespace {

using namespace std::chrono_literals;

// 2026-11-16 06:00:00Z, a 15-minute window's opening.
constexpr std::int64_t kWindowMs = 1794808800000;
constexpr std::int64_t kPrice = 10000000000;  // 100.00000000

KlineClock::time_point at_ms(std::int64_t ms) {
    return KlineClock::time_point(std::chrono::milliseconds(ms));
}

WarmupRequest request(int lookback = 20) {
    WarmupRequest r;
    r.symbol = "BTCUSDT";
    r.window_start = at_ms(kWindowMs);
    r.lookback_minutes = lookback;
    return r;
}

// The bars a 20-minute lookback needs: 21 closes before the window, then the
// window's own bar. Closes alternate between kPrice and kPrice * 1.001, so
// every return is +/- log(1.001) and the sample deviation is known exactly.
std::vector<Kline> bars(int lookback = 20, std::int64_t window_ms = kWindowMs) {
    std::vector<Kline> out;
    const int count = lookback + 1;  // closes before the window
    for (int i = count; i >= 1; --i) {
        const std::int64_t open_ms = window_ms - static_cast<std::int64_t>(i) * 60000;
        const std::int64_t close = (i % 2 == 0) ? kPrice : kPrice + kPrice / 1000;
        Kline bar;
        bar.open_time = at_ms(open_ms);
        bar.close_time = at_ms(open_ms + 59999);
        bar.open = close;
        bar.high = close;
        bar.low = close;
        bar.close = close;
        bar.closed = true;
        out.push_back(bar);
    }
    Kline opening;
    opening.open_time = at_ms(window_ms);
    opening.close_time = at_ms(window_ms + 59999);
    opening.open = kPrice + 7;  // the opening price, and nothing else, matters
    opening.high = kPrice + 7;
    opening.low = kPrice + 7;
    opening.close = kPrice + 7;
    opening.closed = false;
    out.push_back(opening);
    return out;
}

double expected_sigma(int returns) {
    // Alternating returns of equal size: mean zero, so the sample deviation
    // is the return times sqrt(n / (n - 1)). Then per sqrt(second).
    const double r = std::log(1.001);
    return r * std::sqrt(static_cast<double>(returns) / (returns - 1)) / std::sqrt(60.0);
}

TEST(QuotingWarmup, TakesSigmaAndTheOpeningFromTheWindowsBars) {
    const auto warmed = warmup_from_bars(bars(), request(), at_ms(kWindowMs + 120000));
    ASSERT_TRUE(warmed.ok()) << warmed.error;
    EXPECT_EQ(warmed.warmup.opening, kPrice + 7);
    EXPECT_EQ(warmed.warmup.returns, 20);
    EXPECT_NEAR(warmed.warmup.sigma, expected_sigma(20), 1e-15);
    EXPECT_EQ(warmed.warmup.window_start, at_ms(kWindowMs));
    EXPECT_EQ(warmed.warmup.describe(), "20m of 1m klines to 2026-11-16 06:00:00Z");
}

TEST(QuotingWarmup, ReadsTheSameWindowWhateverTimeItIsAsked) {
    // Created at 06:00:02 and created at 06:12 must price identically: the
    // bars behind the window are closed and never change again.
    const auto early = warmup_from_bars(bars(), request(), at_ms(kWindowMs + 2000));
    const auto late = warmup_from_bars(bars(), request(), at_ms(kWindowMs + 12 * 60000));
    ASSERT_TRUE(early.ok() && late.ok());
    EXPECT_EQ(early.warmup.opening, late.warmup.opening);
    EXPECT_DOUBLE_EQ(early.warmup.sigma, late.warmup.sigma);
}

TEST(QuotingWarmup, HonoursTheLookback) {
    const auto warmed = warmup_from_bars(bars(40), request(40), at_ms(kWindowMs + 1000));
    ASSERT_TRUE(warmed.ok()) << warmed.error;
    EXPECT_EQ(warmed.warmup.returns, 40);
    EXPECT_NEAR(warmed.warmup.sigma, expected_sigma(40), 1e-15);
    EXPECT_EQ(warmed.warmup.describe(), "40m of 1m klines to 2026-11-16 06:00:00Z");
}

TEST(QuotingWarmup, RefusesAGapRatherThanStepOverIt) {
    // What a minute of venue outage actually looks like: `limit` counts back
    // from the window, so a missing minute is made up with an older bar and
    // the count still comes out right. Only the walk catches it.
    auto with_hole = bars();
    auto older = with_hole.front();
    older.open_time -= 1min;
    older.close_time -= 1min;
    with_hole.erase(with_hole.begin() + 5);
    with_hole.insert(with_hole.begin(), older);
    ASSERT_EQ(with_hole.size(), bars().size());

    const auto warmed = warmup_from_bars(with_hole, request(), at_ms(kWindowMs + 1000));
    EXPECT_FALSE(warmed.ok());
    EXPECT_NE(warmed.error.find("gap"), std::string::npos) << warmed.error;
}

TEST(QuotingWarmup, RefusesTooFewBars) {
    auto short_answer = bars();
    short_answer.erase(short_answer.begin());
    const auto warmed = warmup_from_bars(short_answer, request(), at_ms(kWindowMs + 1000));
    EXPECT_FALSE(warmed.ok());
    EXPECT_NE(warmed.error.find("21 bars where 22"), std::string::npos) << warmed.error;
}

TEST(QuotingWarmup, RefusesWithoutTheWindowsOwnBar) {
    // The moment the window opens, before the first trade prints: the venue
    // has no 06:00 bar yet, so there is no opening price to quote against.
    auto without = bars();
    without.pop_back();
    const auto warmed = warmup_from_bars(without, request(), at_ms(kWindowMs + 100));
    EXPECT_FALSE(warmed.ok());
    EXPECT_NE(warmed.error.find("06:00:00Z"), std::string::npos) << warmed.error;
}

TEST(QuotingWarmup, RefusesAHistoryBarThatHasNotClosed) {
    auto unclosed = bars();
    unclosed[unclosed.size() - 2].closed = false;
    const auto warmed = warmup_from_bars(unclosed, request(), at_ms(kWindowMs + 1000));
    EXPECT_FALSE(warmed.ok());
    EXPECT_NE(warmed.error.find("not closed"), std::string::npos)
        << warmed.error;  // worded "has not closed yet"
}

TEST(QuotingWarmup, RefusesAMarketThatDidNotMove) {
    auto flat = bars();
    for (auto& bar : flat) bar.close = kPrice;
    const auto warmed = warmup_from_bars(flat, request(), at_ms(kWindowMs + 1000));
    EXPECT_FALSE(warmed.ok());
    EXPECT_NE(warmed.error.find("no variance"), std::string::npos) << warmed.error;
}

TEST(QuotingWarmup, RefusesALookbackThatIsNotWholeBars) {
    EXPECT_FALSE(warmup_from_bars(bars(), request(1), at_ms(kWindowMs)).ok());
    WarmupRequest odd = request();
    odd.interval = "15m";
    odd.lookback_minutes = 20;  // not a whole number of 15m bars
    EXPECT_FALSE(warmup_from_bars(bars(), odd, at_ms(kWindowMs)).ok());
}

// --- the fetch, with no socket ----------------------------------------------

std::string json_bars(int lookback = 20, std::int64_t window_ms = kWindowMs,
                      bool with_window_bar = true) {
    const auto quoted = [](std::int64_t scaled) {
        return "\"" + cpm::format_scaled(scaled, kKlineScale) + "\"";
    };
    std::string out = "[";
    for (const auto& bar : bars(lookback, window_ms)) {
        if (!with_window_bar && bar.open_time == at_ms(window_ms)) continue;
        const auto open_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 bar.open_time.time_since_epoch())
                                 .count();
        if (out.size() > 1) out += ",";
        out += "[" + std::to_string(open_ms) + "," + quoted(bar.open) + "," + quoted(bar.high) +
               "," + quoted(bar.low) + "," + quoted(bar.close) + ",\"1\"," +
               std::to_string(open_ms + 59999) + ",\"1\",1,\"0\",\"0\",\"0\"]";
    }
    return out + "]";
}

TEST(QuotingWarmup, AsksForExactlyTheBarsItNeeds) {
    cpm::HttpsRequest seen;
    const auto warmed = warm_window(
        request(),
        [&](const cpm::HttpsRequest& http) {
            seen = http;
            return cpm::HttpsResponse{200, json_bars(), ""};
        },
        at_ms(kWindowMs + 60000));

    ASSERT_TRUE(warmed.ok()) << warmed.error;
    EXPECT_EQ(warmed.warmup.returns, 20);
    EXPECT_EQ(seen.path, "/fapi/v1/klines");
    // 21 closes plus the window's own bar, ending at the window's open.
    EXPECT_EQ(seen.query,
              "symbol=BTCUSDT&interval=1m&limit=22&endTime=" + std::to_string(kWindowMs));
}

TEST(QuotingWarmup, PassesTheVenuesRefusalThrough) {
    const auto warmed = warm_window(request(), [](const cpm::HttpsRequest&) {
        return cpm::HttpsResponse{0, "", "connection timed out"};
    });
    EXPECT_FALSE(warmed.ok());
    EXPECT_NE(warmed.error.find("connection timed out"), std::string::npos) << warmed.error;
}

// --- the cache --------------------------------------------------------------

TEST(QuotingWarmupCache, FetchesAWindowOnceHoweverOftenItIsAsked) {
    WarmupCache cache;
    int calls = 0;
    const HttpsFetch fetch = [&](const cpm::HttpsRequest&) {
        ++calls;
        return cpm::HttpsResponse{200, json_bars(), ""};
    };
    for (int i = 0; i < 50; ++i) {
        const auto warmed = cache.get(request(), fetch, at_ms(kWindowMs + 1000 * i));
        ASSERT_TRUE(warmed.ok()) << warmed.error;
    }
    EXPECT_EQ(calls, 1);
}

TEST(QuotingWarmupCache, DoesNotRetryAFailureOnEveryTick) {
    // The reason this class exists: a failed roll is retried on every tick of
    // the daemon loop, and a blocking HTTPS call there would stall it.
    WarmupCache cache(2s);
    int calls = 0;
    const HttpsFetch fetch = [&](const cpm::HttpsRequest&) {
        ++calls;
        return cpm::HttpsResponse{0, "", "connection refused"};
    };
    const auto start = at_ms(kWindowMs + 1000);
    for (int i = 0; i < 100; ++i) {
        const auto warmed = cache.get(request(), fetch, start + std::chrono::milliseconds(10 * i));
        EXPECT_FALSE(warmed.ok());
    }
    EXPECT_EQ(calls, 1);

    // ... but it does try again once the venue has had a moment.
    EXPECT_FALSE(cache.get(request(), fetch, start + 3s).ok());
    EXPECT_EQ(calls, 2);
}

TEST(QuotingWarmupCache, KeepsWindowsApart) {
    WarmupCache cache;
    int calls = 0;
    const HttpsFetch fetch = [&](const cpm::HttpsRequest& http) {
        ++calls;
        const auto next = http.query.find("endTime=" + std::to_string(kWindowMs + 900000)) !=
                          std::string::npos;
        return cpm::HttpsResponse{200, json_bars(20, next ? kWindowMs + 900000 : kWindowMs), ""};
    };
    auto first = request();
    auto second = request();
    second.window_start = at_ms(kWindowMs + 900000);  // the next 15-minute window

    ASSERT_TRUE(cache.get(first, fetch, at_ms(kWindowMs + 1000)).ok());
    ASSERT_TRUE(cache.get(second, fetch, at_ms(kWindowMs + 901000)).ok());
    ASSERT_TRUE(cache.get(first, fetch, at_ms(kWindowMs + 902000)).ok());
    EXPECT_EQ(calls, 2);
}

}  // namespace
}  // namespace cpm::strategy
