#pragma once

#include "cpm/binance/reference.hpp"
#include "cpm/common/https.hpp"
#include "cpm/strategy/quoting_klines.hpp"

#include <deque>
#include <optional>

namespace cpm::strategy {

using PricingSecond = std::chrono::sys_seconds;

struct PricingHistory {
    double strike = 0;
    std::deque<double> minute_variances;
    double fast_variance = 0;
    double last_mid = 0;
    PricingSecond last_second{};
    std::chrono::duration<double> max_age{};
    double open = 0, high = 0, low = 0, close = 0;
    int minute_count = 0;
    double settlement_sum = 0;
    int settlement_count = 0;

    void add(PricingSecond second, double mid, PricingSecond final_start);
    double sigma() const;
    std::optional<double> z(double spot, double remaining, int elapsed,
                            double correction, double offset) const;
};

struct HistoryResult {
    PricingHistory state;
    std::string error;
};

HistoryResult fetch_pricing_history(cpm::HttpsRequest request, const HttpsFetch& fetch,
                                   const std::string& symbol, PricingSecond as_of,
                                   PricingSecond window_open, PricingSecond final_start);

}  // namespace cpm::strategy
