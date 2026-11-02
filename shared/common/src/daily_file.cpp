#include "cpm/common/daily_file.hpp"

#include <cerrno>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <system_error>

namespace cpm {
namespace {

// "2026-11-02T05:41:10.414Z" -> "20261102". Anything shorter than a date is
// taken as it comes: a stamp this file cannot read is still better in one
// file than dropped.
std::string day_of(std::string_view date) {
    std::string day;
    for (const char c : date) {
        if (c != '-') day.push_back(c);
    }
    return day;
}

}  // namespace

DailyFile::DailyFile(std::string dir, std::string prefix, std::string extension)
    : dir_(std::move(dir)), prefix_(std::move(prefix)), extension_(std::move(extension)) {}

DailyFile::Opened DailyFile::for_stamp(std::string_view stamp) {
    const std::string date(stamp.substr(0, std::min<std::size_t>(stamp.size(), 10)));
    if (date == date_) {
        // Same day: the open file, or nothing if opening it already failed.
        return {out_.is_open() ? &out_ : nullptr, false};
    }
    date_ = date;
    error_.clear();
    out_.close();
    out_.clear();

    // The day is the directory, not part of the name: everything written
    // for one day belongs together.
    const std::string day_dir = (dir_.empty() ? "" : dir_ + "/") + day_of(date);
    path_ = day_dir + "/" + prefix_ + "." + extension_;
    {
        std::error_code failed;
        std::filesystem::create_directories(day_dir, failed);
        if (failed && !std::filesystem::is_directory(day_dir)) {
            error_ = "cannot create " + day_dir + ": " + failed.message();
            return {};
        }
    }
    std::error_code ignored;
    const bool existed = std::filesystem::exists(path_, ignored);

    errno = 0;
    out_.open(path_, std::ios::app);
    if (!out_.is_open()) {
        error_ = "cannot open " + path_ + ": " + std::strerror(errno);
        return {};
    }
    return {&out_, !existed};
}

}  // namespace cpm
