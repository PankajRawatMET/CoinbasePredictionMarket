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

std::string point_latest_at(const std::string& dir, std::string_view stamp,
                            const std::string& run) {
    if (run.empty()) return "no run directory to point at";
    const std::filesystem::path base = dir.empty() ? std::filesystem::path(".") : std::filesystem::path(dir);
    const std::filesystem::path link = base / "latest";
    // The date, as for_stamp() takes it: the first ten characters and no
    // more, so "2026-11-04T08:15:00.000Z" is the same day as "2026-11-04".
    const auto date = stamp.substr(0, std::min<std::size_t>(stamp.size(), 10));
    const std::filesystem::path target = std::filesystem::path(day_of(date)) / run;

    std::error_code failed;
    std::filesystem::create_directories(base / target, failed);
    if (failed && !std::filesystem::is_directory(base / target)) {
        return "could not create " + (base / target).string() + ": " + failed.message();
    }
    // Replaced rather than updated: there is no atomic retarget, and a link
    // that briefly does not exist is better than one that points nowhere.
    std::filesystem::remove(link, failed);
    std::filesystem::create_directory_symlink(target, link, failed);
    if (failed) return "could not link " + link.string() + ": " + failed.message();
    return {};
}

DailyFile::DailyFile(std::string dir, std::string prefix, std::string extension, std::string run)
    : dir_(std::move(dir)),
      run_(std::move(run)),
      prefix_(std::move(prefix)),
      extension_(std::move(extension)) {}

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
    std::string day_dir = (dir_.empty() ? "" : dir_ + "/") + day_of(date);
    // The run inside the day, when the caller named one: one daemon's files
    // together, and the day still the thing an operator archives.
    if (!run_.empty()) day_dir += "/" + run_;
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
