#pragma once

#include "cpm/common/tls_stream.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

// A WebSocket client, RFC 6455, written here because the instructions allow
// QuickFIX and GoogleTest and nothing else - the same reason `https_get` and
// the JSON reader are written here (docs/open-questions-binance.txt B-Q1).
//
// It knows nothing about Binance. What it does know is the handful of rules
// that make a long-lived connection survive: a client masks everything it
// sends, a ping is answered with its own payload copied, and a message may
// arrive in fragments.
namespace cpm::ws {

enum class Opcode : std::uint8_t {
    Continuation = 0x0,
    Text = 0x1,
    Binary = 0x2,
    Close = 0x8,
    Ping = 0x9,
    Pong = 0xA,
};

struct Frame {
    Opcode opcode = Opcode::Text;
    std::string payload;
    bool fin = true;
};

// One frame on the wire. `mask` must be true for anything a client sends:
// the RFC requires it, and Binance closes a connection that forgets.
std::string encode_frame(Opcode opcode, std::string_view payload, bool mask);

enum class Parse {
    Ok,        // one frame taken off the front of the buffer
    NeedMore,  // a whole frame is not there yet; keep reading
    Bad,       // see the error; the connection cannot continue
};

// Takes one frame off the front of `buffer` when a whole one is present.
// Exposed because the simulator speaks the same protocol from the other
// side, and a codec tested from both ends is tested twice.
Parse parse_frame(std::string& buffer, Frame& out, std::string& error);

// base64(SHA1(key + the RFC's GUID)), which is what a server answers a
// handshake with and what a client checks.
std::string accept_key(std::string_view client_key);

// 16 random bytes, base64. One per connection.
std::string make_client_key();

class Client {
public:
    struct Options {
        std::string host;
        int port = 443;
        std::string path = "/";
        bool tls = true;
        std::string ca_file;
        std::chrono::milliseconds handshake_timeout{10000};
        // A message larger than this is a protocol error rather than a
        // message: nothing this talks to sends one, and a daemon that is up
        // for days must not grow a buffer without limit.
        std::size_t max_message = 1u << 20;
    };

    // What one pump of the socket did.
    enum class Pump {
        Idle,    // nothing arrived inside the wait
        Read,    // at least one message was delivered
        Closed,  // the peer closed, or the connection failed; see error()
    };

    std::string connect(const Options& options);
    bool connected() const { return stream_.connected(); }

    // Reads for up to `wait`, delivers every complete text message to
    // `on_text`, and answers pings itself. Binary frames are dropped: this
    // talks to one server and that server sends JSON text.
    Pump pump(std::chrono::milliseconds wait, const std::function<void(std::string_view)>& on_text);

    std::string send_text(std::string_view text);
    void close();

    const std::string& error() const { return error_; }

private:
    bool handle(const Frame& frame, const std::function<void(std::string_view)>& on_text);

    TlsStream stream_;
    std::string buffer_;    // bytes read but not yet a whole frame
    std::string message_;   // a fragmented message being put back together
    Opcode message_kind_ = Opcode::Text;
    bool assembling_ = false;
    std::size_t max_message_ = 1u << 20;
    std::string error_;
};

// The other end, for a simulator and for the tests: one client at a time,
// plain TCP on loopback, no TLS. It exists so that the client above is
// tested against something that speaks the protocol rather than against a
// recording - a codec checked from both sides is checked twice - and so that
// `binance_sim` can serve the real feed's shape offline.
class Server {
public:
    ~Server();

    Server() = default;
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // 0 asks the kernel for a free port, which is what the tests use.
    std::string start(int port);
    int port() const { return port_; }

    // Waits up to `wait` for a client and completes its handshake.
    bool accept(std::chrono::milliseconds wait, std::string& error);
    bool attached() const { return client_ >= 0; }

    std::string send_text(std::string_view text);
    std::string send_ping(std::string_view payload);
    std::string send_close();
    // Bytes straight onto the socket, for a test that builds its own frames
    // - a fragmented message, or a malformed one.
    std::string send_raw(std::string_view bytes);

    // Reads whatever the client sent, so that a pong is noticed and a close
    // is seen. Text frames go to `on_text` when one is given, which is how
    // a simulator hears a SUBSCRIBE. Returns false once the client has gone.
    bool pump(std::chrono::milliseconds wait,
              const std::function<void(std::string_view)>& on_text = {});
    bool saw_pong() const { return pongs_ > 0; }
    int pongs() const { return pongs_; }

    // Closes the client's socket without a close frame: what a dropped
    // connection looks like from the other side.
    void drop();
    void stop();

private:
    int listener_ = -1;
    int client_ = -1;
    int port_ = 0;
    int pongs_ = 0;
    std::string buffer_;
};

}  // namespace cpm::ws
