#pragma once

// The fakes the GUI tests drive: a ControlApi over stand-ins for both
// sessions, and the smallest HTTP client that can hold a stream open. Shared
// by the server tests and the page tests, so what the page is checked against
// is the same daemon the server is checked against.
#include "cpm/gui/server.hpp"

#include "cpm/md/ticks.hpp"
#include "cpm/strategy/crypto_mm.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace cpm::gui::test {

using cpm::gui::Server;
using cpm::md::InstrumentView;
using cpm::md::LifecycleStatus;
using cpm::strategy::StrategyHooks;
using cpm::strategy::StrategyHost;
using cpm::strategy::StrategyRegistry;
using cpm::trader::BookView;
using cpm::trader::ControlApi;
using cpm::trader::LimitsView;
using cpm::trader::PositionView;
using cpm::trader::SessionView;

inline const std::string kSym = "SIMOE-26NOV01HOMAWY-HOM";
inline const std::string kOther = "SIMOE-26NOV01HOMAWY-AWY";

// A ControlApi over fakes: the same shape as trader/tests/control_api_test.cpp,
// because what the GUI drives must be the thing the daemon drives.
struct Daemon {
    Daemon() {
        cpm::strategy::register_crypto_mm(registry);

        StrategyHooks venue;
        venue.can_submit = [] { return true; };
        venue.orders_permitted = [] { return true; };
        venue.send = [](const cpm::oms::NewOrder& order) { return order.cl_ord_id; };
        venue.cancel = [](const cpm::oms::CancelOrder&) { return true; };
        venue.next_cl_ord_id = [this] { return "mm-" + std::to_string(++counter); };
        venue.log = [](const std::string&) {};

        StrategyHost::Feeds feeds;
        feeds.book = [](const std::string& symbol) -> std::optional<InstrumentView> {
            InstrumentView view;
            view.symbol = symbol;
            view.book_valid = true;
            view.status = LifecycleStatus::Open;
            // Three rungs a side, best first, as MarketState orders them.
            view.yes.push_back(cpm::md::BookLevel{500, 500});
            view.yes.push_back(cpm::md::BookLevel{490, 700});
            view.yes.push_back(cpm::md::BookLevel{480, 900});
            view.no.push_back(cpm::md::BookLevel{400, 500});
            view.no.push_back(cpm::md::BookLevel{390, 600});
            view.no.push_back(cpm::md::BookLevel{380, 800});
            return view;
        };
        feeds.position = [](const std::string& symbol) {
            cpm::oms::Position position;
            position.symbol = symbol;
            return position;
        };
        host = std::make_unique<StrategyHost>(StrategyHost::Options{}, registry, std::move(venue),
                                              std::move(feeds), logger);

        ControlApi::Env env;
        env.market_data = [] { return SessionView{true, ""}; };
        env.order_entry = [] { return SessionView{true, ""}; };
        env.recovery = [] { return std::string("live"); };
        env.positions = [this] { return positions; };
        env.pnl = [this] { return pnl; };
        env.limits = [] {
            LimitsView limits;
            limits.allow_orders = true;
            limits.max_quantity = 1000;
            limits.max_notional = 10'000'000;
            limits.max_orders_per_sec = 8;
            return limits;
        };
        env.book = [](const std::string& symbol) -> std::optional<BookView> {
            BookView view;
            view.symbol = symbol;
            view.valid = true;
            view.best_yes_bid = 500;
            view.best_yes_bid_qty = 500;
            view.implied_yes_ask = 600;
            view.best_no_bid_qty = 500;
            view.mid = 550;
            view.spread = 100;
            view.status = "open";
            // The same book as a ladder: YES bids as they are, NO bids turned
            // into the YES asks they imply (Q9), both best first.
            view.bids = {{500, 500, 500}, {490, 700, 490}, {480, 900, 480}};
            view.asks = {{600, 500, 400}, {610, 600, 390}, {620, 800, 380}};
            return view;
        };
        env.axis = [](const std::string& symbol) {
            cpm::trader::AxisView axis;
            axis.symbol = symbol;
            axis.structure = cpm::md::to_string(cpm::md::TickStructure::TaperedDeciCent);
            axis.prices = cpm::md::price_grid(cpm::md::TickStructure::TaperedDeciCent);
            return axis;
        };
        env.subscribe = [](const std::string&) { return std::string(); };
        env.unsubscribe = [](const std::string&) {};

        api = std::make_unique<ControlApi>(*host, registry, std::move(env), true);
    }

    StrategyRegistry registry;
    std::ostringstream sink;
    cpm::Logger logger{sink, cpm::LogLevel::Error};
    std::unique_ptr<StrategyHost> host;
    std::unique_ptr<ControlApi> api;
    std::vector<PositionView> positions;
    std::vector<cpm::trader::PnlView> pnl;
    std::uint64_t counter = 0;
};

// The smallest HTTP client that can hold a stream open: blocking, with a
// timeout, so a test that would hang fails instead.
class Client {
public:
    ~Client() { disconnect(); }

    bool connect(int port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) return false;
        timeval timeout{2, 0};
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(static_cast<std::uint16_t>(port));
        return ::connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
    }

    void disconnect() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }

    bool write(const std::string& bytes) {
        return ::send(fd_, bytes.data(), bytes.size(), 0) == static_cast<ssize_t>(bytes.size());
    }

    // Reads until the headers are in and then exactly Content-Length bytes.
    std::string response() {
        const auto head_end = read_until("\r\n\r\n");
        if (head_end == std::string::npos) return buffer_;
        const auto length = content_length(buffer_.substr(0, head_end));
        while (buffer_.size() < head_end + 4 + length) {
            if (!pump()) break;
        }
        const auto whole = buffer_.substr(0, head_end + 4 + length);
        buffer_.erase(0, whole.size());
        return whole;
    }

    // One SSE event, including its blank-line terminator.
    std::string event() {
        const auto end = read_until("\n\n");
        if (end == std::string::npos) return {};
        const auto whole = buffer_.substr(0, end + 2);
        buffer_.erase(0, end + 2);
        return whole;
    }

    std::string drain_headers() {
        const auto end = read_until("\r\n\r\n");
        if (end == std::string::npos) return {};
        const auto head = buffer_.substr(0, end + 4);
        buffer_.erase(0, end + 4);
        return head;
    }

    // True if the server closed this connection within `ms`. Data arriving in
    // the meantime is kept and does not count as a close, so this answers the
    // question it is asked even on a stream that is still beating.
    bool closed_within(int ms) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (true) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  deadline - std::chrono::steady_clock::now())
                                  .count();
            if (left <= 0) return false;
            pollfd watch{fd_, POLLIN, 0};
            if (::poll(&watch, 1, static_cast<int>(left)) <= 0) continue;
            char probe[4096];
            const ssize_t got = ::recv(fd_, probe, sizeof(probe), 0);
            if (got > 0) {
                buffer_.append(probe, static_cast<std::size_t>(got));
                continue;
            }
            return true;
        }
    }

private:
    bool pump() {
        char chunk[4096];
        const ssize_t got = ::recv(fd_, chunk, sizeof(chunk), 0);
        if (got <= 0) return false;
        buffer_.append(chunk, static_cast<std::size_t>(got));
        return true;
    }

    std::size_t read_until(const std::string& marker) {
        while (true) {
            const auto at = buffer_.find(marker);
            if (at != std::string::npos) return at;
            if (!pump()) return std::string::npos;
        }
    }

    static std::size_t content_length(const std::string& head) {
        const std::string name = "Content-Length: ";
        const auto at = head.find(name);
        if (at == std::string::npos) return 0;
        return static_cast<std::size_t>(std::stoul(head.substr(at + name.size())));
    }

    int fd_ = -1;
    std::string buffer_;
};

inline int status_of(const std::string& response) {
    if (response.size() < 12) return 0;
    return std::stoi(response.substr(9, 3));
}

inline std::string body_of(const std::string& response) {
    const auto at = response.find("\r\n\r\n");
    return at == std::string::npos ? std::string() : response.substr(at + 4);
}


}  // namespace cpm::gui::test
