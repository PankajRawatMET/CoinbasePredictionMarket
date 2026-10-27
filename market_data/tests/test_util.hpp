#pragma once

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <ostream>
#include <streambuf>
#include <string>
#include <thread>

namespace cpm::md::test {

inline std::uint16_t free_port() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    socklen_t len = sizeof(addr);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    ::close(fd);
    return ntohs(addr.sin_port);
}

template <typename Pred>
bool eventually(Pred pred, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return pred();
}

inline bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// An ostream whose contents can be read while other threads write to it
// (cpm::Logger serialises writers, but not a reader of an ostringstream).
class SharedSink : private std::streambuf, public std::ostream {
public:
    SharedSink() : std::ostream(this) {}

    std::string text() const {
        std::lock_guard lock(mutex_);
        return text_;
    }

private:
    int overflow(int c) override {
        if (c != std::char_traits<char>::eof()) {
            std::lock_guard lock(mutex_);
            text_.push_back(static_cast<char>(c));
        }
        return c;
    }
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        std::lock_guard lock(mutex_);
        text_.append(s, static_cast<std::size_t>(n));
        return n;
    }

    mutable std::mutex mutex_;
    std::string text_;
};

}  // namespace cpm::md::test
