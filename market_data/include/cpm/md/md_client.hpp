#pragma once

#include "cpm/common/config.hpp"
#include "cpm/common/logging.hpp"
#include "cpm/md/events.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cpm::md {

struct SessionEvent {
    // DataLoss: a MsgSeqNum gap, so market data was lost. The session itself
    // may be perfectly healthy; nothing is resent (Q12, Q24).
    enum class Kind { LoggedOn, LoggedOut, DataLoss };
    Kind kind;
    std::string reason;  // LoggedOut: server Text(58) if any, else a local description
};

struct InstrumentRef {
    std::string symbol;       // Symbol(55)
    std::string security_id;  // SecurityID(48)
};

struct InstrumentList {
    std::vector<InstrumentRef> instruments;  // sorted by symbol
    std::string reject;                      // non-empty if the server rejected the request
};

// One market data channel group (design §7: one request per group).
enum class Channel {
    Book,       // 269=0,1: YES and NO bids. Per-instrument only; needs depth 1 or 10.
    Trades,     // 269=2
    VolumeOi,   // 269=B,C
    Lifecycle,  // 269=L (SecurityStatus f)
};

struct ChannelRequest {
    Channel channel;
    std::vector<std::string> symbols;  // empty = all instruments (not allowed for Book)
    int depth = 0;                     // Book only: 1 (L1) or 10 (L2)
};

// Market data callbacks. Each is optional; all are called on a QuickFIX
// thread, one message at a time, and must not block. Register them with
// on_market_data() before start().
struct MdCallbacks {
    std::function<void(const BookSnapshot&)> book_snapshot;
    std::function<void(const BookUpdate&)> book_update;
    std::function<void(const Trade&)> trade;
    std::function<void(const Volume&)> volume;
    std::function<void(const OpenInterest&)> open_interest;
    std::function<void(const InstrumentLifecycle&)> lifecycle;
    std::function<void(const MdReject&)> reject;
    std::function<void(const DecodeError&)> decode_error;
};

// Runs both sets of callbacks in order, for wiring several consumers (state,
// monitor, printing) into one MdClient. Every member is chained, including
// reject and decode_error.
MdCallbacks combine(MdCallbacks first, MdCallbacks second);

struct MdClientOptions {
    MdConfig config;
    std::string dict_dir;               // directory holding the config/ dictionaries
    int reconnect_interval_secs = 30;   // QuickFIX reconnect cadence after a drop
};

// One FIX session to the CPM market data gateway. RAII: the destructor logs out.
// Scope: logon, heartbeats, logout, subscription requests, instrument
// discovery, and decoding every channel into the plain structs of
// cpm/md/events.hpp. No book state is kept here; that is Stage 4.
class MdClient {
public:
    using SessionCallback = std::function<void(const SessionEvent&)>;

    // Throws ConfigError if the endpoint is not the sandbox or loopback.
    MdClient(MdClientOptions options, Logger& logger);
    ~MdClient();

    MdClient(const MdClient&) = delete;
    MdClient& operator=(const MdClient&) = delete;

    // Register before start(). Called on a QuickFIX thread.
    void on_session_event(SessionCallback callback);
    void on_market_data(MdCallbacks callbacks);

    void start();
    // Sends Logout and waits (QuickFIX: up to 10 s) for the server's reply.
    // Idempotent and safe from any thread, so a FeedMonitor halt handler and
    // the owner's shutdown path can both call it. Never call it from a
    // QuickFIX callback thread: it waits for the logout to complete.
    void stop();

    bool wait_logged_on(std::chrono::milliseconds timeout) const;
    bool logged_on() const;
    // Empty until the first logout/disconnect.
    std::string last_logout_reason() const;

    // Sends a subscribe (MarketDataRequest, 263=1) and returns its MDReqID.
    // Throws std::invalid_argument for a request the spec forbids, and
    // std::runtime_error if not logged on.
    std::string subscribe(const ChannelRequest& request);
    // Repeats the original request with 263=2 (design Q5). Returns false if
    // `md_req_id` is not an active subscription or the session is down.
    bool unsubscribe(const std::string& md_req_id);
    void unsubscribe_all();

    // Instrument discovery. Subscribes to trade volume + open interest for all
    // instruments (spec §4: 269=B,C with 146=0, which returns a W snapshot per
    // instrument), collects Symbol/SecurityID from the snapshots, then unsubscribes.
    // Returns once no new snapshot has arrived for `quiet`, on a reject, or at `timeout`.
    // Throws std::runtime_error if not logged on.
    InstrumentList list_instruments(std::chrono::milliseconds quiet, std::chrono::milliseconds timeout);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cpm::md
