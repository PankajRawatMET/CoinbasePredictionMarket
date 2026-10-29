#include "cpm/common/https.hpp"

#include "cpm/common/tls_stream.hpp"

#include <cctype>
#include <cstring>

namespace cpm {
namespace {

std::string lowercase(std::string_view text) {
    std::string out(text);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// A chunked body, put back together. The alternative - refusing chunked - is
// not available: the server chooses, not us.
std::string dechunk(std::string_view body, std::string& error) {
    std::string out;
    std::size_t at = 0;
    while (at < body.size()) {
        const auto line_end = body.find("\r\n", at);
        if (line_end == std::string_view::npos) {
            error = "a chunked body that ends mid-header";
            return out;
        }
        const auto header = body.substr(at, line_end - at);
        // A chunk size may carry extensions after a ';'; the size is the part
        // before it, in hex.
        const auto size_text = header.substr(0, header.find(';'));
        std::size_t size = 0;
        if (size_text.empty()) {
            error = "a chunked body with no size";
            return out;
        }
        for (const char c : size_text) {
            int digit = 0;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
            else {
                error = "a chunk size that is not hexadecimal";
                return out;
            }
            size = size * 16 + static_cast<std::size_t>(digit);
        }
        at = line_end + 2;
        if (size == 0) return out;  // the last chunk
        if (at + size > body.size()) {
            error = "a chunked body that ends mid-chunk";
            return out;
        }
        out.append(body.substr(at, size));
        at += size + 2;  // the chunk and its trailing CRLF
    }
    error = "a chunked body with no terminator";
    return out;
}

}  // namespace

HttpsResponse https_get(const HttpsRequest& request) {
    HttpsResponse response;
    if (request.host.empty()) {
        response.error = "no host to ask: is CPM_REST_URL set?";
        return response;
    }

    // One verified-TLS path for this and for the WebSocket client, so that a
    // certificate check cannot be right in one and wrong in the other.
    TlsStream stream;
    TlsStream::Options transport;
    transport.host = request.host;
    transport.port = request.port;
    transport.ca_file = request.ca_file;
    transport.connect_timeout = request.timeout;
    if (auto failed = stream.connect(transport); !failed.empty()) {
        response.error = failed;
        return response;
    }

    std::string wire = "GET " + request.path;
    if (!request.query.empty()) wire += "?" + request.query;
    wire += " HTTP/1.1\r\nHost: " + request.host + "\r\n";
    for (const auto& [name, value] : request.headers) wire += name + ": " + value + "\r\n";
    // One request per connection: no keep-alive to manage, and the close is
    // how the body ends when there is no length.
    wire += "Connection: close\r\n\r\n";

    if (auto failed = stream.write(wire); !failed.empty()) {
        response.error = failed;
        return response;
    }

    std::string raw;
    const auto deadline = std::chrono::steady_clock::now() + request.timeout;
    while (true) {
        std::string why;
        const auto got = stream.read(raw, std::chrono::milliseconds(200), why);
        if (got == TlsStream::Read::Closed) break;  // how a body with no length ends
        if (got == TlsStream::Read::Error) {
            response.error = why;
            return response;
        }
        // A response this large is not one of ours; stop rather than grow.
        if (raw.size() > 8u * 1024 * 1024) {
            response.error = "the response is larger than 8 MiB";
            return response;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            response.error = "the server did not finish answering in time";
            return response;
        }
    }

    const auto head_end = raw.find("\r\n\r\n");
    if (head_end == std::string::npos) {
        response.error = raw.empty() ? "the server said nothing at all"
                                     : "the server's answer had no complete header";
        return response;
    }
    const auto head = raw.substr(0, head_end);
    auto body = raw.substr(head_end + 4);

    const auto line_end = head.find("\r\n");
    const auto status_line = head.substr(0, line_end);
    const auto first_space = status_line.find(' ');
    if (first_space == std::string::npos || status_line.size() < first_space + 4) {
        response.error = "the server's status line was malformed: " + status_line;
        return response;
    }
    try {
        response.status = std::stoi(status_line.substr(first_space + 1, 3));
    } catch (const std::exception&) {
        response.error = "the server's status line was malformed: " + status_line;
        return response;
    }

    if (lowercase(head).find("transfer-encoding: chunked") != std::string::npos) {
        std::string error;
        body = dechunk(body, error);
        if (!error.empty()) {
            response.error = error;
            return response;
        }
    }
    response.body = std::move(body);
    return response;
}

}  // namespace cpm
