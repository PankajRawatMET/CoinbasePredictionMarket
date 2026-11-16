#include "cpm/strategy/quoting_warmup.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>

namespace cpm::strategy {
namespace {

std::string utc(KlineClock::time_point when) {
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(when.time_since_epoch());
    const std::time_t raw = static_cast<std::time_t>(seconds.count());
    std::tm parts{};
    gmtime_r(&raw, &parts);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%SZ", &parts);
    return buffer;
}

std::string key_for(const WarmupRequest& request) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        request.window_start.time_since_epoch());
    return request.symbol + "|" + std::to_string(ms.count()) + "|" + request.interval + "|" +
           std::to_string(request.lookback_minutes);
}

}  // namespace

std::string WindowWarmup::describe() const {
    return std::to_string(lookback_minutes) + "m of " + interval + " klines to " +
           utc(window_start);
}

WarmupResult warmup_from_bars(const std::vector<Kline>& bars, const WarmupRequest& request,
                              KlineClock::time_point now) {
    using namespace std::chrono;
    WarmupResult out;
    const int per_bar = kline_interval_minutes(request.interval);
    if (per_bar == 0) {
        out.error = "\"" + request.interval + "\" is not a futures kline interval";
        return out;
    }
    if (request.lookback_minutes < 2 * per_bar || request.lookback_minutes % per_bar != 0) {
        out.error = "a " + std::to_string(request.lookback_minutes) + "-minute lookback is not a "
                    "whole number of " + request.interval + " bars, or is too short to have a "
                    "variance";
        return out;
    }
    // n minutes of returns is n/per_bar returns, which needs one more close
    // than that - the same arithmetic the one-second path used.
    const int wanted_returns = request.lookback_minutes / per_bar;
    const auto step = minutes(per_bar);
    const auto first = request.window_start - step * (wanted_returns + 1);

    // The window's own bar is the last one, and its open is the opening
    // price: the first price the venue traded at or after the window opened.
    if (bars.empty() || bars.back().open_time != request.window_start) {
        out.error = "the venue has no " + request.interval + " bar opening at " +
                    utc(request.window_start) + ", so this window has no opening price yet";
        return out;
    }
    if (bars.size() < static_cast<std::size_t>(wanted_returns) + 2) {
        out.error = "the venue returned " + std::to_string(bars.size()) + " bars where " +
                    std::to_string(wanted_returns + 2) + " were needed for " +
                    std::to_string(request.lookback_minutes) + " minutes of returns";
        return out;
    }

    // Walk only the bars before the window, contiguously, from the oldest
    // one needed. Anything missing, out of step or still open is refused.
    const auto begin = bars.end() - (wanted_returns + 2);
    std::vector<std::int64_t> closes;
    closes.reserve(wanted_returns + 1);
    auto expected = first;
    for (auto it = begin; it + 1 != bars.end(); ++it, expected += step) {
        if (it->open_time != expected) {
            out.error = "the venue's history has a gap at " + utc(expected) +
                        ": a volatility taken across it would understate the move";
            return out;
        }
        if (!it->closed) {
            out.error = "the bar at " + utc(it->open_time) + " has not closed yet";
            return out;
        }
        if (it->close <= 0) {
            out.error = "the bar at " + utc(it->open_time) + " has no close";
            return out;
        }
        closes.push_back(it->close);
    }
    if (bars.back().open <= 0) {
        out.error = "the bar at " + utc(request.window_start) + " has no opening price";
        return out;
    }
    if (now < request.window_start) {
        out.error = "the window at " + utc(request.window_start) + " has not opened yet";
        return out;
    }

    // Sample standard deviation of close-to-close log returns, by Welford.
    double mean = 0, m2 = 0;
    for (std::size_t i = 1; i < closes.size(); ++i) {
        const double r = std::log(static_cast<double>(closes[i]) / static_cast<double>(closes[i - 1]));
        const double delta = r - mean;
        mean += delta / static_cast<double>(i);
        m2 += delta * (r - mean);
    }
    const double per_bar_sigma = std::sqrt(std::max(0.0, m2) / (wanted_returns - 1));
    // The model counts in seconds, so the per-bar figure is rescaled by the
    // square root of the bar's length. This is the one place the two sigmas
    // are made comparable.
    const double sigma = per_bar_sigma / std::sqrt(static_cast<double>(per_bar) * 60.0);
    if (!std::isfinite(sigma) || sigma <= 0) {
        out.error = "the " + std::to_string(request.lookback_minutes) +
                    " minutes before " + utc(request.window_start) +
                    " have no variance: the underlying did not move";
        return out;
    }

    out.warmup.opening = bars.back().open;
    out.warmup.sigma = sigma;
    out.warmup.returns = wanted_returns;
    out.warmup.window_start = request.window_start;
    out.warmup.symbol = request.symbol;
    out.warmup.interval = request.interval;
    out.warmup.lookback_minutes = request.lookback_minutes;
    return out;
}

WarmupResult warm_window(const WarmupRequest& request, const HttpsFetch& fetch,
                         KlineClock::time_point now) {
    WarmupResult out;
    const int per_bar = kline_interval_minutes(request.interval);
    if (per_bar == 0) {
        out.error = "\"" + request.interval + "\" is not a futures kline interval";
        return out;
    }
    if (request.lookback_minutes < 2 * per_bar || request.lookback_minutes % per_bar != 0) {
        out.error = "a " + std::to_string(request.lookback_minutes) + "-minute lookback is not a "
                    "whole number of " + request.interval + " bars, or is too short to have a "
                    "variance";
        return out;
    }

    KlineRequest klines;
    klines.symbol = request.symbol;
    klines.interval = request.interval;
    // The returns, their anchor, and the window's own bar.
    klines.limit = request.lookback_minutes / per_bar + 2;
    klines.end_time = request.window_start;
    klines.host = request.host;
    klines.port = request.port;
    klines.ca_file = request.ca_file;
    klines.timeout = request.timeout;

    const auto series = fetch_klines(klines, fetch, now);
    if (!series.ok()) {
        out.error = series.error;
        return out;
    }
    return warmup_from_bars(series.bars, request, now);
}

WarmupCache::WarmupCache(std::chrono::seconds retry_after, std::size_t capacity)
    : retry_after_(retry_after), capacity_(std::max<std::size_t>(1, capacity)) {}

WarmupResult WarmupCache::get(const WarmupRequest& request, const HttpsFetch& fetch,
                              KlineClock::time_point now) {
    const auto key = key_for(request);
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = entries_.find(key);
    if (found != entries_.end()) {
        // A window's bars are closed and immutable, so a success is the
        // answer for good. A failure is worth trying again, but not on every
        // tick of the daemon loop.
        if (found->second.result.ok()) return found->second.result;
        if (now - found->second.attempted < retry_after_) return found->second.result;
    }

    auto result = warm_window(request, fetch, now);
    entries_[key] = Entry{result, now};
    // Keyed by symbol then window start in milliseconds, which is fixed
    // width for the next two centuries - so for one symbol this drops the
    // oldest window first, and the window just fetched is never the one
    // dropped.
    while (entries_.size() > capacity_) entries_.erase(entries_.begin());
    return result;
}

}  // namespace cpm::strategy
