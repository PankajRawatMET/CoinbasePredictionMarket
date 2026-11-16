#pragma once

#include "cpm/binance/history.hpp"

#include <optional>

namespace cpm::strategy {

// The ordinary Student t CDF with four degrees of freedom, which is the
// T4 the model is written in terms of.
double student_t4_cdf(double z);

// How far the underlying has travelled from the strike, in standard
// deviations of the move still to come:
//
//     z = ln(spot / opening) / (correction * sigma * sqrt(seconds_left))
//
// Exposed beside the probability because it is the number that explains a
// quote - the probability alone cannot say whether fair moved because the
// underlying did or because the window ran down - and because a correction
// factor is fitted against it.
std::optional<double> quoting_z(std::int64_t spot, std::int64_t opening,
                                double sigma_per_sqrt_second, double seconds_left,
                                double correction);

std::optional<double> quoting_probability(std::int64_t spot, std::int64_t opening,
                                         double sigma_per_sqrt_second, double seconds_left,
                                         double correction);

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
