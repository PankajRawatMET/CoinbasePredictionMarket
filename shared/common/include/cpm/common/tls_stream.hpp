#pragma once

#include <chrono>
#include <string>
#include <string_view>

// One connection, held open: connect, write, read with a deadline, close.
//
// It exists because `https_get` is one-shot by design and a WebSocket is the
// opposite - a socket that stays up for a day and is read whenever something
// arrives. Both now sit on this, so there is one place where a certificate
// chain and a hostname are verified, rather than two that could drift
// (docs/design-binance.md section 3).
//
// Not thread-safe: one owner, one thread. The Binance feed gives it a thread
// of its own and talks to the rest of the daemon through a mutex and a byte
// down a pipe, never by sharing this.
namespace cpm {

class TlsStream {
public:
    struct Options {
        std::string host;
        int port = 443;
        // False is a plain TCP socket, for a simulator on loopback. There is
        // no certificate to verify there and no secret to protect, and the
        // alternative - a test that needs a key pair - is a test nobody runs.
        bool tls = true;
        // Empty means the system trust store, as both FIX clients use.
        std::string ca_file;
        std::chrono::milliseconds connect_timeout{15000};
        bool bounded_io = false;
    };

    enum class Read {
        Data,     // bytes were appended to `out`
        Timeout,  // nothing arrived inside the wait, which is not an error
        Closed,   // the peer went away cleanly
        Error,    // see the error string
    };

    TlsStream() = default;
    ~TlsStream();

    TlsStream(const TlsStream&) = delete;
    TlsStream& operator=(const TlsStream&) = delete;

    // Empty on success, else why not, worded for an operator.
    std::string connect(const Options& options);
    bool connected() const { return bio_ != nullptr; }

    // Writes all of it, or says why it could not.
    std::string write(std::string_view data);

    // Waits up to `wait` for bytes and appends whatever arrived to `out`.
    Read read(std::string& out, std::chrono::milliseconds wait, std::string& error);

    void close();

private:
    void* ctx_ = nullptr;  // SSL_CTX*, kept opaque so OpenSSL stays in the .cpp
    void* bio_ = nullptr;  // BIO*
    bool bounded_io_ = false;
    std::chrono::milliseconds timeout_{};
};

}  // namespace cpm
