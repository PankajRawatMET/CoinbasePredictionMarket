#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

namespace cpm::binance {

using HistoryClock = std::chrono::system_clock;

// One observed UTC second. Gaps stay gaps; no synthetic zero returns.
// The first observation is retained for window opens; the close is used
// for volatility. High/low preserve jumps that reverse between host passes.
struct ReferenceSample {
    HistoryClock::time_point second{};
    HistoryClock::time_point first_at{};
    std::int64_t first = 0;
    std::int64_t close = 0;
    std::int64_t low = 0;
    std::int64_t high = 0;
    // Receipt timestamps are available only for live observations.
    HistoryClock::time_point close_at{};
    std::int64_t boundary_mid = 0;
};

using HistoryView = std::vector<ReferenceSample>;

class ReferenceHistory {
public:
    static constexpr std::size_t kMaxSamples = 3602;
    // 3,600 completed returns need 3,601 completed prices, plus the current
    // partial bucket. Retain that extra boundary second.
    static constexpr std::chrono::seconds kRetention{3601};
    void observe(std::int64_t mid, HistoryClock::time_point at);
    // Restores only structurally valid, recent, ordered samples. Cached data
    // never marks the live feed fresh, and never fills a restart gap.
    bool restore(const ReferenceSample& sample, HistoryClock::time_point now);
    std::shared_ptr<const HistoryView> view() const;

private:
    std::deque<ReferenceSample> samples_;
    mutable std::shared_ptr<const HistoryView> cached_;
};

}  // namespace cpm::binance
