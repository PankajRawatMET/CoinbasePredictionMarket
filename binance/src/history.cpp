#include "cpm/binance/history.hpp"

#include <algorithm>

namespace cpm::binance {

void ReferenceHistory::observe(std::int64_t mid, HistoryClock::time_point at) {
    if (mid <= 0) return;
    const auto second = std::chrono::floor<std::chrono::seconds>(at);
    if (!samples_.empty() && second < samples_.back().second) return;
    if (!samples_.empty() && second == samples_.back().second) {
        auto& sample = samples_.back();
        sample.close = mid;
        sample.close_at = at;
        if (at == second) sample.boundary_mid = mid;
        sample.low = std::min(sample.low, mid);
        sample.high = std::max(sample.high, mid);
    } else {
        samples_.push_back({second, at, mid, mid, mid, mid, at, at == second ? mid : 0});
    }
    while (!samples_.empty() &&
           (samples_.size() > kMaxSamples || second - samples_.front().second > kRetention)) {
        samples_.pop_front();
    }
    cached_.reset();
}

bool ReferenceHistory::restore(const ReferenceSample& s, HistoryClock::time_point now) {
    if (s.second != std::chrono::floor<std::chrono::seconds>(s.second) ||
        s.first_at < s.second || s.first_at >= s.second + std::chrono::seconds(1) ||
        s.first <= 0 || s.close <= 0 || s.low <= 0 || s.low > std::min(s.first, s.close) ||
        s.high < std::max(s.first, s.close) || s.first_at > now ||
        std::chrono::floor<std::chrono::seconds>(now) - s.second > kRetention ||
        (!samples_.empty() && s.second <= samples_.back().second)) return false;
    samples_.push_back(s);
    while (samples_.size() > kMaxSamples) samples_.pop_front();
    cached_.reset();
    return true;
}

std::shared_ptr<const HistoryView> ReferenceHistory::view() const {
    if (!cached_) cached_ = std::make_shared<const HistoryView>(samples_.begin(), samples_.end());
    return cached_;
}

}  // namespace cpm::binance
