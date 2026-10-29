#include "cpm/common/websocket.hpp"

#include "cpm/common/signing.hpp"

#include <openssl/rand.h>
#include <openssl/sha.h>

#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cctype>
#include <cerrno>
#include <cstring>

namespace cpm::ws {
namespace {

// RFC 6455 section 1.3. Fixed by the standard, not by the server.
constexpr std::string_view kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

std::string lowercase(std::string_view text) {
    std::string out(text);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

}  // namespace

std::string accept_key(std::string_view client_key) {
    const std::string material = std::string(client_key) + std::string(kGuid);
    std::array<unsigned char, SHA_DIGEST_LENGTH> digest{};
    SHA1(reinterpret_cast<const unsigned char*>(material.data()), material.size(), digest.data());
    return base64_encode(std::span<const std::uint8_t>(digest.data(), digest.size()));
}

std::string make_client_key() {
    std::array<std::uint8_t, 16> raw{};
    if (RAND_bytes(raw.data(), static_cast<int>(raw.size())) != 1) return {};
    return base64_encode(std::span<const std::uint8_t>(raw.data(), raw.size()));
}

std::string encode_frame(Opcode opcode, std::string_view payload, bool mask) {
    std::string out;
    out.push_back(static_cast<char>(0x80 | static_cast<std::uint8_t>(opcode)));  // FIN

    const std::size_t size = payload.size();
    const std::uint8_t mask_bit = mask ? 0x80 : 0x00;
    if (size < 126) {
        out.push_back(static_cast<char>(mask_bit | static_cast<std::uint8_t>(size)));
    } else if (size <= 0xFFFF) {
        out.push_back(static_cast<char>(mask_bit | 126));
        out.push_back(static_cast<char>((size >> 8) & 0xFF));
        out.push_back(static_cast<char>(size & 0xFF));
    } else {
        out.push_back(static_cast<char>(mask_bit | 127));
        for (int shift = 56; shift >= 0; shift -= 8) {
            out.push_back(static_cast<char>((size >> shift) & 0xFF));
        }
    }

    if (!mask) {
        out.append(payload);
        return out;
    }
    std::array<std::uint8_t, 4> key{};
    if (RAND_bytes(key.data(), static_cast<int>(key.size())) != 1) {
        // Not fatal to correctness - the mask is not a secret, it exists so
        // that a client's bytes cannot be made to look like a proxy's HTTP.
        for (std::size_t i = 0; i < key.size(); ++i) key[i] = static_cast<std::uint8_t>(i + 1);
    }
    for (const auto byte : key) out.push_back(static_cast<char>(byte));
    for (std::size_t i = 0; i < size; ++i) {
        out.push_back(static_cast<char>(static_cast<std::uint8_t>(payload[i]) ^ key[i % 4]));
    }
    return out;
}

Parse parse_frame(std::string& buffer, Frame& out, std::string& error) {
    if (buffer.size() < 2) return Parse::NeedMore;
    const auto byte = [&buffer](std::size_t at) {
        return static_cast<std::uint8_t>(buffer[at]);
    };

    const bool fin = (byte(0) & 0x80) != 0;
    const auto opcode = static_cast<Opcode>(byte(0) & 0x0F);
    const bool masked = (byte(1) & 0x80) != 0;
    std::uint64_t size = byte(1) & 0x7F;
    std::size_t at = 2;

    if (size == 126) {
        if (buffer.size() < at + 2) return Parse::NeedMore;
        size = (static_cast<std::uint64_t>(byte(at)) << 8) | byte(at + 1);
        at += 2;
    } else if (size == 127) {
        if (buffer.size() < at + 8) return Parse::NeedMore;
        size = 0;
        for (std::size_t i = 0; i < 8; ++i) size = (size << 8) | byte(at + i);
        at += 8;
        if ((size >> 63) != 0) {
            error = "a frame claiming a negative length";
            return Parse::Bad;
        }
    }

    std::array<std::uint8_t, 4> key{};
    if (masked) {
        if (buffer.size() < at + 4) return Parse::NeedMore;
        for (std::size_t i = 0; i < 4; ++i) key[i] = byte(at + i);
        at += 4;
    }
    if (buffer.size() < at + size) return Parse::NeedMore;

    out.fin = fin;
    out.opcode = opcode;
    out.payload.assign(buffer, at, static_cast<std::size_t>(size));
    if (masked) {
        for (std::size_t i = 0; i < out.payload.size(); ++i) {
            out.payload[i] = static_cast<char>(static_cast<std::uint8_t>(out.payload[i]) ^
                                               key[i % 4]);
        }
    }
    buffer.erase(0, at + static_cast<std::size_t>(size));
    return Parse::Ok;
}

std::string Client::connect(const Options& options) {
    close();
    max_message_ = options.max_message;

    TlsStream::Options transport;
    transport.host = options.host;
    transport.port = options.port;
    transport.tls = options.tls;
    transport.ca_file = options.ca_file;
    transport.connect_timeout = options.handshake_timeout;
    if (auto failed = stream_.connect(transport); !failed.empty()) {
        error_ = failed;
        return failed;
    }

    const std::string key = make_client_key();
    if (key.empty()) {
        close();
        error_ = "could not generate a WebSocket key";
        return error_;
    }
    std::string request = "GET " + options.path + " HTTP/1.1\r\n";
    request += "Host: " + options.host + "\r\n";
    request += "Upgrade: websocket\r\n";
    request += "Connection: Upgrade\r\n";
    request += "Sec-WebSocket-Key: " + key + "\r\n";
    request += "Sec-WebSocket-Version: 13\r\n\r\n";
    if (auto failed = stream_.write(request); !failed.empty()) {
        close();
        error_ = "could not send the WebSocket handshake: " + failed;
        return error_;
    }

    // The response head, and not a byte more: anything after it is already
    // frames, and throwing them away would lose the first message.
    const auto deadline = std::chrono::steady_clock::now() + options.handshake_timeout;
    std::string head;
    std::size_t end = std::string::npos;
    while (std::chrono::steady_clock::now() < deadline) {
        end = head.find("\r\n\r\n");
        if (end != std::string::npos) break;
        std::string why;
        const auto got = stream_.read(head, std::chrono::milliseconds(200), why);
        if (got == TlsStream::Read::Closed) {
            close();
            error_ = "the server closed during the WebSocket handshake";
            return error_;
        }
        if (got == TlsStream::Read::Error) {
            close();
            error_ = "the WebSocket handshake failed: " + why;
            return error_;
        }
        if (head.size() > 64u * 1024) {
            close();
            error_ = "the server's handshake response is implausibly large";
            return error_;
        }
    }
    if (end == std::string::npos) {
        close();
        error_ = "the server did not answer the WebSocket handshake in time";
        return error_;
    }

    const std::string response = head.substr(0, end);
    buffer_ = head.substr(end + 4);  // frames that arrived with the head

    if (response.find(" 101") == std::string::npos) {
        close();
        error_ = "the server refused the upgrade: " + response.substr(0, response.find("\r\n"));
        return error_;
    }
    const std::string folded = lowercase(response);
    const std::string expected = lowercase(accept_key(key));
    if (folded.find("sec-websocket-accept: " + expected) == std::string::npos) {
        close();
        error_ = "the server's Sec-WebSocket-Accept did not match the key we sent";
        return error_;
    }
    error_.clear();
    return {};
}

bool Client::handle(const Frame& frame, const std::function<void(std::string_view)>& on_text) {
    switch (frame.opcode) {
        case Opcode::Ping:
            // "as soon as possible", with the payload copied back. Binance
            // pings every 20 s and drops a connection that has not answered
            // within a minute.
            if (auto failed = stream_.write(encode_frame(Opcode::Pong, frame.payload, true));
                !failed.empty()) {
                error_ = "could not answer a ping: " + failed;
                return false;
            }
            return true;
        case Opcode::Pong:
            return true;
        case Opcode::Close:
            stream_.write(encode_frame(Opcode::Close, {}, true));
            error_ = "the server closed the connection";
            return false;
        case Opcode::Text:
        case Opcode::Binary:
            if (frame.fin) {
                if (frame.opcode == Opcode::Text && on_text) on_text(frame.payload);
                return true;
            }
            assembling_ = true;
            message_kind_ = frame.opcode;
            message_ = frame.payload;
            return true;
        case Opcode::Continuation:
            if (!assembling_) {
                error_ = "a continuation frame with nothing to continue";
                return false;
            }
            message_ += frame.payload;
            if (message_.size() > max_message_) {
                error_ = "a message larger than this client accepts";
                return false;
            }
            if (!frame.fin) return true;
            assembling_ = false;
            if (message_kind_ == Opcode::Text && on_text) on_text(message_);
            message_.clear();
            return true;
    }
    error_ = "a frame with an opcode this client does not know";
    return false;
}

Client::Pump Client::pump(std::chrono::milliseconds wait,
                          const std::function<void(std::string_view)>& on_text) {
    if (!stream_.connected()) {
        if (error_.empty()) error_ = "not connected";
        return Pump::Closed;
    }

    bool delivered = false;
    // Whatever arrived with the handshake, or was left over last time, is
    // already a frame and must not wait for the socket to say so.
    auto drain = [&]() {
        Frame frame;
        std::string why;
        while (true) {
            const auto parsed = parse_frame(buffer_, frame, why);
            if (parsed == Parse::NeedMore) return true;
            if (parsed == Parse::Bad) {
                error_ = why;
                return false;
            }
            const bool text = frame.opcode == Opcode::Text || frame.opcode == Opcode::Continuation;
            if (!handle(frame, on_text)) return false;
            if (text) delivered = true;
        }
    };

    if (!drain()) {
        close();
        return Pump::Closed;
    }

    std::string why;
    const auto got = stream_.read(buffer_, wait, why);
    if (got == TlsStream::Read::Closed) {
        error_ = "the server closed the connection";
        close();
        return Pump::Closed;
    }
    if (got == TlsStream::Read::Error) {
        error_ = why;
        close();
        return Pump::Closed;
    }
    if (buffer_.size() > max_message_ * 2) {
        error_ = "the server is sending more than this client can hold";
        close();
        return Pump::Closed;
    }
    if (!drain()) {
        close();
        return Pump::Closed;
    }
    return delivered ? Pump::Read : Pump::Idle;
}

std::string Client::send_text(std::string_view text) {
    if (!stream_.connected()) return "not connected";
    return stream_.write(encode_frame(Opcode::Text, text, true));
}

void Client::close() {
    stream_.close();
    buffer_.clear();
    message_.clear();
    assembling_ = false;
}

// ---- the other end -------------------------------------------------------

Server::~Server() { stop(); }

std::string Server::start(int port) {
    stop();
    listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener_ < 0) return std::string("could not create a socket: ") + std::strerror(errno);
    int yes = 1;
    ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    if (::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        const std::string why = std::strerror(errno);
        stop();
        return "could not bind: " + why;
    }
    if (::listen(listener_, 1) != 0) {
        const std::string why = std::strerror(errno);
        stop();
        return "could not listen: " + why;
    }
    socklen_t length = sizeof(address);
    if (::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) == 0) {
        port_ = ntohs(address.sin_port);
    }
    return {};
}

bool Server::accept(std::chrono::milliseconds wait, std::string& error) {
    if (listener_ < 0) {
        error = "not started";
        return false;
    }
    pollfd watch{listener_, POLLIN, 0};
    if (::poll(&watch, 1, static_cast<int>(wait.count())) <= 0) {
        error = "no client arrived";
        return false;
    }
    client_ = ::accept(listener_, nullptr, nullptr);
    if (client_ < 0) {
        error = std::string("could not accept: ") + std::strerror(errno);
        return false;
    }

    // The handshake, by the book: find the key, answer with its accept.
    std::string head;
    const auto deadline = std::chrono::steady_clock::now() + wait;
    while (head.find("\r\n\r\n") == std::string::npos) {
        if (std::chrono::steady_clock::now() > deadline) {
            error = "the client did not finish its handshake";
            drop();
            return false;
        }
        char chunk[2048];
        pollfd ready{client_, POLLIN, 0};
        if (::poll(&ready, 1, 100) <= 0) continue;
        const auto got = ::recv(client_, chunk, sizeof(chunk), 0);
        if (got <= 0) {
            error = "the client went away during its handshake";
            drop();
            return false;
        }
        head.append(chunk, static_cast<std::size_t>(got));
    }

    const std::string folded = lowercase(head);
    const auto at = folded.find("sec-websocket-key:");
    if (at == std::string::npos) {
        error = "the client sent no Sec-WebSocket-Key";
        drop();
        return false;
    }
    auto start_of = head.find_first_not_of(" \t", at + std::strlen("sec-websocket-key:"));
    const auto end_of = head.find("\r\n", start_of);
    const std::string key = head.substr(start_of, end_of - start_of);

    std::string response = "HTTP/1.1 101 Switching Protocols\r\n";
    response += "Upgrade: websocket\r\n";
    response += "Connection: Upgrade\r\n";
    response += "Sec-WebSocket-Accept: " + accept_key(key) + "\r\n\r\n";
    if (::send(client_, response.data(), response.size(), MSG_NOSIGNAL) < 0) {
        error = std::string("could not answer the handshake: ") + std::strerror(errno);
        drop();
        return false;
    }
    buffer_.clear();
    return true;
}

namespace {

std::string send_all(int fd, std::string_view data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const auto wrote = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (wrote <= 0) return std::string("could not send: ") + std::strerror(errno);
        sent += static_cast<std::size_t>(wrote);
    }
    return {};
}

}  // namespace

std::string Server::send_text(std::string_view text) {
    if (client_ < 0) return "no client";
    // A server never masks, which is the other half of the rule a client
    // must mask.
    return send_all(client_, encode_frame(Opcode::Text, text, false));
}

std::string Server::send_ping(std::string_view payload) {
    if (client_ < 0) return "no client";
    return send_all(client_, encode_frame(Opcode::Ping, payload, false));
}

std::string Server::send_close() {
    if (client_ < 0) return "no client";
    return send_all(client_, encode_frame(Opcode::Close, {}, false));
}

std::string Server::send_raw(std::string_view bytes) {
    if (client_ < 0) return "no client";
    return send_all(client_, bytes);
}

bool Server::pump(std::chrono::milliseconds wait,
                  const std::function<void(std::string_view)>& on_text) {
    if (client_ < 0) return false;
    pollfd watch{client_, POLLIN, 0};
    if (::poll(&watch, 1, static_cast<int>(wait.count())) > 0) {
        char chunk[4096];
        const auto got = ::recv(client_, chunk, sizeof(chunk), 0);
        if (got <= 0) {
            drop();
            return false;
        }
        buffer_.append(chunk, static_cast<std::size_t>(got));
    }
    Frame frame;
    std::string why;
    while (parse_frame(buffer_, frame, why) == Parse::Ok) {
        if (frame.opcode == Opcode::Pong) ++pongs_;
        if (frame.opcode == Opcode::Text && on_text) on_text(frame.payload);
        if (frame.opcode == Opcode::Close) {
            drop();
            return false;
        }
    }
    return client_ >= 0;
}

void Server::drop() {
    if (client_ >= 0) ::close(client_);
    client_ = -1;
    buffer_.clear();
}

void Server::stop() {
    drop();
    if (listener_ >= 0) ::close(listener_);
    listener_ = -1;
}

}  // namespace cpm::ws
