#pragma once

#include "cpm/common/daily_file.hpp"

#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace cpm {

// One CSV field, quoted only when it has to be: a field holding a comma, a
// quote mark, a carriage return or a newline is wrapped in quotes with its
// own quotes doubled. Reject text and QuickFIX's Text(58) are the fields
// this exists for - everything else here is a number, a ticker or an enum.
std::string csv_field(std::string_view text);

// The fields joined with commas, each escaped. No trailing newline.
std::string csv_row(const std::vector<std::string>& fields);

// An append-only CSV of one kind of record, rolling over daily.
//
// The narrative log answers "what happened and why"; this answers "what did
// we do, at what price, and what did it make" - a question a prose log can
// only answer through a parser written against prose that was never a
// format. One row per record, fixed columns, and it opens in a spreadsheet
// or a dataframe with nothing to write first.
//
// Prices, sizes and amounts are written as decimal text through
// format_price/format_qty/format_amount, not as scaled integers: both are
// exact, and this way a row and a screenshot of the page cannot disagree.
// An absent optional is an empty field, never a zero - "not known" and
// "nothing" are different answers (GUI-Q15).
class CsvLog {
public:
    // `header` goes at the top of each new day's file and never again, so a
    // restart appends to today's file rather than planting a second header
    // down the middle of it.
    CsvLog(DailyFile& file, std::string header);

    CsvLog(const CsvLog&) = delete;
    CsvLog& operator=(const CsvLog&) = delete;

    // Opens today's file now, writing its header if it is new: a directory
    // that cannot be written is then found at startup rather than at the
    // first record. It has to go through here rather than through the
    // DailyFile, or the early open would take the "new file" answer with it
    // and the header would never be written. Returns why not, empty when it
    // opened.
    std::string open_now(std::string_view stamp);

    // One row, `stamp` first: the same UTC stamp the log line carries, which
    // is what joins a row to the narrative around it and to the other files.
    // Thread-safe, and flushed per row - these are the records a run is
    // reconstructed from, and a buffer that dies with the process is no
    // record at all.
    void row(std::string_view stamp, const std::vector<std::string>& fields);

private:
    // The header, if `opened` is a file that did not exist yet.
    void header_if_fresh(const DailyFile::Opened& opened);

    std::mutex mutex_;
    DailyFile& file_;
    std::string header_;
};

}  // namespace cpm
