#include "cpm/gui/server.hpp"

#include "cpm/gui/json.hpp"
#include "cpm/gui/page.hpp"
#include "cpm/gui/views.hpp"

#include <openssl/rand.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace cpm::gui {
namespace {

using cpm::trader::Result;

constexpr std::size_t kReadChunk = 8192;

std::string errno_text() { return std::strerror(errno); }

bool set_non_blocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

// "GET /api/strategies/s1/start" -> {"api","strategies","s1","start"}
std::vector<std::string> split_path(std::string_view path) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto slash = path.find('/', start);
        const auto piece = path.substr(start, slash == std::string_view::npos ? std::string_view::npos
                                                                              : slash - start);
        if (!piece.empty()) parts.emplace_back(piece);
        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }
    return parts;
}

// The "params" object of a request body, as the strategy schemas want it:
// every value as text, whatever JSON type it arrived as. Shared by create and
// by the params action so the two cannot disagree about what a value means -
// and the kind's own schema is what validates them, which is also what the
// command line goes through.
std::string read_params(const JsonValue& body, cpm::strategy::ParamValues& out) {
    const auto* params = body.find("params");
    if (params == nullptr) return {};  // nothing to change is not an error
    if (!params->is_object()) return "\"params\" must be an object";
    for (const auto& param : params->fields) {
        if (param.second.type == JsonValue::Type::String ||
            param.second.type == JsonValue::Type::Number) {
            out[param.first] = param.second.text;
        } else if (param.second.type == JsonValue::Type::Bool) {
            out[param.first] = param.second.boolean ? "true" : "false";
        } else {
            return "the parameter " + param.first + " must be a string, a number or a boolean";
        }
    }
    return {};
}

HttpResponse json_response(int status, std::string body) {
    HttpResponse response;
    response.status = status;
    response.content_type = "application/json";
    response.body = std::move(body);
    return response;
}

HttpResponse refusal(int status, std::string_view message) {
    return json_response(status, error_json(message));
}

std::string sse_event(std::string_view name, std::uint64_t id, std::string_view data) {
    std::string out = "event: ";
    out += name;
    out += "\nid: ";
    out += std::to_string(id);
    out += "\ndata: ";
    // The payload is JSON, and JSON never contains a raw newline, so one data
    // line always suffices and no re-framing is needed.
    out += data;
    out += "\n\n";
    return out;
}

}  // namespace

std::string make_token() {
    unsigned char bytes[8];
    if (RAND_bytes(bytes, sizeof(bytes)) != 1) {
        // The same OpenSSL that signs the logon; if it cannot produce eight
        // random bytes, a guessable token is not the compromise to make.
        throw std::runtime_error("OpenSSL could not generate a GUI token");
    }
    static const char kHex[] = "0123456789abcdef";
    std::string token;
    token.reserve(sizeof(bytes) * 2);
    for (const unsigned char byte : bytes) {
        token.push_back(kHex[byte >> 4]);
        token.push_back(kHex[byte & 0x0F]);
    }
    return token;
}

struct Server::Connection {
    int fd = -1;
    std::string in;
    std::string out;
    bool sse = false;
    bool close_when_drained = false;
    bool dead = false;
    std::uint64_t sent_revision = 0;
    // What this connection was last told, so it is sent an update when the
    // daemon's state differs from it - whatever caused the difference.
    std::string last_payload;
    Clock::time_point last_beat{};
    // The last log line this connection has been sent. A page that joins at
    // 0 is sent the tail of the ring, so a reload shows what a session that
    // never dropped is showing.
    std::uint64_t sent_log_seq = 0;
    // The last time this connection was of any use: accepted, read from, or
    // written to. Idleness is measured from here.
    Clock::time_point last_use{};
};

Server::Server(cpm::trader::ControlApi& api, Options options, Logger& logger)
    : api_(api), options_(std::move(options)), logger_(logger) {}

Server::~Server() { stop(); }

std::string Server::start() {
    if (running_) return {};
    token_ = options_.token.empty() ? make_token() : options_.token;

    listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener_ < 0) return "could not create the GUI socket: " + errno_text();

    const int reuse = 1;
    ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    // Loopback, always. There is no option for anything else (GUI-Q3).
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<std::uint16_t>(options_.port));

    if (::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        const bool taken = errno == EADDRINUSE;
        const std::string why = errno_text();
        ::close(listener_);
        listener_ = -1;
        if (taken) {
            return "port " + std::to_string(options_.port) +
                   " is already in use - another cpm_trader is probably still running; stop it or "
                   "pass --port";
        }
        return "could not bind 127.0.0.1:" + std::to_string(options_.port) + ": " + why;
    }

    if (::listen(listener_, 8) != 0) {
        const std::string why = errno_text();
        ::close(listener_);
        listener_ = -1;
        return "could not listen on the GUI socket: " + why;
    }

    sockaddr_in bound{};
    socklen_t bound_length = sizeof(bound);
    if (::getsockname(listener_, reinterpret_cast<sockaddr*>(&bound), &bound_length) == 0) {
        port_ = ntohs(bound.sin_port);
    } else {
        port_ = options_.port;
    }

    if (!set_non_blocking(listener_) || ::pipe(wake_) != 0 || !set_non_blocking(wake_[0])) {
        const std::string why = errno_text();
        ::close(listener_);
        listener_ = -1;
        return "could not set up the GUI socket: " + why;
    }

    running_ = true;
    thread_ = std::thread([this] { run(); });
    return {};
}

void Server::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (wake_[1] >= 0) {
        const char byte = 'x';
        const ssize_t written = ::write(wake_[1], &byte, 1);
        (void)written;
    }
    if (thread_.joinable()) thread_.join();
    for (auto& connection : connections_) {
        if (connection->fd >= 0) ::close(connection->fd);
    }
    connections_.clear();
    if (listener_ >= 0) ::close(listener_);
    listener_ = -1;
    for (int& end : wake_) {
        if (end >= 0) ::close(end);
        end = -1;
    }
}

std::string Server::url() const {
    return "http://localhost:" + std::to_string(port_) + "/?t=" + token_;
}

std::vector<std::string> Server::banner(bool allow_orders) const {
    char host[256];
    if (::gethostname(host, sizeof(host)) != 0) std::strcpy(host, "<this-host>");
    host[sizeof(host) - 1] = '\0';

    std::vector<std::string> lines;
    lines.push_back("cpm_trader listening on 127.0.0.1:" + std::to_string(port_));
    lines.push_back("  open:  " + url());
    lines.push_back(std::string("  no tunnel? reconnect with:  ssh -L ") + std::to_string(port_) +
                    ":localhost:" + std::to_string(port_) + " <user>@" + host);
    lines.push_back(allow_orders ? "orders: PERMITTED (each strategy still has to be armed)"
                                 : "orders: DISARMED (start with --allow-orders to permit arming)");
    return lines;
}

void Server::run() {
    std::vector<pollfd> watch;
    while (running_) {
        watch.clear();
        watch.push_back(pollfd{listener_, POLLIN, 0});
        watch.push_back(pollfd{wake_[0], POLLIN, 0});
        for (const auto& connection : connections_) {
            short events = POLLIN;
            if (!connection->out.empty()) events |= POLLOUT;
            watch.push_back(pollfd{connection->fd, events, 0});
        }

        // Fixed before anything is accepted: accept_ready() appends to
        // connections_, and those new entries have no slot in `watch`.
        const std::size_t watched = connections_.size();

        const int ready = ::poll(watch.data(), watch.size(), 250);
        if (ready < 0 && errno != EINTR) {
            logger_.error("gui: poll failed: " + errno_text());
            break;
        }
        if (!running_) break;

        if (ready > 0) {
            if ((watch[0].revents & POLLIN) != 0) accept_ready();
            if ((watch[1].revents & POLLIN) != 0) {
                char drain[64];
                while (::read(wake_[0], drain, sizeof(drain)) > 0) {
                }
            }
            // Only the connections that had a slot in `watch`: whatever
            // accept_ready() just added is polled on the next pass.
            for (std::size_t i = 0; i < watched; ++i) {
                const auto& events = watch[i + 2].revents;
                auto& connection = *connections_[i];
                if ((events & POLLOUT) != 0) on_writable(connection);
                if (!connection.dead && (events & POLLIN) != 0) on_readable(connection);
                if (!connection.dead && (events & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                    close(connection);
                }
            }
        }

        const auto now = Clock::now();
        push_events();
        reap_idle(now);

        for (auto it = connections_.begin(); it != connections_.end();) {
            if ((*it)->dead) {
                if ((*it)->fd >= 0) ::close((*it)->fd);
                it = connections_.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void Server::accept_ready() {
    while (true) {
        const int fd = ::accept(listener_, nullptr, nullptr);
        if (fd < 0) return;  // EAGAIN: no more waiting
        if (!set_non_blocking(fd)) {
            ::close(fd);
            continue;
        }
        const int nodelay = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        if (connections_.size() >= options_.max_connections) {
            // Answer rather than drop: a browser that is told why can say so.
            const auto body = render_response([] {
                auto response = refusal(503, "too many GUI connections are already open");
                response.close = true;
                return response;
            }());
            ::send(fd, body.data(), body.size(), MSG_NOSIGNAL);
            ::close(fd);
            continue;
        }

        auto connection = std::make_unique<Connection>();
        connection->fd = fd;
        connection->last_beat = Clock::now();
        connection->last_use = connection->last_beat;
        connections_.push_back(std::move(connection));
    }
}

void Server::on_readable(Connection& connection) {
    while (true) {
        char chunk[kReadChunk];
        const ssize_t got = ::recv(connection.fd, chunk, sizeof(chunk), 0);
        if (got == 0) {
            close(connection);
            return;
        }
        if (got < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            close(connection);
            return;
        }
        connection.in.append(chunk, static_cast<std::size_t>(got));
        connection.last_use = Clock::now();
        if (connection.in.size() > options_.limits.max_head + options_.limits.max_body) {
            queue(connection, render_response([] {
                      auto response = refusal(413, "the request is too large");
                      response.close = true;
                      return response;
                  }()));
            connection.close_when_drained = true;
            return;
        }
    }
    serve(connection);
}

void Server::serve(Connection& connection) {
    while (!connection.dead && !connection.close_when_drained && !connection.sse) {
        const auto parsed = parse_request(connection.in, options_.limits);
        if (parsed.state == HttpParse::State::Incomplete) return;
        if (parsed.state == HttpParse::State::Bad) {
            auto response = refusal(parsed.error_status, parsed.error);
            response.close = true;
            queue(connection, render_response(response));
            connection.close_when_drained = true;
            return;
        }
        connection.in.erase(0, parsed.consumed);
        if (!dispatch(connection, parsed.request)) return;  // it is a stream now
    }
}

bool Server::dispatch(Connection& connection, const HttpRequest& request) {
    const bool is_events = request.path == "/api/events" && request.method == "GET";
    if (is_events && authorised(request) && host_is_loopback(request)) {
        connection.sse = true;
        connection.last_beat = Clock::now();
        std::string head = "HTTP/1.1 200 OK\r\n";
        head += "Content-Type: text/event-stream\r\n";
        head += "Cache-Control: no-store\r\n";
        head += "X-Content-Type-Options: nosniff\r\n";
        head += "Referrer-Policy: no-referrer\r\n";
        head += "Connection: keep-alive\r\n\r\n";
        // Snapshot first, always, because a browser arriving mid-run is the
        // normal case on this setup and not the exception (design §7). The
        // reconnect delay is a field of that same event: how long the browser
        // waits for a dropped stream is this daemon's decision rather than
        // whichever browser it is.
        const auto snapshot = api_.snapshot();
        auto payload = snapshot_json(snapshot);
        connection.sent_revision = snapshot.revision;
        head += "retry: " + std::to_string(options_.stream_retry.count()) + "\n";
        head += sse_event("snapshot", snapshot.revision, payload);
        connection.last_payload = std::move(payload);
        queue(connection, std::move(head));
        return false;
    }

    auto response = route(request);
    response.close = response.close || !request.keep_alive();
    if (!response.close) {
        // Say how long this connection will be held, so the browser retires
        // it before we do rather than losing a POST to the race.
        response.headers.emplace_back(
            "Keep-Alive", "timeout=" + std::to_string(options_.idle_timeout.count()));
    }
    queue(connection, render_response(response));
    if (response.close) connection.close_when_drained = true;
    return true;
}

bool Server::authorised(const HttpRequest& request) const {
    if (const auto* header = request.header("authorization"); header != nullptr) {
        constexpr std::string_view kBearer = "Bearer ";
        if (header->size() > kBearer.size() &&
            std::string_view(*header).substr(0, kBearer.size()) == kBearer) {
            return secret_equal(std::string_view(*header).substr(kBearer.size()), token_);
        }
    }
    // EventSource cannot set a header, so the stream and the page load carry
    // the token in the query instead. The page keeps it there for reloads.
    if (const auto value = request.query_param("t"); value) {
        return secret_equal(*value, token_);
    }
    return false;
}

bool Server::host_is_loopback(const HttpRequest& request) const {
    const auto* host = request.header("host");
    if (host == nullptr) return true;  // HTTP/1.0, curl --http1.0, our tests
    std::string_view name = *host;
    if (!name.empty() && name.front() == '[') {  // [::1]:8080
        const auto end = name.find(']');
        if (end == std::string_view::npos) return false;
        name = name.substr(1, end - 1);
    } else if (const auto colon = name.rfind(':'); colon != std::string_view::npos) {
        name = name.substr(0, colon);
    }
    // A name that resolves to 127.0.0.1 from somewhere else is how a page on
    // another site reaches a loopback server; only these three are this one.
    return name == "localhost" || name == "127.0.0.1" || name == "::1";
}

HttpResponse Server::route(const HttpRequest& request) {
    if (!host_is_loopback(request)) {
        return refusal(403, "this server answers to localhost only");
    }
    if (!authorised(request)) {
        auto response = refusal(401, "a valid run token is required");
        response.headers.emplace_back("WWW-Authenticate", "Bearer realm=\"cpm_trader\"");
        return response;
    }

    const auto parts = split_path(request.path);

    if (request.path == "/" || request.path == "/index.html") {
        if (request.method != "GET") return refusal(405, "the page is a GET");
        HttpResponse response;
        response.content_type = "text/html; charset=utf-8";
        response.body = std::string(page());
        // The page is one self-contained document: it loads nothing, and with
        // this it cannot be made to.
        response.headers.emplace_back(
            "Content-Security-Policy",
            "default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; "
            "connect-src 'self'; form-action 'none'; frame-ancestors 'none'");
        return response;
    }

    if (request.path == "/api/snapshot") {
        if (request.method != "GET") return refusal(405, "the snapshot is a GET");
        return json_response(200, snapshot_json(api_.snapshot()));
    }

    if (request.path == "/api/schema") {
        if (request.method != "GET") return refusal(405, "the schema is a GET");
        return json_response(200, schema_json(api_.kinds()));
    }

    if (request.path == "/api/ladder") {
        if (request.method != "GET") return refusal(405, "the ladder axis is a GET");
        const auto symbol = request.query_param("symbol");
        if (!symbol || symbol->empty()) return refusal(400, "the ladder needs a ?symbol=");
        const auto axis = api_.axis(*symbol);
        if (axis.prices.empty()) {
            return refusal(404, "no strategy is watching " + *symbol + ", so it has no ladder");
        }
        return json_response(200, axis_json(axis));
    }

    if (request.path == "/api/events") {
        return refusal(405, "the event stream is a GET");
    }

    if (request.path == "/api/stop-all") {
        if (request.method != "POST") return refusal(405, "stop-all is a POST");
        const auto result = api_.stop_all();
        return json_response(result.ok() ? 200 : 409, result_json(result, api_.revision()));
    }

    // POST /api/positions/{symbol}/exit, body {"qty": N} (G13). The quantity
    // is in contracts, written as the page shows it, and 0 or absent is the
    // whole position.
    if (parts.size() == 4 && parts[0] == "api" && parts[1] == "positions" && parts[3] == "exit") {
        if (request.method != "POST") return refusal(405, "an exit is a POST");
        Qty qty = 0;
        if (!request.body.empty()) {
            const auto parsed = json_parse(request.body);
            if (!parsed.ok()) return refusal(400, "the request body is not JSON: " + parsed.error);
            if (const auto* value = parsed.value.find("qty"); value != nullptr) {
                // A number or the same text the page renders: both arrive as
                // the digits that were sent, so neither goes through a double.
                if (value->type != JsonValue::Type::Number &&
                    value->type != JsonValue::Type::String) {
                    return refusal(400, "\"qty\" must be a number or a string");
                }
                const auto contracts = cpm::parse_qty(value->text);
                if (!contracts) {
                    return refusal(400, "\"qty\" is not a quantity: " + value->text);
                }
                qty = *contracts;
            }
        }
        const auto result = api_.exit_position(parts[2], qty);
        return json_response(result.ok() ? 200 : 409, result_json(result, api_.revision()));
    }

    // POST /api/strategies            create
    // POST /api/strategies/{id}/{action}, where params carries a body
    if (!parts.empty() && parts[0] == "api" && parts.size() >= 2 && parts[1] == "strategies") {
        if (request.method != "POST") return refusal(405, "strategies are changed with POST");

        if (parts.size() == 2) {
            const auto parsed = json_parse(request.body);
            if (!parsed.ok()) return refusal(400, "the request body is not JSON: " + parsed.error);
            const auto* kind = parsed.value.find("kind");
            if (kind == nullptr || !kind->is_string() || kind->text.empty()) {
                return refusal(400, "the request needs a \"kind\"");
            }
            cpm::strategy::StrategySpec spec;
            spec.kind = kind->text;
            if (const auto error = read_params(parsed.value, spec.params); !error.empty()) {
                return refusal(400, error);
            }
            const auto result = api_.create(spec);
            return json_response(result.ok() ? 200 : 409, result_json(result, api_.revision()));
        }

        if (parts.size() != 4) return refusal(404, "no such path");
        const auto& id = parts[2];
        const auto& action = parts[3];
        Result result;
        if (action == "params") {
            // The only action with a body: the values to change, in the same
            // shape create takes them.
            const auto parsed = json_parse(request.body);
            if (!parsed.ok()) return refusal(400, "the request body is not JSON: " + parsed.error);
            cpm::strategy::ParamValues values;
            if (const auto error = read_params(parsed.value, values); !error.empty()) {
                return refusal(400, error);
            }
            result = api_.update(id, values);
        }
        else if (action == "start") result = api_.start(id);
        else if (action == "stop") result = api_.stop(id);
        else if (action == "arm") result = api_.arm(id);
        else if (action == "disarm") result = api_.disarm(id);
        else if (action == "start-feed") result = api_.start_feed(id);
        else if (action == "stop-feed") result = api_.stop_feed(id);
        else if (action == "destroy") result = api_.destroy(id);
        else return refusal(404, "unknown action " + action);
        return json_response(result.ok() ? 200 : 409, result_json(result, api_.revision()));
    }

    return refusal(404, "no such path");
}

void Server::push_events() {
    const bool any_streams = std::any_of(connections_.begin(), connections_.end(),
                                         [](const auto& c) { return c->sse && !c->dead; });
    if (!any_streams) return;

    const auto now = Clock::now();

    // What a watcher is shown must follow the daemon, not the operator. The
    // revision counts control-API calls only, so a stream woken by the
    // revision alone would freeze on the last button pressed while the book
    // moved, quotes were re-placed and fills arrived - every one of which
    // reaches the daemon without passing through ControlApi at all.
    //
    // So the state is rendered on a timer and compared: identical means
    // nothing to say, and the heartbeat below covers saying nothing.
    if (now - last_render_ >= options_.refresh) {
        const auto snapshot = api_.snapshot();
        rendered_ = snapshot_json(snapshot);
        rendered_revision_ = snapshot.revision;
        last_render_ = now;
    }

    std::string event;  // built at most once per pass, however many are watching

    for (auto& holder : connections_) {
        auto& connection = *holder;
        if (!connection.sse || connection.dead) continue;

        // The log goes out on its own event, not in the snapshot: the
        // snapshot is re-sent whenever anything moves, and the tail of a log
        // would be the same hundreds of lines every time (GUI-Q27).
        if (options_.logs != nullptr) {
            const auto lines = options_.logs->since(connection.sent_log_seq, options_.log_backlog);
            if (!lines.empty()) {
                connection.sent_log_seq = lines.back().seq;
                connection.last_beat = now;
                queue(connection, sse_event("log", lines.back().seq, logs_json(lines)));
            }
        }

        if (!rendered_.empty() && connection.last_payload != rendered_) {
            if (event.empty()) event = sse_event("snapshot", rendered_revision_, rendered_);
            connection.sent_revision = rendered_revision_;
            connection.last_payload = rendered_;
            connection.last_beat = now;
            queue(connection, event);
        } else if (now - connection.last_beat >= options_.heartbeat) {
            connection.last_beat = now;
            // A named event rather than an SSE comment: a comment keeps the
            // connection warm but is invisible to the page, which then cannot
            // tell a quiet market from a frozen tunnel. The revision rides
            // along, so a page that missed an event can tell (G5).
            JsonWriter beat;
            beat.object_begin();
            beat.field("revision", rendered_revision_);
            beat.object_end();
            queue(connection, sse_event("heartbeat", rendered_revision_, beat.str()));
        }
    }
}

void Server::reap_idle(Clock::time_point now) {
    for (auto& holder : connections_) {
        auto& connection = *holder;
        // A stream is supposed to sit there, and a connection with a request
        // part-way in or an answer part-way out is in use by definition.
        if (connection.dead || connection.sse) continue;
        if (!connection.in.empty() || !connection.out.empty()) continue;
        if (now - connection.last_use < options_.idle_timeout) continue;
        close(connection);
    }
}

void Server::queue(Connection& connection, std::string bytes) {
    if (connection.dead) return;
    connection.out += bytes;
    if (connection.out.size() > options_.max_queued_bytes) {
        // A browser that has stopped reading is not a reason for the daemon to
        // grow: it can reconnect and get a fresh snapshot.
        logger_.warn("gui: dropping a connection that stopped reading");
        close(connection);
        return;
    }
    on_writable(connection);
}

void Server::on_writable(Connection& connection) {
    while (!connection.out.empty()) {
        const ssize_t sent =
            ::send(connection.fd, connection.out.data(), connection.out.size(), MSG_NOSIGNAL);
        if (sent > 0) {
            connection.out.erase(0, static_cast<std::size_t>(sent));
            connection.last_use = Clock::now();
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (sent < 0 && errno == EINTR) continue;
        close(connection);
        return;
    }
    if (connection.close_when_drained) close(connection);
}

void Server::close(Connection& connection) {
    connection.dead = true;
    connection.out.clear();
}

}  // namespace cpm::gui
