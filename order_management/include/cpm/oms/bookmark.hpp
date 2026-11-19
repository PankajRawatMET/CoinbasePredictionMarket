#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace cpm::oms {

// The only durable state an order manager keeps (OE-Q7, project instructions
// amendment 6): the newest ExecID processed, which anchors EventResendRequest
// after a restart.
//
// It once carried a ClOrdID counter as well, saved on every id allocated so
// that a restart could not reuse one. That is now the run tag inside the id
// itself (cpm::oms::run_tag), which costs nothing, so the only thing left here
// is the one value no clock can reconstruct: how far we had got.
//
// Written whole and renamed into place, so a crash leaves either the old file
// or the new one and never a half-written line.
struct Bookmark {
    std::string last_exec_id;  // empty when nothing has been processed
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
