#pragma once

#include "cpm/strategy/quoting_klines.hpp"

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// What a QuotingMM instance needs before it can price anything, fetched once
// for the window it is about to quote:
//
//   opening - the underlying's price when the window opened, which is the
//             denominator of log(spot/opening)
//   sigma   - the underlying's volatility over the twenty minutes before the
//             window opened, per sqrt(second)
//
// Both come from one REST call for the bars around the window's start, so a
// strategy created at 06:12 on the 06:00 window prices off exactly what a
// strategy created at 06:00:02 would have (the bars it reads are closed and
// never change). Neither is recomputed while the window runs.
//
// This replaces warming sigma from the live one-second stream, which needed
// twenty uninterrupted minutes before the strategy could quote at all and
// started that clock again after any gap.
namespace cpm::strategy {

struct WindowWarmup {
    std::int64_t opening = 0;  // the window-start bar's open, at kKlineScale
    double sigma = 0;          // per sqrt(second)
    int returns = 0;           // how many bar returns sigma was taken over
    KlineClock::time_point window_start{};
    std::string symbol;    // BTCUSDT
    std::string interval;  // 1m
    int lookback_minutes = 0;

    // "20m of 1m klines to 2026-11-16 06:00:00Z", for the operator's row.
    std::string describe() const;
};

struct WarmupResult {
    WindowWarmup warmup;
    std::string error;  // empty on success, worded for an operator

    bool ok() const { return error.empty(); }
};

struct WarmupRequest {
    std::string symbol;  // the Binance perpetual, not the CPM instrument
    KlineClock::time_point window_start{};
    int lookback_minutes = 20;
    std::string interval = "1m";

    std::string host = "fapi.binance.com";
    int port = 443;
    std::string ca_file;
    std::chrono::seconds timeout{15};
};

// The arithmetic, on bars already in hand. Strict about what it is given:
// the bars must run contiguously up to and including the window's own bar,
// and every bar before the window must have closed. A gap is refused rather
// than stepped over, because a sigma taken across an outage is a smaller
// number than the truth and would quote tighter for it.
WarmupResult warmup_from_bars(const std::vector<Kline>& bars, const WarmupRequest& request,
                              KlineClock::time_point now);

// The same, having fetched the bars. One call, weight 1.
WarmupResult warm_window(const WarmupRequest& request, const HttpsFetch& fetch = cpm::https_get,
                         KlineClock::time_point now = KlineClock::now());

// One fetch per window, however often it is asked for.
//
// This exists because of where the call happens. A strategy is built at
// create and again at every roll, a failed roll is retried on the daemon's
// next tick (trader.cpp), and the fetch blocks for up to its timeout - so
// without this, one unreachable venue would put a blocking HTTPS call into
// every tick of the daemon loop. A success is kept for as long as the window
// is interesting, and a failure is not retried more often than `retry_after`.
class WarmupCache {
public:
    explicit WarmupCache(std::chrono::seconds retry_after = std::chrono::seconds(2),
                         std::size_t capacity = 16);

    WarmupResult get(const WarmupRequest& request, const HttpsFetch& fetch,
                     KlineClock::time_point now = KlineClock::now());

private:
    struct Entry {
        WarmupResult result;
        KlineClock::time_point attempted{};
    };

    std::chrono::seconds retry_after_;
    std::size_t capacity_;
    std::mutex mutex_;
    std::map<std::string, Entry> entries_;
};

}  // namespace cpm::strategy
