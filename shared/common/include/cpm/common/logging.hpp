#pragma once

#include <mutex>
#include <ostream>
#include <string_view>

namespace cpm {

enum class LogLevel { Debug, Info, Warn, Error };

// Thread-safe line logger. QuickFIX calls back on its own threads, so every
// line is written under a lock. Callers own the instance and pass it down.
class Logger {
public:
    explicit Logger(std::ostream& out, LogLevel min_level = LogLevel::Info);

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void log(LogLevel level, std::string_view message);
    void debug(std::string_view message) { log(LogLevel::Debug, message); }
    void info(std::string_view message) { log(LogLevel::Info, message); }
    void warn(std::string_view message) { log(LogLevel::Warn, message); }
    void error(std::string_view message) { log(LogLevel::Error, message); }

private:
    std::mutex mutex_;
    std::ostream& out_;
    LogLevel min_level_;
};

}  // namespace cpm
