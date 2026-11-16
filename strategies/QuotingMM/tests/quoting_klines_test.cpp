#include "cpm/strategy/quoting_klines.hpp"

#include "cpm/common/fixed.hpp"

#include <gtest/gtest.h>

#include <string>

namespace cpm::strategy {
namespace {

KlineClock::time_point at_ms(std::int64_t ms) {
    return KlineClock::time_point(std::chrono::milliseconds(ms));
}

// Two consecutive BTCUSDT 1m bars in the venue's own shape: twelve fields,
// prices and sizes as strings, times as numbers.
const char* kTwoBars = R"([
  [1762300800000,"80162.60","80190.10","80150.00","80175.30","12.345",1762300859999,"989654.12",321,"6.100","489000.00","0"],
  [1762300860000,"80175.30","80210.00","80170.00","80205.00","9.100",1762300919999,"729000.00",210,"4.000","320000.00","0"]
])";

std::int64_t scaled(const char* text) { return *cpm::parse_scaled(text, kKlineScale); }

TEST(QuotingKlines, ParsesTheVenuesRows) {
    const auto series = parse_klines(kTwoBars, at_ms(1762300920000));
    ASSERT_TRUE(series.ok()) << series.error;
    ASSERT_EQ(series.bars.size(), 2u);

    const auto& first = series.bars[0];
    EXPECT_EQ(first.open_time, at_ms(1762300800000));
    EXPECT_EQ(first.close_time, at_ms(1762300859999));
    EXPECT_EQ(first.open, scaled("80162.60"));
    EXPECT_EQ(first.high, scaled("80190.10"));
    EXPECT_EQ(first.low, scaled("80150.00"));
    EXPECT_EQ(first.close, scaled("80175.30"));
    EXPECT_EQ(first.volume, scaled("12.345"));
    EXPECT_EQ(first.quote_volume, scaled("989654.12"));
    EXPECT_EQ(first.trades, 321);
    EXPECT_TRUE(first.closed);
    EXPECT_TRUE(series.bars[1].closed);
}

TEST(QuotingKlines, TheNewestBarIsStillForming) {
    // Halfway through the second bar: it has opened, it has not closed.
    const auto series = parse_klines(kTwoBars, at_ms(1762300890000));
    ASSERT_TRUE(series.ok()) << series.error;
    ASSERT_EQ(series.bars.size(), 2u);
    EXPECT_TRUE(series.bars[0].closed);
    EXPECT_FALSE(series.bars[1].closed);
}

TEST(QuotingKlines, RefusesAnAnswerItCannotRead) {
    EXPECT_FALSE(parse_klines("not json", at_ms(0)).ok());
    EXPECT_FALSE(parse_klines(R"({"code":-1121,"msg":"Invalid symbol."})", at_ms(0)).ok());
    // A row the venue would never send: too few fields.
    EXPECT_FALSE(parse_klines(R"([[1,"1","1","1","1","1",2]])", at_ms(0)).ok());
    // A price as a JSON number rather than a string. Accepting it would put
    // the price through a double on the way in.
    EXPECT_FALSE(
        parse_klines(R"([[1,80162.6,"1","1","1","1",2,"1",0,"0","0","0"]])", at_ms(0)).ok());
}

TEST(QuotingKlines, RefusesABarThatIsNotOne) {
    // high below close.
    const char* inverted =
        R"([[1762300800000,"80162.60","80100.00","80150.00","80175.30","1",1762300859999,"1",1,"0","0","0"]])";
    EXPECT_FALSE(parse_klines(inverted, at_ms(0)).ok());
    // Out of order, which would silently corrupt any returns taken from it.
    const char* backwards =
        R"([[1762300860000,"1","1","1","1","1",1762300919999,"1",1,"0","0","0"],
            [1762300800000,"1","1","1","1","1",1762300859999,"1",1,"0","0","0"]])";
    EXPECT_FALSE(parse_klines(backwards, at_ms(0)).ok());
}

TEST(QuotingKlines, KnowsTheVenuesIntervals) {
    EXPECT_EQ(kline_interval_minutes("1m"), 1);
    EXPECT_EQ(kline_interval_minutes("15m"), 15);
    EXPECT_EQ(kline_interval_minutes("1h"), 60);
    EXPECT_EQ(kline_interval_minutes("12h"), 720);
    EXPECT_EQ(kline_interval_minutes("1d"), 1440);
    EXPECT_EQ(kline_interval_minutes("1w"), 10080);
    // A month, which is not a minute and is not supported.
    EXPECT_EQ(kline_interval_minutes("1M"), 0);
    // Spellings the venue does not serve.
    EXPECT_EQ(kline_interval_minutes("7m"), 0);
    EXPECT_EQ(kline_interval_minutes("2m"), 0);
    EXPECT_EQ(kline_interval_minutes(""), 0);
    EXPECT_EQ(kline_interval_minutes("m"), 0);
    EXPECT_EQ(kline_interval_minutes("1"), 0);
}

TEST(QuotingKlines, ChargesTheDocumentedWeight) {
    EXPECT_EQ(kline_request_weight(1), 1);
    EXPECT_EQ(kline_request_weight(100), 1);
    EXPECT_EQ(kline_request_weight(101), 2);
    EXPECT_EQ(kline_request_weight(500), 2);
    EXPECT_EQ(kline_request_weight(501), 5);
    EXPECT_EQ(kline_request_weight(1000), 5);
    EXPECT_EQ(kline_request_weight(1001), 10);
}

TEST(QuotingKlines, TurnsMinutesIntoBars) {
    KlineRequest request;
    request.symbol = "BTCUSDT";
    request.minutes = 60;
    std::string error;
    // One spare for the bar that is still forming.
    EXPECT_EQ(kline_limit(request, error).value_or(0), 61) << error;

    // Dropping the forming bar does not shrink the ask: 60 minutes of
    // completed history is still 61 bars, 60 of which have closed.
    request.include_unclosed = false;
    EXPECT_EQ(kline_limit(request, error).value_or(0), 61) << error;

    // Rounded up: 90 minutes is two whole hours of 1h bars, not one.
    request.interval = "1h";
    request.minutes = 90;
    EXPECT_EQ(kline_limit(request, error).value_or(0), 3) << error;

    // An explicit count wins over the arithmetic.
    request.limit = 5;
    EXPECT_EQ(kline_limit(request, error).value_or(0), 5) << error;
}

TEST(QuotingKlines, RefusesWhatTheVenueCannotAnswer) {
    KlineRequest request;
    request.symbol = "BTCUSDT";
    std::string error;

    request.minutes = 2000;  // more 1m bars than one call returns
    EXPECT_FALSE(kline_limit(request, error).has_value());
    EXPECT_NE(error.find("1500"), std::string::npos) << error;

    request.minutes = 60;
    request.limit = 2000;
    EXPECT_FALSE(kline_limit(request, error).has_value());

    request.limit = 0;
    request.minutes = 0;
    EXPECT_FALSE(kline_limit(request, error).has_value());

    request.minutes = 60;
    request.interval = "2m";
    EXPECT_FALSE(kline_limit(request, error).has_value());
    EXPECT_NE(error.find("2m"), std::string::npos) << error;
}

TEST(QuotingKlines, BuildsTheQuery) {
    KlineRequest request;
    request.symbol = "BTCUSDT";
    request.minutes = 60;
    std::string error;
    EXPECT_EQ(kline_query(request, error), "symbol=BTCUSDT&interval=1m&limit=61");
    EXPECT_TRUE(error.empty());

    request.symbol = "";
    EXPECT_TRUE(kline_query(request, error).empty());
    EXPECT_FALSE(error.empty());

    // Nothing that would need escaping ever reaches the URL.
    request.symbol = "BTC/USDT";
    EXPECT_TRUE(kline_query(request, error).empty());
    EXPECT_NE(error.find("BTC/USDT"), std::string::npos) << error;
}

TEST(QuotingKlines, AsksTheVenueThePublicWay) {
    KlineRequest request;
    request.symbol = "ETHUSDT";
    request.interval = "5m";
    request.minutes = 60;

    cpm::HttpsRequest seen;
    const auto series = fetch_klines(
        request,
        [&](const cpm::HttpsRequest& http) {
            seen = http;
            return cpm::HttpsResponse{200, kTwoBars, ""};
        },
        at_ms(1762300920000));

    ASSERT_TRUE(series.ok()) << series.error;
    EXPECT_EQ(series.bars.size(), 2u);
    EXPECT_EQ(seen.host, "fapi.binance.com");
    EXPECT_EQ(seen.path, "/fapi/v1/klines");
    EXPECT_EQ(seen.query, "symbol=ETHUSDT&interval=5m&limit=13");
    EXPECT_EQ(series.weight, 1);
    // Public market data: one header, and no credential anywhere near it.
    ASSERT_EQ(seen.headers.size(), 1u);
    EXPECT_EQ(seen.headers[0].first, "Accept");
}

TEST(QuotingKlines, DropsTheFormingBarWhenAsked) {
    KlineRequest request;
    request.symbol = "BTCUSDT";
    request.include_unclosed = false;

    const auto series = fetch_klines(
        request, [&](const cpm::HttpsRequest&) { return cpm::HttpsResponse{200, kTwoBars, ""}; },
        at_ms(1762300890000));

    ASSERT_TRUE(series.ok()) << series.error;
    ASSERT_EQ(series.bars.size(), 1u);
    EXPECT_TRUE(series.bars[0].closed);
}

TEST(QuotingKlines, SaysWhyTheVenueSaidNo) {
    KlineRequest request;
    request.symbol = "NOTACOIN";

    const auto rejected = fetch_klines(request, [&](const cpm::HttpsRequest&) {
        return cpm::HttpsResponse{400, R"({"code":-1121,"msg":"Invalid symbol."})", ""};
    });
    EXPECT_FALSE(rejected.ok());
    EXPECT_NE(rejected.error.find("Invalid symbol."), std::string::npos) << rejected.error;
    EXPECT_NE(rejected.error.find("-1121"), std::string::npos) << rejected.error;

    const auto unreachable = fetch_klines(request, [&](const cpm::HttpsRequest&) {
        return cpm::HttpsResponse{0, "", "connection timed out"};
    });
    EXPECT_FALSE(unreachable.ok());
    EXPECT_NE(unreachable.error.find("fapi.binance.com"), std::string::npos) << unreachable.error;
    EXPECT_NE(unreachable.error.find("connection timed out"), std::string::npos)
        << unreachable.error;
}

TEST(QuotingKlines, RefusesABadRequestWithoutCallingOut) {
    KlineRequest request;
    request.symbol = "BTCUSDT";
    request.interval = "1M";  // a month

    bool called = false;
    const auto series = fetch_klines(request, [&](const cpm::HttpsRequest&) {
        called = true;
        return cpm::HttpsResponse{200, "[]", ""};
    });
    EXPECT_FALSE(series.ok());
    EXPECT_FALSE(called);
}

}  // namespace
}  // namespace cpm::strategy
