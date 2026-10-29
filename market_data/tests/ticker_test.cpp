// The instrument name, taken apart. Every ticker here is one that actually
// appeared on the sandbox, copied out of the captures in docs/spec/ - a parser
// tested against names a simulator made up would only be testing itself.
#include "cpm/md/ticker.hpp"

#include <gtest/gtest.h>

namespace {

using cpm::md::parse_ticker;

// The window end as "YYYY-MM-DD HH:MM" UTC, for readable expectations.
std::string ends_at(const cpm::md::Clock::time_point& when) {
    const auto seconds = cpm::md::Clock::to_time_t(when);
    std::tm utc{};
    gmtime_r(&seconds, &utc);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &utc);
    return buffer;
}

TEST(Ticker, ReadsARollingWindowFromTheCaptures) {
    // docs/spec/capture-sandbox-20261026T0607-roll.log, the window that closed
    // at 06:15 and the one that replaced it.
    const auto closing = parse_ticker("BTCD15M-BTC26OCT260615-0");
    ASSERT_TRUE(closing.has_value());
    EXPECT_EQ(closing->series, "BTCD15M");
    EXPECT_EQ(closing->asset, "BTC");
    EXPECT_EQ(closing->suffix, "0");
    EXPECT_EQ(ends_at(closing->window_end), "2026-10-26 06:15");

    const auto opening = parse_ticker("BTCD15M-BTC26OCT260630-0");
    ASSERT_TRUE(opening.has_value());
    EXPECT_EQ(ends_at(opening->window_end), "2026-10-26 06:30");
    // Fifteen minutes apart, which is what makes the series a D15M.
    EXPECT_EQ(opening->window_end - closing->window_end, std::chrono::minutes(15));
}

TEST(Ticker, ReadsTheOtherSeriesAndAssetLengths) {
    // [MD] O6: 9 assets x {D5M, D15M}, and the asset is the part whose length
    // varies - BTC, DOGE, HYPE.
    const auto eth = parse_ticker("ETHD15M-ETH26OCT260630-0");
    ASSERT_TRUE(eth.has_value());
    EXPECT_EQ(eth->series, "ETHD15M");
    EXPECT_EQ(eth->asset, "ETH");

    const auto doge = parse_ticker("DOGED5M-DOGE26OCT231120-0");
    ASSERT_TRUE(doge.has_value());
    EXPECT_EQ(doge->series, "DOGED5M");
    EXPECT_EQ(doge->asset, "DOGE");
    EXPECT_EQ(ends_at(doge->window_end), "2026-10-23 11:20");

    const auto hype = parse_ticker("HYPED5M-HYPE26OCT231120-0");
    ASSERT_TRUE(hype.has_value());
    EXPECT_EQ(hype->asset, "HYPE");

    // [MD] O2's example, the first instrument the project ever looked at.
    const auto first = parse_ticker("BTCD15M-BTC26OCT230830-0");
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(ends_at(first->window_end), "2026-10-23 08:30");
}

TEST(Ticker, RefusesNamesThatCarryNoWindow) {
    // A sports market is a real instrument with no scheduled end in its name:
    // the suffix is a team ([MD] O6). Saying "no end" is the right answer, not
    // an error.
    EXPECT_FALSE(parse_ticker("NFLGAME-26OCT25LVLAR-LV").has_value());
    EXPECT_FALSE(parse_ticker("NCAAFGAME-26OCT31STETDAY-STET").has_value());
    // The simulators' own names, which is what keeps --withdraw-before inert
    // against md_sim rather than acting on a time it invented.
    EXPECT_FALSE(parse_ticker("SIMGAME-26NOV01HOMAWY-HOM").has_value());
    EXPECT_FALSE(parse_ticker("SIMOE-26NOV01HOMAWY-HOM").has_value());
}

TEST(Ticker, RefusesWhatIsNotATickerAtAll) {
    const char* bad[] = {
        "",
        "BTCD15M",                      // no dashes
        "BTCD15M-BTC26OCT260630",       // no suffix
        "BTCD15M-BTC26OCT260630-0-1",   // too many parts
        "BTCD15M--0",                   // nothing in the middle
        "BTCD15M-26OCT260630-0",        // no asset
        "BTCD15M-BTC26XXX260630-0",     // not a month
        "BTCD15M-BTC26OCT262530-0",     // hour 25
        "BTCD15M-BTC26OCT260670-0",     // minute 70
        "BTCD15M-BTC26OCT320630-0",     // day 32
        "BTCD15M-BTCxxOCT260630-0",     // year not digits
    };
    for (const char* symbol : bad) {
        EXPECT_FALSE(parse_ticker(symbol).has_value()) << "accepted: " << symbol;
    }
}

TEST(Ticker, TheEndIsUtcWhateverTheMachineThinks) {
    // The name is the venue's UTC, and the daemon's timezone is its own
    // business: timegm, not mktime.
    const char* was = ::getenv("TZ");
    ::setenv("TZ", "Asia/Kolkata", 1);  // +05:30, so an off-by-a-zone shows up
    ::tzset();
    const auto here = parse_ticker("BTCD15M-BTC26OCT260630-0");
    if (was != nullptr) {
        ::setenv("TZ", was, 1);
    } else {
        ::unsetenv("TZ");
    }
    ::tzset();
    ASSERT_TRUE(here.has_value());
    EXPECT_EQ(ends_at(here->window_end), "2026-10-26 06:30");
}

}  // namespace
