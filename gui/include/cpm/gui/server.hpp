#pragma once

#include "cpm/common/log_ring.hpp"
#include "cpm/common/logging.hpp"
#include "cpm/gui/http.hpp"
#include "cpm/trader/control_api.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// The only sockets in the project that are not FIX: an HTTP/SSE surface over
// ControlApi, bound to the loopback interface and nothing else.
//
// It binds 127.0.0.1 with no way to ask for anything wider (GUI-Q3). The
// operator reaches it through an SSH local forward, so SSH is the
// authentication and the encryption; the per-run token is what keeps another
// user on the same instance out, which a loopback bind alone does not do.
namespace cpm::gui {

using Clock = cpm::trader::Clock;

class Server {
public:
    struct Options {
        // 0 asks the kernel for a free one, which is what the tests use.
        int port = 8080;
        // Empty generates one from OpenSSL's RAND_bytes.
        std::string token;
        std::size_t max_connections = 16;
        // A browser that stops reading must not grow the daemon's heap: past
        // this much unsent data the connection is dropped.
        std::size_t max_queued_bytes = 1 << 20;
        // How often the daemon looks at itself for the watchers' sake. The
        // operator's own actions are not the only thing that changes a
        // screen: books move, quotes are re-placed and fills arrive, and none
        // of those pass through the control API at all.
        std::chrono::milliseconds refresh{500};
        // An SSE comment on this cadence, so a dead tunnel is noticed by both
        // ends rather than looking like a quiet market.
        std::chrono::seconds heartbeat{15};
        // How long a browser's spare keep-alive connection may sit holding a
        // slot with no request in it. Streams are exempt: sitting there is
        // what a stream is for. Advertised in Keep-Alive, so the browser
        // closes its end first rather than racing us with a POST.
        std::chrono::seconds idle_timeout{60};
        // What the browser waits before reconnecting a dropped stream. Sent
        // as the stream's own `retry:` so it is this daemon's decision rather
        // than a browser default.
        std::chrono::milliseconds stream_retry{2000};
        HttpLimits limits;
        // The tail of the daemon's log, for the panel at the bottom of the
        // page (GUI-Q27). Null leaves the panel empty, which is what the
        // tests that do not care about it get.
        const LogRing* logs = nullptr;
        // At most this many lines in the first event a page is sent. The
        // ring holds more; a browser does not need all of it to start.
        std::size_t log_backlog = 200;
    };

    Server(cpm::trader::ControlApi& api, Options options, Logger& logger);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Binds, listens and starts the thread. Returns an empty string, or why
    // it could not - a taken port names itself, because a stale daemon still
    // holding it is the obvious first failure.
    std::string start();
    void stop();

    int port() const { return port_; }
    const std::string& token() const { return token_; }
    // http://localhost:PORT/?t=TOKEN - the link the operator opens.
    std::string url() const;
    // The lines the daemon prints at startup: the link, the tunnel that makes
    // it reachable from Windows, and whether orders are permitted at all.
    std::vector<std::string> banner(bool allow_orders) const;

private:
    struct Connection;

    void run();
    void accept_ready();
    void on_readable(Connection& connection);
    void on_writable(Connection& connection);
    // Parses whatever has arrived and answers what is complete.
    void serve(Connection& connection);
    // Routes one request. Returns false when the connection has become an SSE
    // stream and must not be answered again.
    bool dispatch(Connection& connection, const HttpRequest& request);
    HttpResponse route(const HttpRequest& request);
    bool authorised(const HttpRequest& request) const;
    bool host_is_loopback(const HttpRequest& request) const;
    void push_events();
    // Closes the keep-alive connections nobody is using.
    void reap_idle(Clock::time_point now);
    void queue(Connection& connection, std::string bytes);
    void close(Connection& connection);

    cpm::trader::ControlApi& api_;
    const Options options_;
    Logger& logger_;

    std::string token_;
    int listener_ = -1;
    // Written to by stop() to break the poll(), because a thread parked in
    // poll() cannot be asked anything else.
    int wake_[2] = {-1, -1};
    int port_ = 0;
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::vector<std::unique_ptr<Connection>> connections_;

    // The snapshot as it was last rendered, shared by every watcher: one
    // render per refresh however many are connected.
    std::string rendered_;
    std::uint64_t rendered_revision_ = 0;
    Clock::time_point last_render_{};
};

// 16 hex characters from RAND_bytes. Exposed for the test that proves two
// runs do not share one.
std::string make_token();

}  // namespace cpm::gui
