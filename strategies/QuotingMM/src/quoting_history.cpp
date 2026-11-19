#include "cpm/strategy/quoting_history.hpp"

#include "cpm/common/json.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <stdexcept>

namespace cpm::strategy {
namespace {
using namespace std::chrono;
using Type = JsonValue::Type;

std::string utc(PricingSecond second) {
    const auto day = floor<days>(second);
    const year_month_day date{day};
    const hh_mm_ss time{second - day};
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02u-%02uT%02d:%02d:%02dZ",
                  int(date.year()), unsigned(date.month()), unsigned(date.day()),
                  int(time.hours().count()), int(time.minutes().count()), int(time.seconds().count()));
    return buffer;
}

bool matches(const JsonValue& object, std::string_view key, std::string_view expected) {
    const auto* field = object.find(key);
    return field && field->type == Type::String && field->text == expected;
}

bool flag(const JsonValue& object, std::string_view key) {
    const auto* field = object.find(key);
    return field && field->type == Type::Bool && field->boolean;
}

double number(const JsonValue& object, std::string_view key, double minimum = 0) {
    const auto* field = object.find(key);
    double value = 0;
    if (field && field->type == Type::Number) {
        const auto* end = field->text.data() + field->text.size();
        const auto parsed = std::from_chars(field->text.data(), end, value);
        if (parsed.ec == std::errc{} && parsed.ptr == end && std::isfinite(value) && value >= minimum)
            return value;
    }
    throw std::runtime_error("missing or invalid history field " + std::string(key));
}

double price(const JsonValue& object, std::string_view key) {
    const double value = number(object, key);
    if (value <= 0) throw std::runtime_error("nonpositive history price " + std::string(key));
    return value;
}

const JsonValue& field(const JsonValue& object, std::string_view key, Type type) {
    const auto* value = object.find(key);
    if (!value || value->type != type)
        throw std::runtime_error("missing or invalid history field " + std::string(key));
    return *value;
}

JsonValue get(const HttpsFetch& fetch, const HttpsRequest& request) {
    const auto response = fetch(request);
    if (!response.ok()) {
        throw std::runtime_error(request.path + (response.error.empty()
            ? " returned HTTP " + std::to_string(response.status) : ": " + response.error));
    }
    auto parsed = json_parse(response.body);
    if (!parsed.ok() || !parsed.value.is_object())
        throw std::runtime_error(request.path + " returned invalid JSON");
    return std::move(parsed.value);
}
}  // namespace

HistoryResult fetch_pricing_history(HttpsRequest request, const HttpsFetch& fetch,
                                   const std::string& symbol, PricingSecond as_of,
                                   PricingSecond window_open, PricingSecond final_start) {
    try {
        if (symbol.empty() || symbol.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") !=
                                  std::string::npos)
            throw std::runtime_error("invalid Binance symbol");
        const auto prefix = request.path;
        request.path = prefix + "/v1/features";
        request.query = "symbol=" + symbol + "&as_of=" + utc(as_of) + "&window_open=" + utc(window_open);
        const auto payload = get(fetch, request);
        if (!matches(payload, "feature_version", "mid-boundary-v1") ||
            !matches(payload, "symbol", symbol) || !matches(payload, "as_of", utc(as_of)) ||
            !flag(payload, "history_complete"))
            throw std::runtime_error("historical features are incomplete or do not match the request");

        PricingHistory state;
        const auto& strike = field(payload, "reference", Type::Object);
        if (!matches(strike, "window_open", utc(window_open)) || !flag(strike, "valid") ||
            number(strike, "valid_sample_count") != 60)
            throw std::runtime_error("pre-window strike requires 60 valid samples");
        state.strike = price(strike, "price");
        state.last_second = as_of - seconds(1);
        if (!matches(payload, "last_second_timestamp", utc(state.last_second)) ||
            !matches(payload, "fast_variance_as_of", utc(state.last_second)))
            throw std::runtime_error("invalid last sample or EWMA timestamp");
        state.last_mid = price(payload, "last_second_mid");
        state.fast_variance = number(payload, "fast_variance");
        const auto age = number(payload, "max_quote_age_seconds");
        state.max_age = duration<double>(age);
        const auto minute = floor<minutes>(as_of);
        const auto& variances = field(payload, "gk_minute_variances", Type::Array).items;
        if (variances.size() != 120) throw std::runtime_error("expected 120 minute variances");
        auto expected = minute - minutes(120);
        for (const auto& candle : variances) {
            if (!matches(candle, "minute_start", utc(expected)) ||
                number(candle, "valid_sample_count") != 60)
                throw std::runtime_error("incomplete minute at " + utc(expected));
            state.minute_variances.push_back(number(candle, "variance"));
            expected += minutes(1);
        }
        state.minute_count = static_cast<int>((as_of - minute).count());
        if (state.minute_count > 0) {
            const auto& partial = field(payload, "partial_minute", Type::Object);
            if (!matches(partial, "minute_start", utc(minute)) ||
                number(partial, "valid_sample_count") != state.minute_count)
                throw std::runtime_error("incomplete partial minute");
            state.open = price(partial, "open");
            state.high = price(partial, "high");
            state.low = price(partial, "low");
            state.close = price(partial, "close");
            if (state.low > std::min(state.open, state.close) ||
                state.high < std::max(state.open, state.close) || state.close != state.last_mid)
                throw std::runtime_error("inconsistent partial minute");
        }

        if (as_of > final_start) {
            request.path = prefix + "/v1/midpoints";
            request.query = "symbol=" + symbol + "&start=" + utc(final_start) + "&end=" + utc(as_of);
            const auto lookup = get(fetch, request);
            const auto& samples = field(lookup, "samples", Type::Array).items;
            if (!matches(lookup, "symbol", symbol) ||
                !matches(lookup, "start", utc(final_start)) ||
                !matches(lookup, "end_exclusive", utc(as_of)) ||
                samples.size() != static_cast<std::size_t>((as_of - final_start).count()))
                throw std::runtime_error("incomplete settlement history");
            auto second = final_start;
            for (const auto& sample : samples) {
                if (!matches(sample, "time", utc(second)) ||
                    number(sample, "quote_age_seconds") > age)
                    throw std::runtime_error("invalid settlement sample at " + utc(second));
                const auto mid = price(sample, "mid");
                if (second == state.last_second && mid != state.last_mid)
                    throw std::runtime_error("settlement and feature history disagree at " + utc(second));
                state.settlement_sum += mid;
                ++state.settlement_count;
                ++second;
            }
        }
        return {std::move(state), {}};
    } catch (const std::exception& e) {
        return {{}, e.what()};
    }
}

void PricingHistory::add(PricingSecond second, double mid, PricingSecond final_start) {
    static const double alpha = 1 - std::exp2(-1.0 / 120);
    const double change = std::log(mid / last_mid);
    fast_variance += alpha * (change * change - fast_variance);
    last_mid = mid;
    last_second = second;
    if (minute_count == 0) open = high = low = mid;
    high = std::max(high, mid);
    low = std::min(low, mid);
    close = mid;
    if (++minute_count == 60) {
        const double range = std::log(high / low);
        const double move = std::log(close / open);
        minute_variances.push_back(0.5 * range * range - (2 * std::log(2.0) - 1) * move * move);
        minute_variances.pop_front();
        minute_count = 0;
    }
    if (second >= final_start) {
        settlement_sum += mid;
        ++settlement_count;
    }
}

double PricingHistory::sigma() const {
    if (minute_variances.size() != 120) return 0;
    const double fast_window = std::accumulate(minute_variances.end() - 20, minute_variances.end(), 0.0) / 20;
    const double slow_window = std::accumulate(minute_variances.begin(), minute_variances.end(), 0.0) / 120;
    const double gk = std::max(fast_window, 0.5625 * slow_window) / 60;
    return std::sqrt(0.5 * (gk + fast_variance));
}

std::optional<double> PricingHistory::z(double spot, double remaining, int elapsed,
                                       double correction, double offset) const {
    const double vol = sigma();
    if (!(vol > 0) || !std::isfinite(vol) || !(spot > 0) || !(strike > 0) ||
        !(correction > 0) || remaining <= 0)
        return std::nullopt;
    if (remaining > 60)
        return (std::log(spot / strike) + offset) / (correction * vol * std::sqrt(remaining - 40));
    const int current_sample = settlement_count - elapsed;
    if (elapsed < 0 || elapsed >= 60 || current_sample < 0 || current_sample > 1)
        return std::nullopt;
    const double known = settlement_sum - current_sample * last_mid;
    const double r = 60 - elapsed;
    const double average = (known + r * spot) / 60;
    return std::log(average / strike) / (correction * vol * (r / 60) * std::sqrt(r / 3));
}

}  // namespace cpm::strategy
