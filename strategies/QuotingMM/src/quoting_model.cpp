#include "cpm/strategy/quoting_model.hpp"

#include <algorithm>
#include <cmath>

namespace cpm::strategy {

double student_t4_cdf(double z) {
    // Integral of (3/8) * (1 + z*z/4)^(-5/2), with u=z/sqrt(4+z*z).
    // Evaluate the small tail directly to avoid cancellation near 0 and 1.
    if (std::isinf(z)) return z > 0 ? 1.0 : 0.0;
    const double u = std::abs(z) / std::hypot(2.0, z);
    const double tail = 0.25 * (1.0 - u) * (1.0 - u) * (2.0 + u);
    return z < 0 ? tail : 1.0 - tail;
}

std::optional<double> quoting_probability(std::int64_t spot, std::int64_t opening,
                                         double sigma, double seconds_left,
                                         double correction, TScale scale) {
    if (spot <= 0 || opening <= 0 || !std::isfinite(sigma) || sigma <= 0 ||
        !std::isfinite(seconds_left) || seconds_left <= 0 ||
        !std::isfinite(correction) || correction <= 0) return std::nullopt;
    const double denominator = correction * sigma * std::sqrt(seconds_left);
    if (!std::isfinite(denominator) || denominator <= 0) return std::nullopt;
    double z = std::log(static_cast<double>(spot) / static_cast<double>(opening)) / denominator;
    if (scale == TScale::UnitVariance) z *= std::sqrt(2.0);
    if (std::isnan(z)) return std::nullopt;
    return student_t4_cdf(z);
}

int contiguous_reference_returns(const cpm::binance::HistoryView& history,
                                  int lookback_seconds,
                                  cpm::binance::HistoryClock::time_point now) {
    using namespace std::chrono;
    if (lookback_seconds <= 0) return 0;
    auto expected = floor<seconds>(now) - seconds(1);
    auto it = std::upper_bound(history.begin(), history.end(), expected,
                                [](auto at, const auto& sample) { return at < sample.second; });
    int prices = 0;
    while (it != history.begin() && prices <= lookback_seconds) {
        --it;
        if (it->second != expected || it->close <= 0) break;
        ++prices;
        expected -= seconds(1);
    }
    return std::max(0, prices - 1);
}

std::optional<double> reference_volatility(const cpm::binance::HistoryView& history,
                                           int lookback_seconds,
                                           cpm::binance::HistoryClock::time_point now) {
    using namespace std::chrono;
    if (lookback_seconds < 2) return std::nullopt;
    const auto end = floor<seconds>(now) - seconds(1);
    const auto start = end - seconds(lookback_seconds);
    auto it = std::lower_bound(history.begin(), history.end(), start,
                               [](const auto& sample, auto at) { return sample.second < at; });
    if (it == history.end() || it->second != start || it->close <= 0) return std::nullopt;
    double previous = static_cast<double>(it->close);
    double mean = 0, m2 = 0;
    ++it;
    for (int n = 1; n <= lookback_seconds; ++n, ++it) {
        if (it == history.end() || it->second != start + seconds(n) || it->close <= 0) {
            return std::nullopt;
        }
        const double current = static_cast<double>(it->close);
        const double r = std::log(current / previous);
        const double delta = r - mean;
        mean += delta / n;
        m2 += delta * (r - mean);
        previous = current;
    }
    const double sigma = std::sqrt(std::max(0.0, m2) / (lookback_seconds - 1));
    if (!std::isfinite(sigma) || sigma <= 0) return std::nullopt;
    return sigma;
}

std::optional<std::int64_t> reference_opening(const cpm::binance::HistoryView& history,
                                             cpm::binance::HistoryClock::time_point start) {
    const auto it = std::lower_bound(history.begin(), history.end(), start,
                                    [](const auto& sample, auto at) { return sample.second < at; });
    if (it == history.end() || it->second != start || it->first <= 0 ||
        it->first_at < start || it->first_at >= start + std::chrono::seconds(1)) return std::nullopt;
    return it->first;
}

}  // namespace cpm::strategy
