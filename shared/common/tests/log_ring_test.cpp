// The tail of the log the operator's page shows (GUI-Q27). A daemon meant to
// stay up for days must not grow a log in memory, and a page that has been
// watching all along must not be sent the same line twice.
#include "cpm/common/log_ring.hpp"

#include "cpm/common/logging.hpp"

#include <gtest/gtest.h>

#include <sstream>
#include <string>

namespace {

using cpm::LogLevel;
using cpm::LogRing;

TEST(LogRing, KeepsTheNewestLinesAndDropsTheRest) {
    LogRing ring(3);
    for (int i = 1; i <= 5; ++i) ring.add(LogLevel::Info, "stamp", "line " + std::to_string(i));

    const auto all = ring.since(0);
    ASSERT_EQ(all.size(), 3u);
    EXPECT_EQ(all.front().text, "line 3");
    EXPECT_EQ(all.back().text, "line 5");
    // The sequence keeps counting past what was evicted, so a page that was
    // sent line 2 is not sent line 3 again under the same number.
    EXPECT_EQ(all.back().seq, 5u);
    EXPECT_EQ(ring.last_seq(), 5u);
}

TEST(LogRing, SendsOnlyWhatAWatcherHasNotSeen) {
    LogRing ring(10);
    ring.add(LogLevel::Warn, "t1", "the feed went quiet");
    ring.add(LogLevel::Error, "t2", "order entry rejected");

    const auto first = ring.since(0);
    ASSERT_EQ(first.size(), 2u);
    EXPECT_EQ(first[0].level, LogLevel::Warn);
    EXPECT_EQ(first[1].level, LogLevel::Error);

    EXPECT_TRUE(ring.since(first.back().seq).empty());
    ring.add(LogLevel::Info, "t3", "resolved BTCD15M");
    const auto next = ring.since(first.back().seq);
    ASSERT_EQ(next.size(), 1u);
    EXPECT_EQ(next[0].text, "resolved BTCD15M");
}

TEST(LogRing, AnswersWithTheEndOfWhatWasMissedNotTheStart) {
    LogRing ring(100);
    for (int i = 1; i <= 50; ++i) ring.add(LogLevel::Info, "t", "line " + std::to_string(i));
    // A page gone long enough to miss more than one event's worth is better
    // served the last of what it missed than the first of it.
    const auto some = ring.since(0, 5);
    ASSERT_EQ(some.size(), 5u);
    EXPECT_EQ(some.front().text, "line 46");
    EXPECT_EQ(some.back().text, "line 50");
}

TEST(LogRing, TakesEveryLineTheLoggerWrites) {
    std::ostringstream out;
    cpm::Logger logger(out, LogLevel::Info);
    LogRing ring(10);
    logger.also_to([&ring](LogLevel level, cpm::LogSource, std::string_view stamp,
                           std::string_view message) { ring.add(level, stamp, message); });

    logger.debug("not this one");  // below the logger's level: neither sees it
    logger.info("listening on 127.0.0.1:8080");
    logger.warn("REST is not configured");

    const auto lines = ring.since(0);
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[0].text, "listening on 127.0.0.1:8080");
    EXPECT_EQ(lines[1].level, LogLevel::Warn);
    // The stamp is the one the stream got, so the panel and the file agree.
    EXPECT_NE(out.str().find(lines[0].stamp), std::string::npos);
}

// What cpm_trader installs: the daemon's own events, and anything that went
// wrong wherever it came from. The rest is in the file and not on the page.
TEST(LogRing, TheDaemonsPanelLeavesOutTheWireAndTheQuotingChatter) {
    std::ostringstream out;
    cpm::Logger logger(out, LogLevel::Info);
    LogRing ring(10);
    logger.also_to([&ring](LogLevel level, cpm::LogSource source, std::string_view stamp,
                           std::string_view message) {
        if (source != cpm::LogSource::Daemon && level < LogLevel::Warn) return;
        ring.add(level, stamp, message);
    });

    logger.info("created cryptomm-1 (CryptoMM) on BTCD15M-BTC26OCT290430-0");
    logger.info("<< 8=FIXT.1.1|35=X|...", cpm::LogSource::Wire);
    logger.info("quoting buy 1 @0.62 as mm-f", cpm::LogSource::Strategy);
    logger.info("market 0.61 / 0.67, mid 0.64 -> quoting 0.62 / 0.66",
                cpm::LogSource::Strategy);
    // A strategy that cannot quote is news, so it is kept whatever its
    // source: the filter is about chatter, not about where a line came from.
    logger.warn("could not quote buy: order entry is not available",
                cpm::LogSource::Strategy);
    logger.error("HALTED: the feed went quiet");

    const auto lines = ring.since(0);
    ASSERT_EQ(lines.size(), 3u);
    EXPECT_EQ(lines[0].text, "created cryptomm-1 (CryptoMM) on BTCD15M-BTC26OCT290430-0");
    EXPECT_EQ(lines[1].text, "could not quote buy: order entry is not available");
    EXPECT_EQ(lines[2].text, "HALTED: the feed went quiet");
    // The file still has all six.
    EXPECT_NE(out.str().find("8=FIXT"), std::string::npos);
    EXPECT_NE(out.str().find("quoting buy 1 @0.62"), std::string::npos);
}

}  // namespace
