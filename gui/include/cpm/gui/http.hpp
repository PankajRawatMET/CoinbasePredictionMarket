#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Just enough HTTP/1.1 to serve one page and one JSON API to one operator's
// browser over a loopback socket. Not a web server: there is no file serving,
// no chunked request body, no content negotiation and no virtual hosting.
//
// Parsing is a pure function over the bytes received so far, so every case
// that matters - a request split across reads, a header line that never ends,
// a body larger than anyone should send - is a unit test rather than a
// network experiment.
namespace cpm::gui {

struct HttpRequest {
    std::string method;
    std::string target;   // exactly as sent: "/api/events?t=abc"
    std::string path;     // percent-decoded, without the query
    std::string query;    // raw, without the '?'
    std::string version;  // "HTTP/1.1"
    // Names lowercased, because a browser's capitalisation is its own business.
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;

    const std::string* header(std::string_view name) const;
    std::optional<std::string> query_param(std::string_view name) const;
    // HTTP/1.1 keeps the connection open unless it says otherwise; HTTP/1.0
    // closes unless it says otherwise.
    bool keep_alive() const;
};

struct HttpLimits {
    // The request line and headers together. A browser sends well under 2 KiB.
    std::size_t max_head = 16 * 1024;
    // A create request is a few hundred bytes; this is the refusal threshold,
    // not a target.
    std::size_t max_body = 64 * 1024;
};

struct HttpParse {
    enum class State {
        Incomplete,  // valid so far: read more
        Complete,
        Bad,
    };

    State state = State::Incomplete;
    std::size_t consumed = 0;  // bytes of the buffer this request used
    int error_status = 400;
    std::string error;
    HttpRequest request;
};

// Parses one request from the front of `buffer`.
HttpParse parse_request(std::string_view buffer, const HttpLimits& limits = HttpLimits{});

struct HttpResponse {
    int status = 200;
    std::string content_type = "application/json";
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
    bool close = false;  // close the connection once this is written
};

std::string render_response(const HttpResponse& response);
const char* status_text(int status);

// `plus_is_space` is the form encoding, which applies to a query string and
// not to a path: a '+' in a path is a '+'.
std::string percent_decode(std::string_view text, bool plus_is_space = false);

// Constant-time, because the token is a secret and an attacker on the same
// instance can time a loop that stops at the first wrong character.
bool secret_equal(std::string_view a, std::string_view b);

}  // namespace cpm::gui
