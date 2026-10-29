#pragma once

#include "cpm/common/logging.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace cpm {

// A bounded tail of the daemon's log, for the operator's page (GUI-Q27).
//
// The daemon still logs to stdout and always will: this is a copy of the
// lines worth reading from a browser, not a replacement for the real log.
// It is bounded because a daemon is meant to stay up for days, and it carries
// a sequence number per line so a page that has been watching all along is
// sent only what it has not seen.
class LogRing {
public:
    struct Line {
        std::uint64_t seq = 0;
        std::string stamp;  // the same UTC stamp the stream got
        LogLevel level = LogLevel::Info;
        std::string text;
    };

    explicit LogRing(std::size_t capacity = 500);

    void add(LogLevel level, std::string_view stamp, std::string_view text);

    // Everything after `after`, oldest first, at most `limit` lines. A page
    // joining with after = 0 gets the whole tail, which is what makes a
    // reload show the same thing as a session that never dropped.
    std::vector<Line> since(std::uint64_t after, std::size_t limit = 200) const;

    std::uint64_t last_seq() const;

private:
    mutable std::mutex mutex_;
    std::deque<Line> lines_;
    std::size_t capacity_;
    std::uint64_t next_ = 1;
};

}  // namespace cpm
