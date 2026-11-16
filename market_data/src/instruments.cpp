#include "cpm/md/instruments.hpp"

#include "cpm/common/json.hpp"
#include "cpm/common/signing.hpp"
#include "cpm/md/ticker.hpp"

#include <algorithm>
#include <chrono>

namespace cpm::md {
namespace {

// The venue's PriceLevelStructure spelling, which is not ours.
// Milliseconds since the epoch, as every timestamp in the catalog is written.
std::optional<Clock::time_point> from_millis(const cpm::JsonValue* value) {
    if (value == nullptr || value->type != cpm::JsonValue::Type::Number) return std::nullopt;
    try {
        return Clock::time_point(std::chrono::milliseconds(std::stoll(value->text)));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

const std::string* text_of(const cpm::JsonValue& object, const char* name) {
    const auto* field = object.find(name);
    if (field == nullptr || field->type != cpm::JsonValue::Type::String) return nullptr;
    return &field->text;
}

}  // namespace

RestEndpoint parse_rest_url(const std::string& url) {
    RestEndpoint out;
    std::string rest = url;
    while (!rest.empty() && rest.back() == '/') rest.pop_back();

    if (rest.rfind("https://", 0) == 0) {
        rest = rest.substr(8);
    } else if (rest.rfind("http://", 0) == 0) {
        // Credentials go on this connection; it is not negotiable.
        out.error = "the REST URL must be https: " + url;
        return out;
    } else if (rest.empty()) {
        out.error = "the REST URL is empty: set CPM_REST_URL";
        return out;
    }

    const auto slash = rest.find('/');
    auto authority = slash == std::string::npos ? rest : rest.substr(0, slash);
    auto path = slash == std::string::npos ? std::string() : rest.substr(slash);
    while (!path.empty() && path.back() == '/') path.pop_back();

    const auto colon = authority.find(':');
    if (colon != std::string::npos) {
        try {
            out.port = std::stoi(authority.substr(colon + 1));
        } catch (const std::exception&) {
            out.error = "the REST URL has a port that is not a number: " + url;
            return out;
        }
        authority = authority.substr(0, colon);
    }
    if (authority.empty()) {
        out.error = "the REST URL has no host: " + url;
        return out;
    }
    out.host = authority;

    // The spec's endpoints are written /api/v1/...; accept a base given with
    // none of it, with /api, or with the whole thing already on.
    if (path.size() >= 7 && path.compare(path.size() - 7, 7, "/api/v1") == 0) {
        out.prefix = path;
    } else if (path.size() >= 4 && path.compare(path.size() - 4, 4, "/api") == 0) {
        out.prefix = path + "/v1";
    } else {
        out.prefix = path + "/api/v1";
    }
    return out;
}

std::string rest_signature(const std::string& secret_b64, const std::string& timestamp,
                           const std::string& method, const std::string& path) {
    // "Concatenate timestamp + HTTP method + request path + request body. Do
    // not include query parameters. GET requests use an empty request body."
    return hmac_sha256_b64(secret_b64, timestamp + method + path);
}

InstrumentQuery open_instruments(const RestConfig& config, const std::string& series,
                                 const HttpsFetch& fetch, Clock::time_point now) {
    InstrumentQuery out;
    if (!config.configured()) {
        out.error =
            "REST is not configured, so a series cannot be resolved to the instrument trading "
            "now: set CPM_REST_API_KEY, CPM_REST_PASSPHRASE, CPM_REST_SECRET and CPM_REST_URL "
            "(creds-rest.env)";
        return out;
    }
    if (series.empty()) {
        out.error = "no series to look up";
        return out;
    }

    const auto endpoint = parse_rest_url(config.url);
    if (!endpoint.error.empty()) {
        out.error = endpoint.error;
        return out;
    }

    const auto path = endpoint.prefix + "/instruments";
    const auto timestamp = std::to_string(
        std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count());

    cpm::HttpsRequest request;
    request.host = endpoint.host;
    request.port = endpoint.port;
    request.path = path;
    request.query = "series_ticker=" + series + "&status=OPEN";
    request.ca_file = config.ca_file;
    request.timeout = config.timeout;
    request.headers = {
        {"CB-ACCESS-KEY", config.api_key},
        {"CB-ACCESS-SIGN", rest_signature(config.secret_b64, timestamp, "GET", path)},
        {"CB-ACCESS-PASSPHRASE", config.passphrase},
        {"CB-ACCESS-TIMESTAMP", timestamp},
        {"Accept", "application/json"},
    };

    const auto response = fetch(request);
    if (!response.error.empty()) {
        out.error = "could not reach the instrument catalog: " + response.error;
        return out;
    }
    if (response.status != 200) {
        // The body carries the venue's own reason, which is more use than the
        // status alone - but it is unbounded, so only the start of it.
        auto detail = response.body.substr(0, 300);
        out.error = "the instrument catalog answered HTTP " + std::to_string(response.status) +
                    (detail.empty() ? "" : ": " + detail);
        return out;
    }

    const auto parsed = cpm::json_parse(response.body);
    if (!parsed.ok()) {
        out.error = "the instrument catalog's answer is not JSON: " + parsed.error;
        return out;
    }
    const auto* data = parsed.value.find("data");
    if (data == nullptr || data->type != cpm::JsonValue::Type::Array) {
        out.error = "the instrument catalog's answer has no \"data\" array";
        return out;
    }

    for (const auto& row : data->items) {
        const auto* ticker = text_of(row, "ticker");
        if (ticker == nullptr || ticker->empty()) continue;
        InstrumentInfo info;
        info.ticker = *ticker;
        if (const auto* id = text_of(row, "id")) info.id = *id;
        if (const auto* status = text_of(row, "status")) info.status = *status;
        if (const auto* owner = text_of(row, "series_ticker")) info.series_ticker = *owner;
        if (const auto* structure = text_of(row, "price_level_structure")) {
            info.price_level_structure = *structure;
        }
        if (const auto when = from_millis(row.find("expected_close_time"))) {
            info.expected_close = *when;
        } else if (const auto parsed_ticker = parse_ticker(info.ticker)) {
            // The catalog is the authority, but a window that does not say
            // when it ends still has its end in its name ([MD] O2).
            info.expected_close = parsed_ticker->window_end;
        }
        // The sandbox can return feeder/test instruments in a rolling-series
        // query (ETHFEEDERTEST has no end and used to sort ahead of every
        // real window at epoch zero). Only dated, live windows of the queried
        // series are candidates for automatic resolution.
        if (series.ends_with("D15M") || series.ends_with("D5M")) {
            const auto window = parse_ticker(info.ticker);
            if (!window || window->series != series || window->window_end <= now ||
                info.expected_close <= now ||
                (!info.series_ticker.empty() && info.series_ticker != series) ||
                (!info.status.empty() && info.status != "OPEN")) continue;
        }
        out.instruments.push_back(std::move(info));
    }

    // Soonest to end first, which for a rolling series is the one trading now.
    std::sort(out.instruments.begin(), out.instruments.end(),
              [](const InstrumentInfo& a, const InstrumentInfo& b) {
                  return a.expected_close < b.expected_close;
              });
    return out;
}

}  // namespace cpm::md
