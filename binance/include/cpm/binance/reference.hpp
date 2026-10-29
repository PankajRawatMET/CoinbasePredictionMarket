#pragma once

#include "cpm/common/logging.hpp"
#include "cpm/common/websocket.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

// The reference price: where the underlying is, according to Binance.
//
// It is not a trading feed. Nothing here places an order, holds a credential
// or signs anything - it reads one public stream and keeps the latest touch
// per symbol, so a strategy can tell whether the world has moved since it
// priced (docs/design-binance.md).
namespace cpm::binance {

using Clock = std::chrono::steady_clock;

// Binance sends prices as decimal strings with up to eight decimals, and
// they stay integers from there on: no price in this project passes through
// a double (B-Q12).
inline constexpr int kScale = 8;
using Price = std::int64_t;

// What a strategy is given. Deliberately flat: it says what is known and how
// old it is, and the strategy decides what that means.
struct ReferenceView {
    bool present = false;  // a price has arrived at some point
    bool stale = true;     // nothing recently enough to rely on
    Price mid = 0;
    Price tick_size = 0;  // PRICE_FILTER.tickSize; 0 when it is not known
    std::chrono::milliseconds age{0};

    // What the guard asks: a number it can compare a move against.
    bool usable() const { return present && !stale && tick_size > 0; }
};

// The `<symbol>@bookTicker` payload, or the combined-stream envelope around
// one. Exposed because it is the whole of the parsing and deserves a test
// that needs no socket.
struct BookTicker {
    std::string symbol;
    Price bid = 0;
    Price ask = 0;

    Price mid() const { return (bid + ask) / 2; }
};
std::optional<BookTicker> parse_book_ticker(std::string_view json);

class ReferenceFeed {
public:
    struct Options {
        // One line in config/binance.conf - url = wss://fstream.binance.com/ws
        // - taken apart by parse_ws_url. No URL in this project is a
        // literal in the source (B-Q15).
        std::string host = "fstream.binance.com";
        int port = 443;
        bool tls = true;
        // No streams in the path: they are subscribed on the live
        // connection as strategies start and stop (B-Q14).
        std::string path = "/ws";
        std::string ca_file;
        // Subscribed for the whole run whatever the strategies do. Usually
        // empty: a stream is normally wanted only while a strategy that
        // needs it is running, which is what want() below is for (B-Q14).
        std::vector<std::string> symbols;
        // Control messages - SUBSCRIBE and UNSUBSCRIBE - per second. The
        // venue allows five INCOMING messages a second on a connection and
        // counts our pongs among them, and an IP that is repeatedly
        // disconnected for exceeding it may be banned. Two a second is
        // more than an operator pressing buttons can need and leaves the
        // rest of the budget alone.
        int control_per_sec = 2;
        // Nothing for this long and the price is not to be relied on. The
        // BTCUSDT touch changes many times a second, so five seconds of
        // silence is a dead feed rather than a quiet market (B-Q4).
        std::chrono::milliseconds stale_after{5000};
        std::chrono::milliseconds reconnect_after{1000};
        std::chrono::milliseconds max_reconnect_after{30000};
        // Each symbol's tick size, which is what PricingMidMoveThreshold
        // counts in. It comes from the symbol mapping in the config file
        // and from nowhere else: this project makes no REST call to
        // Binance at all (B-Q17).
        std::map<std::string, Price> tick_size;
    };

    ReferenceFeed(Options options, Logger& logger);
    ~ReferenceFeed();

    ReferenceFeed(const ReferenceFeed&) = delete;
    ReferenceFeed& operator=(const ReferenceFeed&) = delete;

    // The streams that should be up, as a set. The reader thread works out
    // what to subscribe and what to unsubscribe and sends the difference,
    // batched into one message each way - `params` is an array, so N
    // symbols cost one message, not N (B-Q14).
    //
    // Declarative on purpose: the caller says what it wants rather than
    // counting references, so a missed decrement cannot leak a stream and
    // a reconnect is just "want everything again".
    void want(const std::vector<std::string>& symbols);
    // What the connection is actually carrying right now.
    std::vector<std::string> subscribed() const;

    // Starts the reader thread. Returns a reason only when it could not be
    // started at all; a feed that cannot connect yet is not an error, it is
    // a feed that is stale until it can.
    std::string start();
    void stop();

    // Called on the reader thread whenever a price arrives. It must do
    // almost nothing - the daemon's is a write to a pipe - because it runs
    // between a frame and the next read, and because the thread that calls
    // it must never touch strategy state (design-binance.md section 6).
    void on_update(std::function<void()> waker);

    ReferenceView latest(const std::string& symbol, Clock::time_point now) const;
    bool connected() const { return connected_.load(); }
    // Every symbol this feed was asked for, for the operator's pill.
    const std::vector<std::string>& symbols() const { return options_.symbols; }

private:
    void run();
    void apply(const BookTicker& tick);
    // Sends at most one SUBSCRIBE and one UNSUBSCRIBE, inside the budget.
    // Returns false when the connection failed under it.
    bool reconcile_streams(cpm::ws::Client& client, Clock::time_point now);
    void handle_control(std::string_view text);

    struct Entry {
        Price mid = 0;
        Clock::time_point at{};
        bool present = false;
    };

    Options options_;
    Logger& logger_;
    mutable std::mutex mutex_;
    std::map<std::string, Entry> latest_;
    // What someone has asked for, and what the connection is carrying. The
    // reader thread moves the second towards the first.
    std::set<std::string> wanted_;
    std::set<std::string> live_;
    Clock::time_point last_control_{};
    std::uint64_t control_id_ = 0;
    std::function<void()> waker_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
};

}  // namespace cpm::binance
