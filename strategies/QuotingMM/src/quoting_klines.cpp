#include "cpm/strategy/quoting_klines.hpp"

#include "cpm/common/fixed.hpp"
#include "cpm/common/json.hpp"

#include <algorithm>
#include <charconv>

namespace cpm::strategy {
namespace {

constexpr int kVenueMaxLimit = 1500;

// The venue's own millisecond epoch. Kept as an integer the whole way.
std::optional<KlineClock::time_point> millis_at(const cpm::JsonValue& value) {
    if (value.type != cpm::JsonValue::Type::Number) return std::nullopt;
    std::int64_t ms = 0;
    const char* begin = value.text.data();
    const char* end = begin + value.text.size();
    const auto result = std::from_chars(begin, end, ms);
    if (result.ec != std::errc{} || result.ptr != end || ms < 0) return std::nullopt;
    return KlineClock::time_point(std::chrono::milliseconds(ms));
}

std::optional<std::int64_t> count_of(const cpm::JsonValue& value) {
    if (value.type != cpm::JsonValue::Type::Number) return std::nullopt;
    std::int64_t n = 0;
    const char* begin = value.text.data();
    const char* end = begin + value.text.size();
    const auto result = std::from_chars(begin, end, n);
    if (result.ec != std::errc{} || result.ptr != end || n < 0) return std::nullopt;
    return n;
}

// Prices and sizes arrive as decimal strings, never as JSON numbers - which
// is the venue doing the right thing, and is why they can stay integers.
std::optional<std::int64_t> scaled_of(const cpm::JsonValue& value) {
    if (value.type != cpm::JsonValue::Type::String) return std::nullopt;
    return cpm::parse_scaled(value.text, kKlineScale);
}

std::string row_error(std::size_t index, const std::string& what) {
    return "kline " + std::to_string(index) + " " + what;
}

bool valid_symbol(std::string_view symbol) {
    if (symbol.empty() || symbol.size() > 32) return false;
    for (const char c : symbol) {
        const bool upper = c >= 'A' && c <= 'Z';
        const bool digit = c >= '0' && c <= '9';
        if (!upper && !digit) return false;
    }
    return true;
}

}  // namespace

int kline_interval_minutes(std::string_view interval) {
    if (interval.size() < 2) return 0;
    const char unit = interval.back();
    const auto digits = interval.substr(0, interval.size() - 1);
    int count = 0;
    const char* begin = digits.data();
    const char* end = begin + digits.size();
    const auto result = std::from_chars(begin, end, count);
    if (result.ec != std::errc{} || result.ptr != end || count <= 0) return 0;

    int per_unit = 0;
    switch (unit) {
        case 'm': per_unit = 1; break;        // minutes; 'M' (a month) is not this
        case 'h': per_unit = 60; break;
        case 'd': per_unit = 60 * 24; break;
        case 'w': per_unit = 60 * 24 * 7; break;
        default: return 0;
    }
    // Only the intervals the venue actually serves. Anything else would be a
    // request that 400s in production and passed in a test.
    const int minutes = count * per_unit;
    switch (minutes) {
        case 1: case 3: case 5: case 15: case 30:
        case 60: case 120: case 240: case 360: case 480: case 720:
        case 1440: case 4320: case 10080:
            return minutes;
        default:
            return 0;
    }
}

int kline_request_weight(int limit) {
    if (limit <= 100) return 1;
    if (limit <= 500) return 2;
    if (limit <= 1000) return 5;
    return 10;
}

std::optional<int> kline_limit(const KlineRequest& request, std::string& error) {
    error.clear();
    const int per_bar = kline_interval_minutes(request.interval);
    if (per_bar == 0) {
        error = "\"" + request.interval +
                "\" is not a futures kline interval (1m 3m 5m 15m 30m 1h 2h 4h 6h 8h 12h 1d 3d 1w)";
        return std::nullopt;
    }
    if (request.limit > 0) {
        if (request.limit > kVenueMaxLimit) {
            error = "the venue returns at most " + std::to_string(kVenueMaxLimit) +
                    " bars in one call, and " + std::to_string(request.limit) + " were asked for";
            return std::nullopt;
        }
        return request.limit;
    }
    if (request.minutes <= 0) {
        error = "a lookback of " + std::to_string(request.minutes) + " minutes asks for nothing";
        return std::nullopt;
    }
    // Round up: 90 minutes of 1h bars is two bars, not one.
    std::int64_t bars = (static_cast<std::int64_t>(request.minutes) + per_bar - 1) / per_bar;
    // One more, always. A `limit` query answers with the bar that is still
    // forming at the head of it, whatever the caller means to do with it, so
    // n minutes of *completed* history is n+1 bars asked for - and dropping
    // the forming one then leaves exactly the n that were wanted.
    bars += 1;
    if (bars > kVenueMaxLimit) {
        error = std::to_string(request.minutes) + " minutes of " + request.interval + " bars is " +
                std::to_string(bars) + " bars, and the venue returns at most " +
                std::to_string(kVenueMaxLimit) + " in one call";
        return std::nullopt;
    }
    return static_cast<int>(bars);
}

std::string kline_query(const KlineRequest& request, std::string& error) {
    error.clear();
    if (!valid_symbol(request.symbol)) {
        error = request.symbol.empty()
                    ? std::string("no symbol to ask about")
                    : "\"" + request.symbol + "\" is not a Binance symbol (BTCUSDT, ETHUSDT)";
        return {};
    }
    const auto limit = kline_limit(request, error);
    if (!limit) return {};
    // Every part of this is already known to be [A-Z0-9] or digits, so there
    // is nothing here to percent-encode.
    std::string query = "symbol=" + request.symbol + "&interval=" + request.interval +
                        "&limit=" + std::to_string(*limit);
    if (request.end_time) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            request.end_time->time_since_epoch());
        if (ms.count() <= 0) {
            error = "an end time before the epoch is not a bar the venue has";
            return {};
        }
        query += "&endTime=" + std::to_string(ms.count());
    }
    return query;
}

KlineSeries parse_klines(std::string_view body, KlineClock::time_point now) {
    KlineSeries out;
    const auto parsed = cpm::json_parse(body);
    if (!parsed.ok()) {
        out.error = "the venue's answer is not JSON: " + parsed.error;
        return out;
    }
    if (parsed.value.type != cpm::JsonValue::Type::Array) {
        out.error = "the venue's answer is not an array of klines";
        return out;
    }

    out.bars.reserve(parsed.value.items.size());
    for (std::size_t i = 0; i < parsed.value.items.size(); ++i) {
        const auto& row = parsed.value.items[i];
        // openTime, open, high, low, close, volume, closeTime, quoteVolume,
        // trades, takerBuyBase, takerBuyQuote, ignore. Trailing fields are
        // read when they are there and never required: the venue has added
        // to the end of this row before.
        if (row.type != cpm::JsonValue::Type::Array || row.items.size() < 8) {
            out.error = row_error(i, "is not a row of at least 8 fields");
            return {std::vector<Kline>{}, out.error, 0};
        }
        Kline bar;
        const auto open_time = millis_at(row.items[0]);
        const auto close_time = millis_at(row.items[6]);
        const auto open = scaled_of(row.items[1]);
        const auto high = scaled_of(row.items[2]);
        const auto low = scaled_of(row.items[3]);
        const auto close = scaled_of(row.items[4]);
        const auto volume = scaled_of(row.items[5]);
        const auto quote_volume = scaled_of(row.items[7]);
        if (!open_time || !close_time) {
            out.error = row_error(i, "has no usable open or close time");
            return {std::vector<Kline>{}, out.error, 0};
        }
        if (!open || !high || !low || !close || !volume || !quote_volume) {
            out.error = row_error(i, "has a price or size this build cannot read");
            return {std::vector<Kline>{}, out.error, 0};
        }
        if (*open <= 0 || *close <= 0 || *low <= 0 || *high < std::max(*open, *close) ||
            *low > std::min(*open, *close)) {
            out.error = row_error(i, "is not a well-formed bar: high and low do not contain "
                                     "open and close");
            return {std::vector<Kline>{}, out.error, 0};
        }
        if (*close_time <= *open_time) {
            out.error = row_error(i, "closes before it opens");
            return {std::vector<Kline>{}, out.error, 0};
        }
        if (!out.bars.empty() && *open_time <= out.bars.back().open_time) {
            out.error = row_error(i, "is not newer than the bar before it");
            return {std::vector<Kline>{}, out.error, 0};
        }
        bar.open_time = *open_time;
        bar.close_time = *close_time;
        bar.open = *open;
        bar.high = *high;
        bar.low = *low;
        bar.close = *close;
        bar.volume = *volume;
        bar.quote_volume = *quote_volume;
        if (row.items.size() > 8) {
            if (const auto trades = count_of(row.items[8])) bar.trades = *trades;
        }
        // The venue's closeTime is the bar's last millisecond, so a bar is
        // over once the clock has passed it.
        bar.closed = bar.close_time < now;
        out.bars.push_back(bar);
    }
    return out;
}

KlineSeries fetch_klines(const KlineRequest& request, const HttpsFetch& fetch,
                         KlineClock::time_point now) {
    KlineSeries out;
    std::string error;
    // One validation, in one place: the query builder checks the symbol and
    // works out the bar count, and refuses rather than guessing.
    const auto query = kline_query(request, error);
    if (!error.empty()) {
        out.error = error;
        return out;
    }
    std::string ignored;
    out.weight = kline_request_weight(kline_limit(request, ignored).value_or(0));

    cpm::HttpsRequest http;
    http.host = request.host;
    http.port = request.port;
    http.path = request.path;
    http.query = query;
    http.ca_file = request.ca_file;
    http.timeout = request.timeout;
    // No key, no signature: /fapi/v1/klines is public market data.
    http.headers = {{"Accept", "application/json"}};

    const auto response = fetch(http);
    if (!response.error.empty()) {
        out.error = "could not reach " + request.host + ": " + response.error;
        return out;
    }
    if (response.status != 200) {
        // Binance answers an error as {"code":-1121,"msg":"Invalid symbol."},
        // which says more than the status does - but the body is unbounded,
        // so only the start of it.
        std::string detail = response.body.substr(0, 300);
        const auto parsed = cpm::json_parse(response.body);
        if (parsed.ok()) {
            const auto* msg = parsed.value.find("msg");
            const auto* code = parsed.value.find("code");
            if (msg != nullptr && msg->is_string()) {
                detail = msg->text;
                if (code != nullptr && code->type == cpm::JsonValue::Type::Number) {
                    detail += " (code " + code->text + ")";
                }
            }
        }
        out.error = request.host + " answered HTTP " + std::to_string(response.status) +
                    (detail.empty() ? "" : ": " + detail);
        return out;
    }

    const auto weight = out.weight;
    out = parse_klines(response.body, now);
    out.weight = weight;
    if (!out.ok()) return out;
    if (!request.include_unclosed && !out.bars.empty() && !out.bars.back().closed) {
        out.bars.pop_back();
    }
    return out;
}

}  // namespace cpm::strategy
