#pragma once

#include "cpm/md/md_client.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace cpm::md {

// Why the feed stopped being trustworthy. A silent book can be handled
// separately from terminal session and feed-integrity failures.
struct FeedHalt {
    enum class Reason {
        Gap,         // a MsgSeqNum gap: market data was lost
        StaleFeed,   // a live instrument's book went silent (Q25)
        Reject,      // any MarketDataRequestReject or BusinessMessageReject (Q27)
        Disconnect,  // the session dropped or the server logged us out (Q28)
    };

    Reason reason;
    std::string md_req_id;  // the subscription concerned, if the trigger named one
    std::string symbol;
    std::string detail;
    Clock::time_point time{};
};

const char* to_string(FeedHalt::Reason reason);

// Watches one MdClient's feed and reports the first condition that makes the
// data untrustworthy. It never resubscribes and never reconnects.
//
// Staleness is only meaningful for book subscriptions. Trades can legitimately
// be silent for minutes on a thin market (O3), and the lifecycle channel only
// speaks at window boundaries, minutes apart (O6), so a timeout on those would
// fire constantly. A book subscription is also exempt once every instrument it
// covers is Closed, Determined or Settled: those are silent by design (O10),
// and without this the strategy would halt at every window roll.
class FeedMonitor {
public:
    struct Options {
        // How long a live instrument's book may be silent (Q25).
        std::chrono::milliseconds stale_after{std::chrono::seconds(30)};
        // How often the driver thread re-checks.
        std::chrono::milliseconds poll{250};
        // Owners with independent strategies can handle stale books locally.
        bool halt_on_stale = true;
    };

    using HaltHandler = std::function<void(const FeedHalt&)>;
    using ClockFn = std::function<Clock::time_point()>;

    // `on_halt` is called once, from tick() or from the driver thread, never
    // from a QuickFIX callback, so it may stop the client.
    FeedMonitor(Options options, HaltHandler on_halt, ClockFn clock = {});
    ~FeedMonitor();

    FeedMonitor(const FeedMonitor&) = delete;
    FeedMonitor& operator=(const FeedMonitor&) = delete;

    // Feed these into MdClient. The market data callbacks only stamp arrival
    // times and watch for rejects; they do not copy any market data.
    MdCallbacks callbacks();
    MdClient::SessionCallback session_callback();

    // Register each subscription as it is created, and drop it when it is
    // unsubscribed, so staleness is judged per subscription.
    void track(const std::string& md_req_id, const ChannelRequest& request);
    void forget(const std::string& md_req_id);
    // Model-based strategies tolerate an unchanged book. This exempts only
    // silence, never session failure, sequence gaps or rejected subscriptions.
    void allow_quiet_books(std::vector<std::string> symbols);

    // Runs the staleness check on a timer thread until stop(). A clean
    // shutdown stops the monitor first, so the logout it causes is not
    // mistaken for a disconnect.
    void start();
    void stop();

    // One check, for tests and for the driver thread.
    void tick(Clock::time_point now);

    // Age of a silent single-instrument book, when it has timed out.
    std::optional<std::chrono::seconds> stale_book(const std::string& symbol,
                                                   Clock::time_point now) const;

    bool halted() const;
    std::optional<FeedHalt> halt() const;

private:
    struct Subscription {
        Channel channel = Channel::Book;
        std::vector<std::string> symbols;  // empty means all instruments
        int depth = 0;
        Clock::time_point last_message{};
    };

    // Records the first halt. While the driver thread runs it wakes that
    // thread to deliver it, so a QuickFIX callback never runs the handler.
    // After stop() nothing is delivered at all: the owner is already shutting
    // down, and the logout its own stop() causes would otherwise come back as
    // a Disconnect on the session's thread and re-enter the handler there.
    void raise(FeedHalt::Reason reason, std::string md_req_id, std::string symbol, std::string detail,
               Clock::time_point now);
    // Calls on_halt_ exactly once.
    void deliver();
    bool delivery_pending() const;
    void stamp(const std::string& md_req_id, Clock::time_point time);
    // True when every instrument the subscription covers has finished trading.
    bool all_instruments_finished(const Subscription& subscription) const;
    void run();

    const Options options_;
    const HaltHandler on_halt_;
    const ClockFn clock_;

    mutable std::mutex mutex_;
    std::map<std::string, Subscription> subscriptions_;
    std::map<std::string, LifecycleStatus> statuses_;
    std::vector<std::string> quiet_books_;
    std::optional<FeedHalt> halt_;
    bool delivered_ = false;
    bool logged_on_ = false;

    // Fresh: no driver thread, so a halt is delivered inline (unit tests).
    // Running: the driver thread delivers it. Stopped: it is only recorded.
    enum class Phase { Fresh, Running, Stopped };
    std::atomic<Phase> phase_{Phase::Fresh};
    std::atomic<bool> running_{false};
    std::mutex wake_mutex_;
    std::condition_variable wake_;
    std::thread thread_;
};

}  // namespace cpm::md
