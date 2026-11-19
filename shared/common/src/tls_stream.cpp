#include "cpm/common/tls_stream.hpp"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <poll.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>

namespace cpm {
namespace {

std::string openssl_error() {
    const unsigned long code = ERR_get_error();
    if (code == 0) return "no detail from OpenSSL";
    char buffer[256];
    ERR_error_string_n(code, buffer, sizeof(buffer));
    return buffer;
}

using Deadline = std::chrono::steady_clock::time_point;

bool wait_for_bio(BIO* bio, Deadline deadline) {
    while (std::chrono::steady_clock::now() < deadline) {
        int fd = -1;
        if (BIO_get_fd(bio, &fd) <= 0 || fd < 0) return false;
        const short events = BIO_should_read(bio) ? POLLIN : POLLOUT;
        pollfd watch{fd, events, 0};
        const auto left = std::chrono::ceil<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        const int result = ::poll(&watch, 1, static_cast<int>(std::clamp<std::int64_t>(left, 0, 60000)));
        if (result > 0) return true;
        if (result < 0 && errno != EINTR) return false;
    }
    return false;
}

bool connect_bio(BIO* bio, bool tls, bool bounded, Deadline deadline) {
    if (bounded) BIO_set_nbio(bio, 1);
    while (BIO_do_connect(bio) <= 0) {
        if (!bounded || !BIO_should_retry(bio) || !wait_for_bio(bio, deadline)) return false;
    }
    if (tls) {
        while (BIO_do_handshake(bio) <= 0) {
            if (!bounded || !BIO_should_retry(bio) || !wait_for_bio(bio, deadline)) return false;
        }
    }
    return true;
}

}  // namespace

TlsStream::~TlsStream() { close(); }

void TlsStream::close() {
    if (bio_ != nullptr) BIO_free_all(static_cast<BIO*>(bio_));
    if (ctx_ != nullptr) SSL_CTX_free(static_cast<SSL_CTX*>(ctx_));
    bio_ = nullptr;
    ctx_ = nullptr;
}

std::string TlsStream::connect(const Options& options) {
    close();
    if (options.host.empty()) return "no host to connect to";
    bounded_io_ = options.bounded_io;
    timeout_ = options.connect_timeout;
    const auto deadline = std::chrono::steady_clock::now() + timeout_;

    const auto where = options.host + ":" + std::to_string(options.port);
    BIO* bio = nullptr;

    if (options.tls) {
        auto* ctx = SSL_CTX_new(TLS_client_method());
        if (ctx == nullptr) return "could not create a TLS context: " + openssl_error();
        ctx_ = ctx;
        // TLS 1.2 at the oldest, which is what every endpoint here uses.
        SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
        const bool trust = options.ca_file.empty()
                               ? SSL_CTX_set_default_verify_paths(ctx) == 1
                               : SSL_CTX_load_verify_locations(ctx, options.ca_file.c_str(),
                                                               nullptr) == 1;
        if (!trust) {
            close();
            return "could not load the CA certificates: " + openssl_error();
        }

        bio = BIO_new_ssl_connect(ctx);
        if (bio == nullptr) {
            close();
            return "could not create the connection: " + openssl_error();
        }
        bio_ = bio;

        SSL* ssl = nullptr;
        BIO_get_ssl(bio, &ssl);
        if (ssl == nullptr) {
            close();
            return "could not reach the TLS session: " + openssl_error();
        }
        SSL_set_mode(ssl, SSL_MODE_AUTO_RETRY);
        // The two that matter: the name we asked for must be in the
        // certificate, and it must be sent so the server knows which one to
        // present.
        if (SSL_set1_host(ssl, options.host.c_str()) != 1 ||
            SSL_set_tlsext_host_name(ssl, options.host.c_str()) != 1) {
            close();
            return "could not set the TLS hostname: " + openssl_error();
        }
        BIO_set_conn_hostname(bio, where.c_str());
        if (!connect_bio(bio, true, bounded_io_, deadline)) {
            close();
            return "could not connect or finish TLS with " + where + ": " + openssl_error();
        }
        if (const long verified = SSL_get_verify_result(ssl); verified != X509_V_OK) {
            const std::string why = X509_verify_cert_error_string(verified);
            close();
            return "the server's certificate did not verify: " + why;
        }
        return {};
    }

    bio = BIO_new_connect(where.c_str());
    if (bio == nullptr) return "could not create the connection: " + openssl_error();
    bio_ = bio;
    if (!connect_bio(bio, false, bounded_io_, deadline)) {
        close();
        return "could not connect to " + where + ": " + openssl_error();
    }
    return {};
}

std::string TlsStream::write(std::string_view data) {
    if (bio_ == nullptr) return "not connected";
    auto* bio = static_cast<BIO*>(bio_);
    const auto deadline = std::chrono::steady_clock::now() + timeout_;
    std::size_t sent = 0;
    while (sent < data.size()) {
        const int wrote = BIO_write(bio, data.data() + sent, static_cast<int>(data.size() - sent));
        if (wrote > 0) {
            sent += static_cast<std::size_t>(wrote);
            continue;
        }
        if (BIO_should_retry(bio)) {
            if (!bounded_io_ || wait_for_bio(bio, deadline)) continue;
            return "timed out sending request";
        }
        return "could not send: " + openssl_error();
    }
    return {};
}

TlsStream::Read TlsStream::read(std::string& out, std::chrono::milliseconds wait,
                                std::string& error) {
    if (bio_ == nullptr) {
        error = "not connected";
        return Read::Error;
    }
    auto* bio = static_cast<BIO*>(bio_);

    // Buffered TLS records can hold a whole message with nothing left on the
    // socket, and poll() would then wait for bytes that have already arrived.
    int fd = -1;
    if (BIO_get_fd(bio, &fd) <= 0 || fd < 0) {
        error = "the connection has no socket";
        return Read::Error;
    }
    bool ready = BIO_pending(bio) > 0;
    if (!ready) {
        pollfd watch{fd, POLLIN, 0};
        const int got = ::poll(&watch, 1, static_cast<int>(wait.count()));
        if (got == 0) return Read::Timeout;
        if (got < 0) {
            if (errno == EINTR) return Read::Timeout;
            error = std::string("poll failed: ") + std::strerror(errno);
            return Read::Error;
        }
        ready = true;
    }

    char chunk[16384];
    const int got = BIO_read(bio, chunk, sizeof(chunk));
    if (got > 0) {
        out.append(chunk, static_cast<std::size_t>(got));
        return Read::Data;
    }
    if (BIO_should_retry(bio)) return Read::Timeout;
    if (got == 0) return Read::Closed;
    error = "the connection failed: " + openssl_error();
    return Read::Error;
}

}  // namespace cpm
