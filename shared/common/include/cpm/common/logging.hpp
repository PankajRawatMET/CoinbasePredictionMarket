#pragma once

#include <functional>
#include <mutex>
#include <ostream>
#include <string_view>

namespace cpm {

enum class LogLevel { Debug, Info, Warn, Error };

// Who wrote the line. Everything goes to the stream whatever this says; it
// exists so the operator's page can show the daemon's own events without the
// two things that would bury them - the raw FIX traffic and a market maker's
// running commentary on its own quotes (GUI-Q28).
enum class LogSource {
    Daemon,    // the daemon, its sessions, the host, the reconciler
    Strategy,  // a strategy talking about its own quoting, through its hook
    Wire,      // message traffic: the raw FIX log, and the one-line summary
               // of each order and cancel going out
};

// Thread-safe line logger. QuickFIX calls back on its own threads, so every
// line is written under a lock. Callers own the instance and pass it down.
class Logger {
public:
    explicit Logger(std::ostream& out, LogLevel min_level = LogLevel::Info);

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    // A second home for every line that is written, on top of the stream:
    // the daemon's page shows the tail of it (GUI-Q27). Called under the
    // same lock as the write, so the two can never disagree about the order,
    // which means a sink must not log. Set once at startup.
    using Sink = std::function<void(LogLevel, LogSource, std::string_view stamp,
                                    std::string_view message)>;
    void also_to(Sink sink);

    void log(LogLevel level, std::string_view message, LogSource source = LogSource::Daemon);
    void debug(std::string_view message, LogSource source = LogSource::Daemon) {
        log(LogLevel::Debug, message, source);
    }
    void info(std::string_view message, LogSource source = LogSource::Daemon) {
        log(LogLevel::Info, message, source);
    }
    void warn(std::string_view message, LogSource source = LogSource::Daemon) {
        log(LogLevel::Warn, message, source);
    }
    void error(std::string_view message, LogSource source = LogSource::Daemon) {
        log(LogLevel::Error, message, source);
    }

private:
    std::mutex mutex_;
    std::ostream& out_;
    LogLevel min_level_;
    Sink sink_;
};

}  // namespace cpm
