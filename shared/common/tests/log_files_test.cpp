// The daemon's log on disk: the narrative in one file, the wire in another,
// both a copy of what stdout already gets.
#include "cpm/common/log_files.hpp"

#include "cpm/common/log_ring.hpp"
#include "cpm/common/logging.hpp"
#include "temp_dir.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <string>

namespace {

using cpm::LogFiles;
using cpm::Logger;
using cpm::LogSource;
using cpm::test::contents;
using cpm::test::TempDir;

// Split by source: the narrative is what an operator opens after something
// went wrong, and the two things that would bury it - the wire and a market
// maker's commentary on its own quoting - each get their own file. This
// moved the strategy lines out of the narrative file, where they used to be
// (only the panel excluded them): with two series quoting they are thirty
// lines a second, and the ten lines that matter were in among them.
TEST(LogFilesTest, SplitsTheWireAndTheStrategiesOffFromTheNarrative) {
    const TempDir dir("cpm-logfiles-split-");
    LogFiles files(dir("logs"));
    EXPECT_EQ(files.open_now("2026-11-02T12:00:00.000Z"), "");

    std::ostringstream console;
    Logger logger(console);
    logger.also_to(files.sink());

    logger.info("session logged on");
    logger.info(">> 8=FIXT.1.1|35=D|", LogSource::Wire);
    logger.info("quoting 0.510 / 0.530", LogSource::Strategy);

    const std::string narrative = contents(files.narrative_path());
    EXPECT_NE(narrative.find("session logged on"), std::string::npos);
    EXPECT_EQ(narrative.find("quoting 0.510 / 0.530"), std::string::npos)
        << "the quoting commentary is what this split exists to keep out";
    EXPECT_EQ(narrative.find("8=FIXT.1.1"), std::string::npos);

    EXPECT_NE(contents(files.strategy_path()).find("quoting 0.510 / 0.530"), std::string::npos);
    EXPECT_NE(contents(files.wire_path()).find("8=FIXT.1.1"), std::string::npos);
    // Each file is one timeline, so `sort -m` puts any two back together.
    EXPECT_NE(files.strategy_path(), files.narrative_path());

    // stdout still has all three: the files are a copy of it, not a move.
    const std::string printed = console.str();
    EXPECT_NE(printed.find("8=FIXT.1.1"), std::string::npos);
    EXPECT_NE(printed.find("quoting 0.510 / 0.530"), std::string::npos);
    EXPECT_EQ(std::count(printed.begin(), printed.end(), '\n'), 3);
}

// A problem is never only in the file nobody opens: a Warn or an Error from
// a strategy is written to the narrative as well, exactly as the wire's are.
TEST(LogFilesTest, AStrategysWarningReachesTheNarrativeToo) {
    const TempDir dir("cpm-logfiles-warn-");
    LogFiles files(dir("logs"));
    ASSERT_EQ(files.open_now("2026-11-02T12:00:00.000Z"), "");
    std::ostringstream console;
    Logger logger(console);
    logger.also_to(files.sink());

    logger.info("quoting 0.510 / 0.530", LogSource::Strategy);
    logger.warn("max inventory reached: net 5 of 5", LogSource::Strategy);

    const std::string narrative = contents(files.narrative_path());
    EXPECT_EQ(narrative.find("quoting 0.510"), std::string::npos);
    EXPECT_NE(narrative.find("max inventory reached"), std::string::npos);
    // And it is in the strategy's own file as well, which stays complete.
    const std::string strategy = contents(files.strategy_path());
    EXPECT_NE(strategy.find("quoting 0.510"), std::string::npos);
    EXPECT_NE(strategy.find("max inventory reached"), std::string::npos);
}

// One run's whole account in one directory, under the day it ran.
TEST(LogFilesTest, ARunGetsItsOwnDirectoryUnderTheDay) {
    const TempDir dir("cpm-logfiles-run-");
    LogFiles files(dir("logs"), "080000");
    ASSERT_EQ(files.open_now("2026-11-04T08:00:00.000Z"), "");

    EXPECT_NE(files.narrative_path().find("/20261104/080000/cpm-trader.log"), std::string::npos)
        << files.narrative_path();
    EXPECT_NE(files.strategy_path().find("/20261104/080000/strategy.log"), std::string::npos);
    EXPECT_NE(files.wire_path().find("/20261104/080000/wire.log"), std::string::npos);

    // A second run of the same day is a second directory, not more lines in
    // the first: six starts appended into one file is what this is for.
    LogFiles second(dir("logs"), "081500");
    ASSERT_EQ(second.open_now("2026-11-04T08:15:00.000Z"), "");
    EXPECT_NE(second.narrative_path(), files.narrative_path());

    // latest points at the run going on now, relatively, so moving the tree
    // keeps it meaningful.
    EXPECT_EQ(cpm::point_latest_at(dir("logs"), "2026-11-04T08:15:00.000Z", "081500"), "");
    const auto link = std::filesystem::path(dir("logs")) / "latest";
    ASSERT_TRUE(std::filesystem::is_symlink(link));
    EXPECT_EQ(std::filesystem::read_symlink(link), std::filesystem::path("20261104/081500"));
    EXPECT_TRUE(std::filesystem::is_directory(std::filesystem::path(dir("logs")) / "latest"));
}

TEST(LogFilesTest, WritesTheSameLineTheConsoleGot) {
    const TempDir dir("cpm-logfiles-line-");
    LogFiles files(dir());
    std::ostringstream console;
    Logger logger(console);
    logger.also_to(files.sink());

    logger.warn("the feed went quiet");
    EXPECT_EQ(contents(files.narrative_path()), console.str());
}

TEST(LogFilesTest, PutsAProblemOnTheWireInBothFiles) {
    const TempDir dir("cpm-logfiles-problem-");
    LogFiles files(dir());
    std::ostringstream console;
    Logger logger(console);
    logger.also_to(files.sink());

    logger.error("order rejected: price not on the grid", LogSource::Wire);
    // The wire file stays a complete record of the traffic, and the file an
    // operator opens after something went wrong does not miss it either.
    EXPECT_NE(contents(files.wire_path()).find("order rejected"), std::string::npos);
    EXPECT_NE(contents(files.narrative_path()).find("order rejected"), std::string::npos);
}

TEST(LogFilesTest, SaysAtStartupWhenItCannotWrite) {
    const TempDir dir("cpm-logfiles-blocked-");
    std::filesystem::create_directories(dir());
    std::ofstream(dir("in-the-way")) << "a file, not a directory\n";

    LogFiles files(dir("in-the-way"));
    EXPECT_FALSE(files.open_now("2026-11-02T12:00:00.000Z").empty());

    // And the daemon still runs: logging carries on to stdout alone.
    std::ostringstream console;
    Logger logger(console);
    logger.also_to(files.sink());
    logger.info("still trading");
    EXPECT_NE(console.str().find("still trading"), std::string::npos);
}

TEST(LogFilesTest, LeavesTheLogRingWithEverythingItHad) {
    // Two sinks now share one Logger: the page's tail and the files. Neither
    // may cost the other a line.
    const TempDir dir("cpm-logfiles-ring-");
    LogFiles files(dir());
    cpm::LogRing tail(10);

    std::ostringstream console;
    Logger logger(console);
    logger.also_to(files.sink());
    logger.also_to([&tail](cpm::LogLevel level, LogSource, std::string_view stamp,
                           std::string_view message) { tail.add(level, stamp, message); });

    logger.info("created BTCD15M");
    logger.warn("the feed went quiet");

    const auto lines = tail.since(0);
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines.front().text, "created BTCD15M");
    EXPECT_NE(contents(files.narrative_path()).find("the feed went quiet"), std::string::npos);
}

}  // namespace
