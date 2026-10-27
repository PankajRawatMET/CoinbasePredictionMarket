#include "cpm/md/events.hpp"

#include <cstdio>
#include <ctime>

namespace cpm::md {

std::string to_utc_string(Clock::time_point time) {
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(time.time_since_epoch()).count();
    const auto secs = static_cast<std::time_t>(micros / 1000000);
    std::tm tm{};
    gmtime_r(&secs, &tm);
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d-%02d:%02d:%02d.%06lld", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<long long>(micros % 1000000));
    return buf;
}

const char* to_string(BookSide side) { return side == BookSide::Yes ? "YES" : "NO"; }

const char* to_string(BookAction action) { return action == BookAction::Set ? "set" : "delete"; }

const char* to_string(Aggressor aggressor) {
    switch (aggressor) {
        case Aggressor::None: return "none";
        case Aggressor::YesTaker: return "yes-taker";
        case Aggressor::NoTaker: return "no-taker";
    }
    return "none";
}

const char* to_string(LifecycleStatus status) {
    switch (status) {
        case LifecycleStatus::Unknown: return "Unknown";
        case LifecycleStatus::Listed: return "Listed";
        case LifecycleStatus::Open: return "Open";
        case LifecycleStatus::Pause: return "Pause";
        case LifecycleStatus::Closed: return "Closed";
        case LifecycleStatus::Determined: return "Determined";
        case LifecycleStatus::Settled: return "Settled";
        case LifecycleStatus::Amended: return "Amended";
        case LifecycleStatus::Delisted: return "Delisted";
    }
    return "Unknown";
}

}  // namespace cpm::md
