// The file a day's logging lands in, and the rollover at midnight UTC. The
// stamp comes from the caller, so the rollover is driven here rather than
// waited for.
#include "cpm/common/daily_file.hpp"

#include "temp_dir.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

using cpm::DailyFile;
using cpm::test::contents;
using cpm::test::TempDir;

TEST(DailyFileTest, PutsTheFileInADirectoryNamedForTheDay) {
    const TempDir dir("cpm-daily-name-");
    DailyFile file(dir("logs"), "cpm-trader", "log");

    const auto opened = file.for_stamp("2026-11-02T05:41:10.414Z");
    ASSERT_NE(opened.out, nullptr) << file.error();
    EXPECT_TRUE(opened.fresh) << "a new file has to say so, or a CSV loses its header";
    EXPECT_EQ(file.path(), dir("logs") + "/20261102/cpm-trader.log");

    *opened.out << "a line\n";
    opened.out->flush();
    EXPECT_EQ(contents(file.path()), "a line\n");
}

TEST(DailyFileTest, StaysOnOneFileAllDayAndRollsOverAtMidnight) {
    const TempDir dir("cpm-daily-roll-");
    DailyFile file(dir(), "wire", "log");

    const auto morning = file.for_stamp("2026-11-02T00:00:00.000Z");
    ASSERT_NE(morning.out, nullptr) << file.error();
    const auto evening = file.for_stamp("2026-11-02T23:59:59.999Z");
    EXPECT_EQ(evening.out, morning.out) << "one file for the whole day";
    EXPECT_FALSE(evening.fresh) << "only the call that created it is fresh";

    const auto tomorrow = file.for_stamp("2026-11-03T00:00:00.001Z");
    ASSERT_NE(tomorrow.out, nullptr) << file.error();
    EXPECT_TRUE(tomorrow.fresh);
    EXPECT_EQ(file.path(), dir() + "/20261103/wire.log");
    EXPECT_TRUE(std::filesystem::exists(dir() + "/20261102/wire.log"))
        << "yesterday's directory is left where it is";
}

TEST(DailyFileTest, AppendsToTheDaysFileAcrossARestart) {
    const TempDir dir("cpm-daily-restart-");
    {
        DailyFile before(dir(), "cpm-trader", "log");
        auto opened = before.for_stamp("2026-11-02T09:00:00.000Z");
        ASSERT_NE(opened.out, nullptr) << before.error();
        *opened.out << "before the restart\n";
    }
    DailyFile after(dir(), "cpm-trader", "log");
    const auto opened = after.for_stamp("2026-11-02T09:30:00.000Z");
    ASSERT_NE(opened.out, nullptr) << after.error();
    EXPECT_FALSE(opened.fresh) << "the restart must not plant a second CSV header";
    *opened.out << "after it\n";
    opened.out->flush();
    EXPECT_EQ(contents(after.path()), "before the restart\nafter it\n");
}

TEST(DailyFileTest, SaysWhyItCannotWriteAndDoesNotRetryUntilTheDayTurns) {
    const TempDir dir("cpm-daily-blocked-");
    // A plain file where the directory should be: nothing under it can open.
    std::filesystem::create_directories(dir());
    std::ofstream(dir("not-a-directory")) << "in the way\n";

    DailyFile file(dir("not-a-directory"), "cpm-trader", "log");
    EXPECT_EQ(file.for_stamp("2026-11-02T05:00:00.000Z").out, nullptr);
    EXPECT_FALSE(file.error().empty()) << "a daemon that cannot log must be able to say so";

    // Same day again: still nothing, and no second attempt at the open.
    EXPECT_EQ(file.for_stamp("2026-11-02T06:00:00.000Z").out, nullptr);
    // A new day tries again, because the reason may have been fixed since.
    EXPECT_EQ(file.for_stamp("2026-11-03T00:00:00.000Z").out, nullptr);
    EXPECT_FALSE(file.error().empty());
}

TEST(DailyFileTest, WritesBesideTheDaemonWhenNoDirectoryIsGiven) {
    DailyFile file("", "cpm-daily-here", "log");
    const auto opened = file.for_stamp("2026-11-02T05:00:00.000Z");
    ASSERT_NE(opened.out, nullptr) << file.error();
    EXPECT_EQ(file.path(), "20261102/cpm-daily-here.log");
    std::filesystem::remove_all("20261102");
}

TEST(DailyFileTest, KeepsTheDaysFilesTogetherInOneDirectory) {
    const TempDir dir("cpm-daily-together-");
    DailyFile narrative(dir("logs"), "cpm-trader", "log");
    DailyFile fills(dir("logs"), "fills", "csv");
    const std::string stamp = "2026-11-02T09:00:00.000Z";
    ASSERT_NE(narrative.for_stamp(stamp).out, nullptr) << narrative.error();
    ASSERT_NE(fills.for_stamp(stamp).out, nullptr) << fills.error();

    // Everything written for one day is one directory to open or archive.
    EXPECT_EQ(std::filesystem::path(narrative.path()).parent_path(),
              std::filesystem::path(fills.path()).parent_path());
    EXPECT_EQ(std::filesystem::path(narrative.path()).filename(), "cpm-trader.log");
    EXPECT_EQ(std::filesystem::path(fills.path()).filename(), "fills.csv");
}

}  // namespace
