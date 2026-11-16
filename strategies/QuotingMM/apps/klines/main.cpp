// binance_klines: the last n minutes of USD-M futures OHLC, printed.
//
// A reading tool, not part of the daemon. It exists so the REST history can
// be looked at next to what the live stream builds, before anything in
// QuotingMM depends on it.
//
//   binance_klines --symbol BTCUSDT --minutes 60
//   binance_klines --symbol ETHUSDT --interval 5m --minutes 240 --closed-only
//   binance_klines --symbol BTCUSDT --minutes 30 --csv > bars.csv
#include "cpm/common/fixed.hpp"
#include "cpm/strategy/quoting_klines.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using cpm::strategy::Kline;
using cpm::strategy::KlineRequest;

void usage() {
    std::cout <<
        "binance_klines - last n minutes of Binance USD-M futures OHLC\n"
        "\n"
        "  --symbol SYMBOL    perpetual, as the venue spells it (default BTCUSDT)\n"
        "  --interval I       1m 3m 5m 15m 30m 1h 2h 4h 6h 8h 12h 1d 3d 1w (default 1m)\n"
        "  --minutes N        how far back to ask (default 60)\n"
        "  --limit N          bar count, instead of deriving it from --minutes\n"
        "  --closed-only      drop the bar that is still forming\n"
        "  --host HOST        default fapi.binance.com; testnet.binancefuture.com for testnet\n"
        "  --port N           default 443\n"
        "  --ca-file PATH     CA bundle; default is the system trust store\n"
        "  --csv              machine-readable output\n"
        "  --help\n";
}

bool to_int(std::string_view text, int& out) {
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto result = std::from_chars(begin, end, out);
    return result.ec == std::errc{} && result.ptr == end;
}

std::string utc(cpm::strategy::KlineClock::time_point when) {
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(when.time_since_epoch());
    const std::time_t raw = static_cast<std::time_t>(seconds.count());
    std::tm parts{};
    gmtime_r(&raw, &parts);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &parts);
    return buffer;
}

std::string price(std::int64_t scaled) {
    return cpm::format_scaled(scaled, cpm::strategy::kKlineScale);
}

// A diagnostic, and the reason to print it next to the bars: the sample
// standard deviation of log returns between closes, rescaled per second, is
// the same quantity QuotingMM builds from one-second stream samples. Doubles
// here are fine - nothing is priced off this, it is something to compare.
void print_volatility(const std::vector<Kline>& bars, int bar_minutes) {
    if (bars.size() < 3) return;
    std::vector<double> returns;
    returns.reserve(bars.size() - 1);
    for (std::size_t i = 1; i < bars.size(); ++i) {
        const double previous = static_cast<double>(bars[i - 1].close);
        const double current = static_cast<double>(bars[i].close);
        if (previous > 0 && current > 0) returns.push_back(std::log(current / previous));
    }
    if (returns.size() < 2) return;
    double mean = 0;
    for (const double r : returns) mean += r;
    mean /= static_cast<double>(returns.size());
    double sum_squares = 0;
    for (const double r : returns) sum_squares += (r - mean) * (r - mean);
    const double per_bar = std::sqrt(sum_squares / static_cast<double>(returns.size() - 1));
    const double per_root_second = per_bar / std::sqrt(static_cast<double>(bar_minutes) * 60.0);
    std::printf("sigma   %.3e per sqrt(second), from %zu bar returns (%.3e per bar)\n",
                per_root_second, returns.size(), per_bar);
}

}  // namespace

int main(int argc, char** argv) {
    KlineRequest request;
    request.symbol = "BTCUSDT";
    bool csv = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << what << " needs a value\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--help" || arg == "-h") {
            usage();
            return 0;
        } else if (arg == "--symbol") {
            request.symbol = next("--symbol");
        } else if (arg == "--interval") {
            request.interval = next("--interval");
        } else if (arg == "--minutes") {
            if (!to_int(next("--minutes"), request.minutes)) {
                std::cerr << "--minutes takes a whole number\n";
                return 2;
            }
        } else if (arg == "--limit") {
            if (!to_int(next("--limit"), request.limit)) {
                std::cerr << "--limit takes a whole number\n";
                return 2;
            }
        } else if (arg == "--closed-only") {
            request.include_unclosed = false;
        } else if (arg == "--host") {
            request.host = next("--host");
        } else if (arg == "--port") {
            if (!to_int(next("--port"), request.port)) {
                std::cerr << "--port takes a whole number\n";
                return 2;
            }
        } else if (arg == "--ca-file") {
            request.ca_file = next("--ca-file");
        } else if (arg == "--csv") {
            csv = true;
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            usage();
            return 2;
        }
    }

    const auto series = cpm::strategy::fetch_klines(request);
    if (!series.ok()) {
        std::cerr << series.error << "\n";
        return 1;
    }
    if (series.bars.empty()) {
        std::cerr << "no bars returned for " << request.symbol << "\n";
        return 1;
    }

    if (csv) {
        std::cout << "open_time,close_time,open,high,low,close,volume,quote_volume,trades,closed\n";
        for (const auto& bar : series.bars) {
            std::cout << utc(bar.open_time) << ',' << utc(bar.close_time) << ',' << price(bar.open)
                      << ',' << price(bar.high) << ',' << price(bar.low) << ',' << price(bar.close)
                      << ',' << price(bar.volume) << ',' << price(bar.quote_volume) << ','
                      << bar.trades << ',' << (bar.closed ? "1" : "0") << '\n';
        }
        return 0;
    }

    std::printf("%-19s %14s %14s %14s %14s %14s %8s\n", "open (UTC)", "open", "high", "low",
                "close", "volume", "trades");
    std::int64_t low = series.bars.front().low;
    std::int64_t high = series.bars.front().high;
    for (const auto& bar : series.bars) {
        low = std::min(low, bar.low);
        high = std::max(high, bar.high);
        std::printf("%-19s %14s %14s %14s %14s %14s %8lld%s\n", utc(bar.open_time).c_str(),
                    price(bar.open).c_str(), price(bar.high).c_str(), price(bar.low).c_str(),
                    price(bar.close).c_str(), price(bar.volume).c_str(),
                    static_cast<long long>(bar.trades), bar.closed ? "" : "  forming");
    }
    std::printf("\n%zu %s bars of %s, %s to %s\n", series.bars.size(), request.interval.c_str(),
                request.symbol.c_str(), utc(series.bars.front().open_time).c_str(),
                utc(series.bars.back().close_time).c_str());
    std::printf("range   %s - %s\n", price(low).c_str(), price(high).c_str());
    print_volatility(series.bars, cpm::strategy::kline_interval_minutes(request.interval));
    std::printf("weight  %d of the venue's 2400 per minute\n", series.weight);
    return 0;
}
