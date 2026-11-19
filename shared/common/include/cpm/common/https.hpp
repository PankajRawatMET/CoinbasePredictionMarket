#pragma once

#include <chrono>
#include <string>
#include <utility>
#include <vector>

// The smallest HTTPS client that can ask the venue a question: one GET, one
// answer, TLS verified. It exists because FIX cannot say which instrument of
// a series is open right now and REST can (docs/design-roll.md section 1).
//
// Not a general HTTP library, and deliberately so: no POST, no redirects, no
// cookies, no connection reuse, no concurrency. It is called once per series
// at startup and on reconnect, never polled, and anything it does not do is
// something that cannot then go wrong unattended.
namespace cpm {

struct HttpsRequest {
    std::string host;
    int port = 443;
    std::string path;   // "/api/v1/instruments"
    std::string query;  // "series_ticker=BTCD15M&status=OPEN", no leading '?'
    std::vector<std::pair<std::string, std::string>> headers;
    std::chrono::seconds timeout{15};
    // A CA bundle to verify the server against. Empty means the system trust
    // store, which is what both FIX clients use against the sandbox.
    std::string ca_file;
    bool tls = true;
};

struct HttpsResponse {
    int status = 0;      // 0 when the request never completed
    std::string body;
    std::string error;   // why it never completed, worded for an operator

    bool ok() const { return error.empty() && status == 200; }
};

// Performs the request. Never throws: a failure is a response with an error,
// because this is called while a daemon is starting and the caller has to be
// able to say why it could not.
//
// The server's certificate chain AND its hostname are verified. A client that
// skipped either would hand an API key to whoever answered the socket.
HttpsResponse https_get(const HttpsRequest& request);

}  // namespace cpm
