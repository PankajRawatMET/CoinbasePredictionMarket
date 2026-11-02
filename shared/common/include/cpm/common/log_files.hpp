#pragma once

#include "cpm/common/daily_file.hpp"
#include "cpm/common/logging.hpp"

#include <string>

namespace cpm {

// The daemon's log on disk: the narrative in one file, the message traffic
// in another.
//
//     <dir>/cpm-trader-YYYYMMDD.log   sessions, rolls, strategies, problems
//     <dir>/wire-YYYYMMDD.log         << and >> raw FIX, and each order out
//
// They are a copy of what stdout gets, not a replacement: the console, the
// banner and the page's log panel all read the same Logger, and a run under
// a redirect keeps working exactly as before.
//
// The wire is split off because it is the bulk of what this daemon writes -
// two strategies quoting put out some thirty lines a second - and the file
// an operator opens after something went wrong should be the one without it.
// It is the same division the page's log panel already makes (GUI-Q28), for
// the same reason, and nothing is lost: both files are kept.
//
// What went wrong is the exception: a Warn or an Error from the wire is
// written to both, so the narrative file never misses a problem because of
// which part of the daemon noticed it first.
class LogFiles {
public:
    // Nothing is opened yet; `dir` is created by the first line written.
    explicit LogFiles(std::string dir);

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
    const std::string& wire_path() const { return wire_.path(); }

private:
    void write(DailyFile& file, LogLevel level, std::string_view stamp, std::string_view message);

    DailyFile narrative_;
    DailyFile wire_;
};

}  // namespace cpm
