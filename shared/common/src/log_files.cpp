#include "cpm/common/log_files.hpp"

namespace cpm {

LogFiles::LogFiles(std::string dir, std::string run)
    : narrative_(dir, "cpm-trader", "log", run),
      strategy_(dir, "strategy", "log", run),
      wire_(std::move(dir), "wire", "log", std::move(run)) {}

std::string LogFiles::open_now(const std::string& stamp) {
    if (narrative_.for_stamp(stamp).out == nullptr) return narrative_.error();
    if (strategy_.for_stamp(stamp).out == nullptr) return strategy_.error();
    if (wire_.for_stamp(stamp).out == nullptr) return wire_.error();
    return {};
}

void LogFiles::write(DailyFile& file, LogLevel level, std::string_view stamp,
                     std::string_view message) {
    const auto opened = file.for_stamp(stamp);
    if (opened.out == nullptr) return;
    // The line stdout got, to the character.
    *opened.out << stamp << ' ' << to_string(level) << ' ' << message << '\n';
    opened.out->flush();
}

Logger::Sink LogFiles::sink() {
    return [this](LogLevel level, LogSource source, std::string_view stamp,
                  std::string_view message) {
        if (source == LogSource::Wire) {
            write(wire_, level, stamp, message);
            if (level < LogLevel::Warn) return;
        } else if (source == LogSource::Strategy) {
            write(strategy_, level, stamp, message);
            if (level < LogLevel::Warn) return;
        }
        write(narrative_, level, stamp, message);
    };
}

}  // namespace cpm
