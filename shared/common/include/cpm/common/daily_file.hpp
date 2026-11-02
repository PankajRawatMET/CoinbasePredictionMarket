#pragma once

#include <fstream>
#include <ostream>
#include <string>
#include <string_view>

namespace cpm {

// An append-only file in a directory named for the UTC day it covers:
//
//     <dir>/YYYYMMDD/<prefix>.<extension>
//
// One directory per day, so a day's whole record - the narrative, the wire
// and the four record files - is one thing to open, copy or archive, and
// `ls` of the parent is the list of days the daemon has run.
//
// A daemon meant to stay up for days has to end its file somewhere, and the
// trading day is the only boundary the operator already thinks in. Opened on
// the line it is first given rather than in the constructor, so a stream
// with nothing to say leaves no empty file behind - though a caller may open
// it early on purpose, as LogFiles does to find a full disk at startup
// rather than at the first interesting line.
//
// It has no clock of its own: the caller passes the stamp its line already
// carries, which is what keeps a line and the file it lands in from ever
// disagreeing about the date. The same property makes the rollover testable
// without waiting for midnight.
//
// Append, never rewrite - so there is no temporary-and-rename to do as
// BookmarkFile does. A kill -9 costs at most the line being written, and the
// next run appends to the same file rather than truncating it.
class DailyFile {
public:
    // `dir` may be nested and is created, with the day's directory under it,
    // on the first write. An empty `dir` puts the day's directory beside the
    // daemon.
    DailyFile(std::string dir, std::string prefix, std::string extension);

    struct Opened {
        std::ostream* out = nullptr;  // null when the file cannot be written
        bool fresh = false;           // this call created it: a CSV writes its header
    };

    // The file for `stamp`, opening or rotating first. A day whose file
    // cannot be opened is not tried again until the date changes: a daemon
    // must not stall on a full disk, and it must not repeat the same failure
    // on every line either. Not thread-safe; the owner holds the lock.
    Opened for_stamp(std::string_view stamp);

    // The path last opened, or the one that failed. Empty before the first call.
    const std::string& path() const { return path_; }
    // Why the last open failed, empty when it did not. Cleared by a rollover.
    const std::string& error() const { return error_; }

private:
    std::string dir_;
    std::string prefix_;
    std::string extension_;
    std::string date_;  // "2026-11-02": the day out_ covers, failed or not
    std::string path_;
    std::string error_;
    std::ofstream out_;
};

}  // namespace cpm
