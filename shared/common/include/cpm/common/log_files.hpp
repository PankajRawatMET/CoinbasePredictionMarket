#pragma once

#include "cpm/common/daily_file.hpp"
#include "cpm/common/logging.hpp"

#include <string>

namespace cpm {

// The daemon's log on disk, one directory per run of it:
//
//     <dir>/YYYYMMDD/<run>/cpm-trader.log  sessions, rolls, halts, problems
//     <dir>/YYYYMMDD/<run>/strategy.log    a strategy on its own quoting
//     <dir>/YYYYMMDD/<run>/wire.log        << and >> raw FIX, each order out
//
// They are a copy of what stdout gets, not a replacement: the console, the
// banner and the page's log panel all read the same Logger, and a run under
// a redirect keeps working exactly as before.
//
// The wire and the strategies are split off because they are the bulk of
// what this daemon writes - two strategies quoting put out some thirty lines
// a second - and the file an operator opens after something went wrong
// should be the one without them. It is the same division the page's log
// panel already makes (GUI-Q28), for the same reason, and nothing is lost:
// every file is kept, each one a single timeline, so `sort -m` puts any two
// of them back together.
//
// They are split by SOURCE and never by product. A per-instrument split
// cannot file the lines that matter - "halting: reject - order mm-7x7
// rejected" names a ClOrdID and no symbol - and it hides a daemon-wide halt
// from whichever instrument was not the cause (design-logs.md section 3).
//
// What went wrong is the exception: a Warn or an Error from the wire or a
// strategy is written to the narrative as well, so that file never misses a
// problem because of which part of the daemon noticed it first.
class LogFiles {
public:
    // Nothing is opened yet; the directories are created by the first line
    // written. `run` names this run's directory inside the day's; empty
    // writes straight into the day's, as this did before.
    explicit LogFiles(std::string dir, std::string run = {});

    LogFiles(const LogFiles&) = delete;
    LogFiles& operator=(const LogFiles&) = delete;

    // Opens both files for `stamp` now, so a directory that cannot be
    // written is found at startup rather than silently at the first line.
    // Returns why not, empty when both opened.
    std::string open_now(const std::string& stamp);

    // Install with Logger::also_to. Called under the Logger's lock, which is
    // what keeps the files in the same order as stdout - and means this must
    // never log: a failed write is dropped rather than reported, because the
    // only way to report it is the thing that just failed.
    Logger::Sink sink();

    const std::string& narrative_path() const { return narrative_.path(); }
    const std::string& strategy_path() const { return strategy_.path(); }
    const std::string& wire_path() const { return wire_.path(); }

private:
    void write(DailyFile& file, LogLevel level, std::string_view stamp, std::string_view message);

    DailyFile narrative_;
    DailyFile strategy_;
    DailyFile wire_;
};

}  // namespace cpm
