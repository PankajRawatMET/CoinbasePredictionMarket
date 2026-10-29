#pragma once

#include "cpm/common/https.hpp"
#include "cpm/md/events.hpp"
#include "cpm/md/ticks.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

// The REST instrument catalog: which instrument of a series is open right now.
//
// This is the one question FIX cannot answer. The all-instrument lifecycle
// feed announces the NEXT window when it opens but says nothing about the
// present - it is changes-only, proven in [MD] O6 - and neither FIX session
// defines a SecurityList. So a daemon that starts mid-window has no way to
// resolve BTCD15M without this (docs/open-questions-roll.txt R-Q2).
//
// It is called once per series at startup and on reconnect, never polled.
// REST says what is open now; the lifecycle feed says what just changed.
namespace cpm::md {

struct RestConfig {
    std::string api_key;
    std::string passphrase;
    std::string secret_b64;
    // https://host, with or without /api/v1 - whichever the operator pasted.
    std::string url;
    std::string ca_file;
    std::chrono::seconds timeout{15};

    bool configured() const {
        return !api_key.empty() && !passphrase.empty() && !secret_b64.empty() && !url.empty();
    }
};

// One instrument, as much of it as anything here needs. The catalog carries
// more - min_order_size, position limits, the strike - and this grows when
// something uses them.
struct InstrumentInfo {
    std::string ticker;
    std::string id;
    std::string series_ticker;
    std::string status;  // "OPEN", "LISTED", ... as the venue spells it
    // The scheduled end. The venue sends it as expected_close_time, so this
    // does not depend on parsing the ticker - though parse_ticker agrees, and
    // the catalog checks that they do.
    Clock::time_point expected_close{};
    // What the venue says this instrument's tick grid is, which is better
    // than the configured guess (GUI-Q23). Nullopt for a structure this build
    // does not know.
    std::optional<TickStructure> ticks;
};

struct InstrumentQuery {
    std::vector<InstrumentInfo> instruments;
    std::string error;  // empty on success, worded for an operator

    bool ok() const { return error.empty(); }
};

// The open instruments of one series. One series per call: the venue answers
// HTTP 400 to a repeated series_ticker and zero rows to a comma-separated one
// (docs/spec/capture-rest-20261028T1205-instruments.txt).
//
// `fetch` is the HTTP call, injected so the parsing, the signing and the URL
// handling are all testable without a network.
using HttpsFetch = std::function<cpm::HttpsResponse(const cpm::HttpsRequest&)>;

InstrumentQuery open_instruments(const RestConfig& config, const std::string& series,
                                 const HttpsFetch& fetch = cpm::https_get,
                                 Clock::time_point now = Clock::now());

// Exposed for its own tests: the base URL an operator pasted, turned into the
// path prefix the endpoints hang off. The spec writes them as /api/v1/..., so
// a URL given as https://host, https://host/api or https://host/api/v1 all
// have to end up in the same place.
struct RestEndpoint {
    std::string host;
    int port = 443;
    std::string prefix;  // "/api/v1"
    std::string error;
};
RestEndpoint parse_rest_url(const std::string& url);

// The signature the venue asks for: base64(HMAC-SHA256(timestamp + method +
// path)), with the query string excluded and an empty body on a GET (API spec
// section 2.2).
std::string rest_signature(const std::string& secret_b64, const std::string& timestamp,
                           const std::string& method, const std::string& path);

}  // namespace cpm::md
