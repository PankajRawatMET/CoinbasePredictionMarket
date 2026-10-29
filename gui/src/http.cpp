#include "cpm/gui/http.hpp"

#include <algorithm>
#include <cctype>

namespace cpm::gui {
namespace {

std::string lowercase(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    return text;
}

std::optional<int> hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return std::nullopt;
}

HttpParse bad(int status, std::string message) {
    HttpParse result;
    result.state = HttpParse::State::Bad;
    result.error_status = status;
    result.error = std::move(message);
    return result;
}

}  // namespace

std::string percent_decode(std::string_view text, bool plus_is_space) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '+' && plus_is_space) {
            out.push_back(' ');
        } else if (text[i] == '%' && i + 2 < text.size()) {
            const auto high = hex_value(text[i + 1]);
            const auto low = hex_value(text[i + 2]);
            if (high && low) {
                out.push_back(static_cast<char>((*high << 4) | *low));
                i += 2;
            } else {
                out.push_back(text[i]);
            }
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

bool secret_equal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    unsigned char difference = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        difference |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return difference == 0;
}

const std::string* HttpRequest::header(std::string_view name) const {
    const auto wanted = lowercase(name);
    for (const auto& entry : headers) {
        if (entry.first == wanted) return &entry.second;
    }
    return nullptr;
}

std::optional<std::string> HttpRequest::query_param(std::string_view name) const {
    std::string_view rest = query;
    while (!rest.empty()) {
        const auto amp = rest.find('&');
        const auto pair = rest.substr(0, amp);
        rest = amp == std::string_view::npos ? std::string_view() : rest.substr(amp + 1);
        const auto equals = pair.find('=');
        const auto key = pair.substr(0, equals);
        if (percent_decode(key, true) != name) continue;
        if (equals == std::string_view::npos) return std::string();
        return percent_decode(pair.substr(equals + 1), true);
    }
    return std::nullopt;
}

bool HttpRequest::keep_alive() const {
    const auto* connection = header("connection");
    const auto value = connection ? lowercase(*connection) : std::string();
    if (version == "HTTP/1.0") return value == "keep-alive";
    return value != "close";
}

HttpParse parse_request(std::string_view buffer, const HttpLimits& limits) {
    const auto head_end = buffer.find("\r\n\r\n");
    if (head_end == std::string_view::npos) {
        if (buffer.size() > limits.max_head) {
            return bad(431, "the request headers are too large");
        }
        return HttpParse{};  // incomplete
    }
    if (head_end > limits.max_head) return bad(431, "the request headers are too large");

    HttpParse result;
    HttpRequest& request = result.request;
    std::string_view head = buffer.substr(0, head_end);

    const auto line_end = head.find("\r\n");
    const auto request_line = head.substr(0, line_end);
    head = line_end == std::string_view::npos ? std::string_view() : head.substr(line_end + 2);

    const auto first_space = request_line.find(' ');
    if (first_space == std::string_view::npos) return bad(400, "a malformed request line");
    const auto second_space = request_line.find(' ', first_space + 1);
    if (second_space == std::string_view::npos) return bad(400, "a malformed request line");

    request.method = std::string(request_line.substr(0, first_space));
    request.target = std::string(request_line.substr(first_space + 1, second_space - first_space - 1));
    request.version = std::string(request_line.substr(second_space + 1));
    if (request.method.empty() || request.target.empty()) {
        return bad(400, "a malformed request line");
    }
    if (request.version != "HTTP/1.1" && request.version != "HTTP/1.0") {
        return bad(505, "only HTTP/1.0 and HTTP/1.1 are understood");
    }

    const auto question = request.target.find('?');
    if (question == std::string::npos) {
        request.path = percent_decode(request.target);
    } else {
        request.path = percent_decode(std::string_view(request.target).substr(0, question));
        request.query = request.target.substr(question + 1);
    }

    while (!head.empty()) {
        const auto end = head.find("\r\n");
        const auto line = head.substr(0, end);
        head = end == std::string_view::npos ? std::string_view() : head.substr(end + 2);
        const auto colon = line.find(':');
        if (colon == std::string_view::npos) return bad(400, "a header line without a ':'");
        request.headers.emplace_back(lowercase(trim(line.substr(0, colon))),
                                     std::string(trim(line.substr(colon + 1))));
    }

    std::size_t body_length = 0;
    if (const auto* encoding = request.header("transfer-encoding"); encoding != nullptr) {
        // Browsers do not chunk the bodies this API takes, and accepting a
        // framing we do not implement is how a parser gets desynchronised.
        return bad(411, "a chunked request body is not accepted; send Content-Length");
    }
    if (const auto* length = request.header("content-length"); length != nullptr) {
        if (length->empty() || length->find_first_not_of("0123456789") != std::string::npos) {
            return bad(400, "a Content-Length that is not a number");
        }
        try {
            body_length = static_cast<std::size_t>(std::stoull(*length));
        } catch (const std::exception&) {
            return bad(413, "the request body is too large");
        }
        if (body_length > limits.max_body) return bad(413, "the request body is too large");
    }

    const std::size_t body_start = head_end + 4;
    if (buffer.size() - body_start < body_length) return HttpParse{};  // the body is still arriving

    request.body = std::string(buffer.substr(body_start, body_length));
    result.state = HttpParse::State::Complete;
    result.consumed = body_start + body_length;
    return result;
}

const char* status_text(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 411: return "Length Required";
        case 413: return "Content Too Large";
        case 415: return "Unsupported Media Type";
        case 431: return "Request Header Fields Too Large";
        case 503: return "Service Unavailable";
        case 505: return "HTTP Version Not Supported";
        default: return "Error";
    }
}

std::string render_response(const HttpResponse& response) {
    std::string out = "HTTP/1.1 " + std::to_string(response.status) + " " +
                      status_text(response.status) + "\r\n";
    out += "Content-Type: " + response.content_type + "\r\n";
    out += "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
    // Nothing here is cacheable: every byte is a position or a control.
    out += "Cache-Control: no-store\r\n";
    out += "X-Content-Type-Options: nosniff\r\n";
    // The token travels in the URL on the first load, so no address of this
    // page may leave with a request to anywhere else.
    out += "Referrer-Policy: no-referrer\r\n";
    for (const auto& header : response.headers) {
        out += header.first + ": " + header.second + "\r\n";
    }
    out += std::string("Connection: ") + (response.close ? "close" : "keep-alive") + "\r\n\r\n";
    out += response.body;
    return out;
}

}  // namespace cpm::gui
