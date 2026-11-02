// The records half of the logging: fixed columns, one row per event, and a
// header that is written once however many times the daemon is restarted.
#include "cpm/common/csv_log.hpp"

#include "temp_dir.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using cpm::csv_field;
using cpm::csv_row;
using cpm::CsvLog;
using cpm::DailyFile;
using cpm::test::contents;
using cpm::test::TempDir;

TEST(CsvFieldTest, QuotesOnlyWhatHasToBeQuoted) {
    EXPECT_EQ(csv_field("BTCD15M-26NOV02-1245"), "BTCD15M-26NOV02-1245");
    EXPECT_EQ(csv_field("12.345"), "12.345");
    EXPECT_EQ(csv_field(""), "");
}

TEST(CsvFieldTest, EscapesCommasQuotesAndNewlines) {
    EXPECT_EQ(csv_field("rejected, see Text"), "\"rejected, see Text\"");
    EXPECT_EQ(csv_field("said \"no\""), "\"said \"\"no\"\"\"");
    EXPECT_EQ(csv_field("two\nlines"), "\"two\nlines\"");
    EXPECT_EQ(csv_row({"a", "b,c", "d"}), "a,\"b,c\",d");
}

TEST(CsvLogTest, WritesTheHeaderOnceAndAStampedRowPerRecord) {
    const TempDir dir("cpm-csv-rows-");
    DailyFile file(dir(), "fills", "csv");
    CsvLog log(file, "stamp,symbol,side,qty,price");

    log.row("2026-11-02T12:45:01.100Z", {"BTCD15M-26NOV02-1245", "buy", "10.00", "0.512"});
    log.row("2026-11-02T12:45:02.250Z", {"BTCD15M-26NOV02-1245", "sell", "4.00", "0.530"});

    EXPECT_EQ(contents(file.path()),
              "stamp,symbol,side,qty,price\n"
              "2026-11-02T12:45:01.100Z,BTCD15M-26NOV02-1245,buy,10.00,0.512\n"
              "2026-11-02T12:45:02.250Z,BTCD15M-26NOV02-1245,sell,4.00,0.530\n");
}

TEST(CsvLogTest, LeavesAnAbsentValueEmptyRatherThanZero) {
    const TempDir dir("cpm-csv-absent-");
    DailyFile file(dir(), "pnl", "csv");
    CsvLog log(file, "stamp,symbol,realized,unrealized");
    // No mark, so no unrealized P&L: an empty field, because a zero would
    // read as a flat position rather than an unknown one (GUI-Q15).
    log.row("2026-11-02T12:45:00.000Z", {"BTCD15M-26NOV02-1245", "1.250000", ""});
    EXPECT_EQ(contents(file.path()),
              "stamp,symbol,realized,unrealized\n"
              "2026-11-02T12:45:00.000Z,BTCD15M-26NOV02-1245,1.250000,\n");
}

TEST(CsvLogTest, StartsANewFileWithItsOwnHeaderWhenTheDayTurns) {
    const TempDir dir("cpm-csv-day-");
    DailyFile file(dir(), "fills", "csv");
    CsvLog log(file, "stamp,symbol");

    log.row("2026-11-02T23:59:59.999Z", {"one"});
    const std::string yesterday = file.path();
    log.row("2026-11-03T00:00:00.001Z", {"two"});

    EXPECT_EQ(contents(yesterday), "stamp,symbol\n2026-11-02T23:59:59.999Z,one\n");
    EXPECT_EQ(contents(file.path()), "stamp,symbol\n2026-11-03T00:00:00.001Z,two\n");
}

TEST(CsvLogTest, AppendsWithoutASecondHeaderAfterARestart) {
    const TempDir dir("cpm-csv-restart-");
    {
        DailyFile file(dir(), "fills", "csv");
        CsvLog log(file, "stamp,symbol");
        log.row("2026-11-02T09:00:00.000Z", {"before"});
    }
    DailyFile file(dir(), "fills", "csv");
    CsvLog log(file, "stamp,symbol");
    log.row("2026-11-02T09:30:00.000Z", {"after"});

    EXPECT_EQ(contents(file.path()),
              "stamp,symbol\n2026-11-02T09:00:00.000Z,before\n2026-11-02T09:30:00.000Z,after\n");
}

TEST(CsvLogTest, KeepsRowsWholeWhenThreadsWriteAtOnce) {
    const TempDir dir("cpm-csv-threads-");
    DailyFile file(dir(), "orders", "csv");
    CsvLog log(file, "stamp,writer,n");

    std::vector<std::thread> writers;
    for (int writer = 0; writer < 4; ++writer) {
        writers.emplace_back([&log, writer] {
            for (int n = 0; n < 50; ++n) {
                log.row("2026-11-02T12:00:00.000Z",
                        {std::to_string(writer), std::to_string(n)});
            }
        });
    }
    for (auto& writer : writers) writer.join();

    // QuickFIX calls back on its own threads: every row has to be one line
    // with its three fields, whatever ran at the same time.
    std::istringstream lines(contents(file.path()));
    std::string line;
    int rows = 0;
    ASSERT_TRUE(std::getline(lines, line));
    EXPECT_EQ(line, "stamp,writer,n");
    while (std::getline(lines, line)) {
        EXPECT_EQ(std::count(line.begin(), line.end(), ','), 2) << line;
        ++rows;
    }
    EXPECT_EQ(rows, 200);
}

}  // namespace
