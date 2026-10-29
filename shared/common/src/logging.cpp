#include "cpm/common/logging.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>

namespace cpm {
namespace {

const char* level_name(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO ";
        case LogLevel::Warn: return "WARN ";
        case LogLevel::Error: return "ERROR";
    }
    return "?    ";
}

// 2026-10-23T05:41:10.414Z
std::string utc_now() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
    const std::time_t t = system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[64];  // 24 chars used; sized for the widest int values GCC can prove
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms));
    return buf;
}

}  // namespace

Logger::Logger(std::ostream& out, LogLevel min_level) : out_(out), min_level_(min_level) {}

void Logger::also_to(Sink sink) {
    std::lock_guard lock(mutex_);
    sink_ = std::move(sink);
}

void Logger::log(LogLevel level, std::string_view message, LogSource source) {
    if (level < min_level_) return;
    const std::string stamp = utc_now();
    std::lock_guard lock(mutex_);
    out_ << stamp << ' ' << level_name(level) << ' ' << message << '\n';
    out_.flush();
    if (sink_) sink_(level, source, stamp, message);
}

}  // namespace cpm
