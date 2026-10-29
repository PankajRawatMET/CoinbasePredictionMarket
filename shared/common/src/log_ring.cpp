#include "cpm/common/log_ring.hpp"

#include <algorithm>

namespace cpm {

LogRing::LogRing(std::size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

void LogRing::add(LogLevel level, std::string_view stamp, std::string_view text) {
    std::lock_guard lock(mutex_);
    lines_.push_back(Line{next_++, std::string(stamp), level, std::string(text)});
    while (lines_.size() > capacity_) lines_.pop_front();
}

std::vector<LogRing::Line> LogRing::since(std::uint64_t after, std::size_t limit) const {
    std::vector<Line> out;
    std::lock_guard lock(mutex_);
    for (const auto& line : lines_) {
        if (line.seq > after) out.push_back(line);
    }
    // The newest `limit`, not the oldest: a page that has been away is better
    // served the end of what it missed than the start of it.
    if (out.size() > limit) out.erase(out.begin(), out.end() - static_cast<long>(limit));
    return out;
}

std::uint64_t LogRing::last_seq() const {
    std::lock_guard lock(mutex_);
    return lines_.empty() ? 0 : lines_.back().seq;
}

}  // namespace cpm
