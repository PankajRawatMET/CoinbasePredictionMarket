#include "cpm/common/fixed.hpp"

#include <limits>

namespace cpm {
namespace {

std::int64_t pow10(int digits) {
    std::int64_t unit = 1;
    for (int i = 0; i < digits; ++i) unit *= 10;
    return unit;
}

bool all_digits(std::string_view text) {
    if (text.empty()) return false;
    for (const char c : text) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

// Accumulates digits, refusing anything that would not fit in int64.
std::optional<std::int64_t> accumulate(std::string_view digits) {
    constexpr auto kMax = std::numeric_limits<std::int64_t>::max();
    std::int64_t value = 0;
    for (const char c : digits) {
        const std::int64_t digit = c - '0';
        if (value > (kMax - digit) / 10) return std::nullopt;
        value = value * 10 + digit;
    }
    return value;
}

}  // namespace

std::optional<std::int64_t> parse_scaled(std::string_view text, int scale_digits) {
    if (scale_digits < 0 || scale_digits > 18) return std::nullopt;
    const auto dot = text.find('.');
    std::string_view whole = text.substr(0, dot);
    std::string_view frac;
    if (dot != std::string_view::npos) {
        frac = text.substr(dot + 1);
        if (!all_digits(frac) || frac.size() > static_cast<std::size_t>(scale_digits)) return std::nullopt;
        if (text.find('.', dot + 1) != std::string_view::npos) return std::nullopt;
    }
    if (!all_digits(whole)) return std::nullopt;

    const auto units = accumulate(whole);
    if (!units) return std::nullopt;
    const std::int64_t unit = pow10(scale_digits);
    if (*units > std::numeric_limits<std::int64_t>::max() / unit) return std::nullopt;
    std::int64_t value = *units * unit;

    // "0.44" at scale 3 is 440, not 44: pad the fraction out to the scale.
    const auto padded = accumulate(frac.empty() ? std::string_view("0") : frac);
    if (!padded) return std::nullopt;
    value += *padded * pow10(scale_digits - static_cast<int>(frac.size()));
    return value;
}

std::optional<std::int64_t> parse_signed_truncating(std::string_view text, int scale_digits) {
    if (scale_digits < 0 || scale_digits > 18) return std::nullopt;
    bool negative = false;
    if (!text.empty() && (text.front() == '-' || text.front() == '+')) {
        negative = text.front() == '-';
        text.remove_prefix(1);
    }
    const auto dot = text.find('.');
    std::string_view whole = text.substr(0, dot);
    std::string_view frac;
    if (dot != std::string_view::npos) {
        frac = text.substr(dot + 1);
        if (!all_digits(frac)) return std::nullopt;
        if (text.find('.', dot + 1) != std::string_view::npos) return std::nullopt;
        // Truncate toward zero rather than rounding, as the gateway does.
        if (frac.size() > static_cast<std::size_t>(scale_digits)) {
            frac = frac.substr(0, static_cast<std::size_t>(scale_digits));
        }
    }
    if (!all_digits(whole)) return std::nullopt;

    const auto units = accumulate(whole);
    if (!units) return std::nullopt;
    const std::int64_t unit = pow10(scale_digits);
    if (*units > std::numeric_limits<std::int64_t>::max() / unit) return std::nullopt;
    std::int64_t value = *units * unit;

    const auto fraction = accumulate(frac.empty() ? std::string_view("0") : frac);
    if (!fraction) return std::nullopt;
    value += *fraction * pow10(scale_digits - static_cast<int>(frac.size()));
    return negative ? -value : value;
}

std::string format_signed(std::int64_t value, int scale_digits) {
    if (value >= 0) return format_scaled(value, scale_digits);
    return "-" + format_scaled(-value, scale_digits);
}

std::string format_scaled(std::int64_t value, int scale_digits) {
    if (value < 0 || scale_digits < 0 || scale_digits > 18) return "0";
    const std::int64_t unit = pow10(scale_digits);
    std::string frac = std::to_string(value % unit);
    frac.insert(0, static_cast<std::size_t>(scale_digits) - frac.size(), '0');
    while (!frac.empty() && frac.back() == '0') frac.pop_back();
    return std::to_string(value / unit) + (frac.empty() ? "" : "." + frac);
}

}  // namespace cpm
