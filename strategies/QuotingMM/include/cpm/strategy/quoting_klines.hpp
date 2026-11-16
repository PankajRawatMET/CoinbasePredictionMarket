#pragma once

#include "cpm/common/https.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Binance USD-M futures klines over REST: the last n minutes of OHLC for a
// perpetual, as completed bars.
//
// Standalone on purpose. Nothing in QuotingMM calls it yet: the strategy's
// volatility is built from one-second samples of the live bookTicker stream
// (quoting_model.hpp), and REST cannot go below one minute on futures. This
// is the piece that can be pointed at the venue and read, so the two can be
// compared before either depends on the other.
//
// It reaches the venue through cpm::https_get - the same one-GET client the
// instrument catalog uses - and signs nothing: /fapi/v1/klines is public
// market data and takes no key.
//
// Note for whoever wires this in: docs/open-questions-binance.txt B-Q17 says
// this project makes no REST call to Binance at all, and config/binance.conf
// repeats it. That answer is still what the reference feed does; this module
// is the first thing in the tree that would change it, exactly as [MD] Q13's
// "no REST" was reversed for the instrument catalog.
namespace cpm::strategy {

using KlineClock = std::chrono::system_clock;

// Binance sends futures prices and sizes as decimal strings with up to eight
// decimals. They stay integers from here on: no price in this project passes
// through a double (B-Q12).
inline constexpr int kKlineScale = 8;

// One bar, as the venue draws it.
struct Kline {
    KlineClock::time_point open_time{};
    // The venue's closeTime: the last millisecond of the bar, not the first
    // of the next one. A 1m bar opened at 12:00:00.000 closes 12:00:59.999.
    KlineClock::time_point close_time{};
    std::int64_t open = 0;
    std::int64_t high = 0;
    std::int64_t low = 0;
    std::int64_t close = 0;
    std::int64_t volume = 0;        // base asset (BTC), scaled
    std::int64_t quote_volume = 0;  // quote asset (USDT), scaled
    std::int64_t trades = 0;
    // The bar has ended. The newest bar a `limit` query returns is normally
    // still forming, and its close is a snapshot of right now rather than a
    // settled price - which matters to anything measuring returns.
    bool closed = false;
};

struct KlineRequest {
    // A USD-M perpetual as the venue spells it: BTCUSDT, ETHUSDT. Upper case
    // alphanumerics; the venue 400s on anything else.
    std::string symbol;
    // 1m 3m 5m 15m 30m 1h 2h 4h 6h 8h 12h 1d 3d 1w. Case matters: 1M is a
    // month and is not supported here, because a month is not a lookback
    // this strategy has any use for.
    std::string interval = "1m";
    // How far back to ask, in minutes. `limit` is derived from it.
    int minutes = 60;
    // Override the derived bar count. 0 means derive it; the venue caps it
    // at 1500 and so does this.
    int limit = 0;
    // The newest bar to return, by its open time. Unset asks for the bars
    // ending now, which is what a "last n minutes" question wants; set, the
    // answer ends at a boundary in the past, which is what a window that
    // opened at 06:00 needs. The venue's endTime is inclusive of a bar whose
    // open time equals it, so this names the last bar wanted, not the first
    // one excluded. `limit` then counts back from it.
    std::optional<KlineClock::time_point> end_time;
    // Keep the bar that is still forming. False drops it, which is what
    // anything computing returns wants. It does not change what is asked
    // for: the query always covers `minutes` of completed bars plus the
    // forming one, so dropping it still leaves `minutes` of history.
    bool include_unclosed = true;

    // Where the venue is. Defaulted rather than written into the code at the
    // point of use (B-Q15); an operator can point this at a test endpoint.
    // Production USD-M futures is fapi.binance.com; testnet is
    // testnet.binancefuture.com.
    std::string host = "fapi.binance.com";
    int port = 443;
    std::string path = "/fapi/v1/klines";
    std::string ca_file;  // empty: the system trust store
    std::chrono::seconds timeout{15};
};

struct KlineSeries {
    // Oldest first, as the venue sends them.
    std::vector<Kline> bars;
    std::string error;  // empty on success, worded for an operator
    // What the call cost against the venue's 2400/minute IP budget, so a
    // caller that ever polls can be held to account for it.
    int weight = 0;

    bool ok() const { return error.empty(); }
};

// Minutes in one bar of `interval`, or 0 for a spelling this does not know.
int kline_interval_minutes(std::string_view interval);

// The documented weight of /fapi/v1/klines, which is charged by limit:
// 1 up to 100 bars, 2 up to 500, 5 up to 1000, 10 beyond.
int kline_request_weight(int limit);

// How many bars `request` asks for once minutes, interval and the venue's cap
// are taken together. Nullopt when the request is not answerable, with the
// reason in `error`.
std::optional<int> kline_limit(const KlineRequest& request, std::string& error);

// symbol=BTCUSDT&interval=1m&limit=61, or an error. Exposed because the URL
// handling deserves a test that needs no network.
std::string kline_query(const KlineRequest& request, std::string& error);

// The venue's array-of-arrays body. Exposed for the same reason: this is the
// whole of the parsing, and it is testable from a captured payload.
KlineSeries parse_klines(std::string_view body, KlineClock::time_point now);

// The HTTP call, injected so everything above can run without a socket.
using HttpsFetch = std::function<cpm::HttpsResponse(const cpm::HttpsRequest&)>;

// One GET. Never throws: a failure is a series with an error, because the
// caller has to be able to say why it has no history.
KlineSeries fetch_klines(const KlineRequest& request, const HttpsFetch& fetch = cpm::https_get,
                         KlineClock::time_point now = KlineClock::now());

}  // namespace cpm::strategy
