#pragma once

#include "cpm/binance/history.hpp"

#include <optional>

namespace cpm::strategy {

// The PDF's T4 is the ordinary Student t CDF. UnitVariance rescales its
// argument so sigma is the resulting distribution's standard deviation.
enum class TScale { Raw, UnitVariance };
double student_t4_cdf(double z);
std::optional<double> quoting_probability(std::int64_t spot, std::int64_t opening,
                                         double sigma_per_sqrt_second, double seconds_left,
                                         double correction, TScale scale = TScale::Raw);

// Sample standard deviation of contiguous completed one-second log returns.
// The current partial second is excluded, as are future observations.
int contiguous_reference_returns(const cpm::binance::HistoryView& history,
                                  int lookback_seconds,
                                  cpm::binance::HistoryClock::time_point now);
std::optional<double> reference_volatility(const cpm::binance::HistoryView& history,
                                           int lookback_seconds,
                                           cpm::binance::HistoryClock::time_point now);
std::optional<std::int64_t> reference_opening(const cpm::binance::HistoryView& history,
                                             cpm::binance::HistoryClock::time_point window_start);

}  // namespace cpm::strategy
