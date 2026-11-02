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
#include <string>

namespace {

using cpm::LogFiles;
using cpm::Logger;
using cpm::LogSource;
using cpm::test::contents;
using cpm::test::TempDir;

TEST(LogFilesTest, SplitsTheWireOffFromTheNarrative) {
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
    EXPECT_NE(narrative.find("quoting 0.510 / 0.530"), std::string::npos)
        << "a strategy's own commentary belongs in the log, panel or not";
    EXPECT_EQ(narrative.find("8=FIXT.1.1"), std::string::npos)
        << "the wire is what this split exists to keep out";

    EXPECT_NE(contents(files.wire_path()).find("8=FIXT.1.1"), std::string::npos);
    // stdout still has all three: the files are a copy of it, not a move.
    const std::string printed = console.str();
    EXPECT_NE(printed.find("8=FIXT.1.1"), std::string::npos);
    EXPECT_EQ(std::count(printed.begin(), printed.end(), '\n'), 3);
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
