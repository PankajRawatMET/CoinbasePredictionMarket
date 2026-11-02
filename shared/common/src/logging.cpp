#include "cpm/common/logging.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>

namespace cpm {

const char* to_string(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO ";
        case LogLevel::Warn: return "WARN ";
        case LogLevel::Error: return "ERROR";
    }
    return "?    ";
}

std::string utc_stamp() { return utc_stamp(std::chrono::system_clock::now()); }

std::string utc_stamp(std::chrono::system_clock::time_point when) {
    using namespace std::chrono;
    const auto ms = duration_cast<milliseconds>(when.time_since_epoch()).count() % 1000;
    const std::time_t t = system_clock::to_time_t(when);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[64];  // 24 chars used; sized for the widest int values GCC can prove
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms));
    return buf;
}

Logger::Logger(std::ostream& out, LogLevel min_level) : out_(out), min_level_(min_level) {}

void Logger::also_to(Sink sink) {
    std::lock_guard lock(mutex_);
    sinks_.push_back(std::move(sink));
}

void Logger::log(LogLevel level, std::string_view message, LogSource source) {
    if (level < min_level_) return;
    const std::string stamp = utc_stamp();
    std::lock_guard lock(mutex_);
    out_ << stamp << ' ' << to_string(level) << ' ' << message << '\n';
    out_.flush();
    for (const auto& sink : sinks_) sink(level, source, stamp, message);
}

}  // namespace cpm
