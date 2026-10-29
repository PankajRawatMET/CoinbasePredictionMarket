#include "cpm/md/ticker.hpp"

#include <array>
#include <cctype>
#include <ctime>

namespace cpm::md {
namespace {

bool all_digits(std::string_view text) {
    if (text.empty()) return false;
    for (const char c : text) {
        if (std::isdigit(static_cast<unsigned char>(c)) == 0) return false;
    }
    return true;
}

bool all_alpha(std::string_view text) {
    if (text.empty()) return false;
    for (const char c : text) {
        if (std::isalpha(static_cast<unsigned char>(c)) == 0) return false;
    }
    return true;
}

int to_int(std::string_view text) {
    int value = 0;
    for (const char c : text) value = value * 10 + (c - '0');
    return value;
}

// The month as the venue writes it, uppercase and three letters.
std::optional<int> month_of(std::string_view text) {
    static constexpr std::array<const char*, 12> kMonths{"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                                         "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
    for (std::size_t i = 0; i < kMonths.size(); ++i) {
        if (text == kMonths[i]) return static_cast<int>(i) + 1;
    }
    return std::nullopt;
}

}  // namespace

bool looks_like_instrument(std::string_view symbol) {
    const auto dash = symbol.find('-');
    if (dash == std::string_view::npos) return false;
    // Something either side of it: "-X" and "X-" are not names either.
    return dash > 0 && dash + 1 < symbol.size();
}

std::optional<Ticker> parse_ticker(std::string_view symbol) {
    const auto first = symbol.find('-');
    if (first == std::string_view::npos) return std::nullopt;
    const auto second = symbol.find('-', first + 1);
    if (second == std::string_view::npos) return std::nullopt;
    if (symbol.find('-', second + 1) != std::string_view::npos) return std::nullopt;

    const auto series = symbol.substr(0, first);
    const auto middle = symbol.substr(first + 1, second - first - 1);
    const auto suffix = symbol.substr(second + 1);
    if (series.empty() || suffix.empty()) return std::nullopt;

    // Read the date back to front, so the asset is whatever is left: it is the
    // only part whose length varies (BTC, DOGE, HYPE).
    constexpr std::size_t kDateLength = 2 + 3 + 2 + 4;  // YY MON DD HHMM
    if (middle.size() <= kDateLength) return std::nullopt;
    const auto asset = middle.substr(0, middle.size() - kDateLength);
    const auto date = middle.substr(middle.size() - kDateLength);
    if (!all_alpha(asset)) return std::nullopt;

    const auto year = date.substr(0, 2);
    const auto month = date.substr(2, 3);
    const auto day = date.substr(5, 2);
    const auto time = date.substr(7, 4);
    if (!all_digits(year) || !all_digits(day) || !all_digits(time)) return std::nullopt;
    const auto month_number = month_of(month);
    if (!month_number) return std::nullopt;

    const int day_number = to_int(day);
    const int hour = to_int(time.substr(0, 2));
    const int minute = to_int(time.substr(2, 2));
    if (day_number < 1 || day_number > 31 || hour > 23 || minute > 59) return std::nullopt;

    std::tm when{};
    when.tm_year = 2000 + to_int(year) - 1900;
    when.tm_mon = *month_number - 1;
    when.tm_mday = day_number;
    when.tm_hour = hour;
    when.tm_min = minute;
    // timegm, not mktime: the name is UTC and the daemon's timezone is its own
    // business.
    const std::time_t seconds = ::timegm(&when);
    if (seconds == static_cast<std::time_t>(-1)) return std::nullopt;

    Ticker out;
    out.series = std::string(series);
    out.asset = std::string(asset);
    out.suffix = std::string(suffix);
    out.window_end = Clock::from_time_t(seconds);
    return out;
}

}  // namespace cpm::md
