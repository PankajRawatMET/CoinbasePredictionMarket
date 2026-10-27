#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace cpm::oms {

// The only durable state an order manager keeps (OE-Q7, project instructions
// amendment 6): the newest ExecID processed, which anchors EventResendRequest
// after a restart, and the ClOrdID counter, so ids are never reused.
//
// Written whole and renamed into place, so a crash leaves either the old file
// or the new one and never a half-written line.
struct Bookmark {
    std::string last_exec_id;        // empty when nothing has been processed
    std::uint64_t cl_ord_counter = 0;
};

class BookmarkFile {
public:
    explicit BookmarkFile(std::string path);

    // Reads the file. Returns nullopt when it does not exist, and throws
    // std::runtime_error when it exists but cannot be read or parsed: a
    // corrupt bookmark must stop a run rather than silently restart from zero.
    std::optional<Bookmark> read() const;
    // Replaces the file atomically.
    void write(const Bookmark& bookmark) const;

    const std::string& path() const { return path_; }

private:
    std::string path_;
};

}  // namespace cpm::oms
