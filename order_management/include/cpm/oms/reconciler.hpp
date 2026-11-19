#pragma once

#include "cpm/oms/bookmark.hpp"
#include "cpm/oms/oe_client.hpp"
#include "cpm/oms/order_state.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace cpm::oms {

// Why the run is ending. Order entry is fail-stop like market data, but it
// cannot simply stop: resting orders would be left in the market with nobody
// watching. So the sequence is flatten, reconcile, then halt (OE-Q6).
struct Halt {
    enum class Reason {
        DataLoss,    // a gap fill: events were lost and are not resent
        Reject,      // any reject the session reports
        Disconnect,  // the session dropped or the server logged us out
        Replaced,    // a newer connection for this API key took over
        Anomaly,     // the order flow contradicted itself
    };

    Reason reason;
    std::string detail;
    Clock::time_point time{};
};

const char* to_string(Halt::Reason reason);

// What the reconciler is doing.
enum class RecoveryState {
    Starting,      // not logged on yet
    Reconciling,   // F1 sent, or a replay in flight; submission is blocked
    Live,          // reconciled, submission allowed
    Flattening,    // a mass cancel is out after a halt condition
    Halted,        // finished: flat as far as we can tell, and stopped
};

const char* to_string(RecoveryState state);

// Drives the recovery sequence of OE-Q6 over an OeClient and an OrderState.
//
// On every logon it reconciles before letting anything be submitted: F1 asks
// how far the exchange has got, and if that is ahead of our bookmark, F3
// replays the gap. On a halt condition it mass cancels, reconciles once more so
// the cancels are accounted for, and stops. It never resumes submitting.
class Reconciler {
public:
    struct Options {
        // How long to wait for F2, and for the replay to complete, before
        // giving up and halting anyway.
        std::chrono::milliseconds reconcile_timeout{std::chrono::seconds(10)};
        // Any anomaly in the order flow is treated as a halt condition.
        bool halt_on_anomaly = true;
        // Has this instrument's trading window already ended? Asked only
        // about an order rejected with OrdRejReason 44 (not allowed by
        // market state): a quote the close caught in flight is a race we
        // lost rather than anything we have lost track of, and halting the
        // daemon for it protects nothing - the order never existed and the
        // venue has already cancelled whatever was resting on the window it
        // closed. Every other reject, and a 44 on a window that is still
        // trading, still halts.
        //
        // A hook rather than a check here because order management does not
        // know what a ticker looks like - that is market data's, and the
        // two containers do not depend on each other. Unset means "never",
        // which is the behaviour this had before (OE-Q6).
        std::function<bool(const std::string& symbol)> window_has_ended;
        // This run's tag, for the mass-cancel ClOrdIDs raised from here
        // (cpm::oms::run_tag). Empty takes it from the clock, which is right
        // for anything that owns a Reconciler by itself; the daemon passes its
        // own so that every id in a run, whoever built it, names the same run.
        std::string run_tag;
    };

    // Called once, from whichever thread noticed. It must not stop the client:
    // it may run on the session's own callback thread, where stopping would
    // deadlock. Set a flag and let the owner's loop stop things.
    using HaltHandler = std::function<void(const Halt&)>;

    Reconciler(Options options, OeClient& client, OrderState& state, BookmarkFile bookmark,
               Logger& logger, HaltHandler on_halt);
    ~Reconciler();

    Reconciler(const Reconciler&) = delete;
    Reconciler& operator=(const Reconciler&) = delete;

    // Wire these into the client before it starts.
    OeClient::SessionCallback session_callback();
    OeCallbacks callbacks();
    OrderState::AnomalyHandler anomaly_handler();

    // Persists the newest ExecID the state has processed.
    void save_bookmark();

    RecoveryState state() const;
    std::optional<Halt> halt() const;
    bool submission_allowed() const;

    // Drives the timeouts; call periodically, or let run() do it.
    void tick(Clock::time_point now);
    void start();
    // After this nothing is raised: the owner's own logout would otherwise
    // arrive as a Disconnect and be mistaken for a failure.
    void stop();

private:
    void begin_reconcile();
    void raise(Halt::Reason reason, std::string detail);
    void deliver();
    void run();

    const Options options_;
    OeClient& client_;
    OrderState& state_;
    BookmarkFile bookmark_;
    Logger& logger_;
    const HaltHandler on_halt_;
    // "halt-<run tag>-", built once at construction.
    const std::string halt_prefix_;

    // Serialises save_bookmark. The file is written whole, so two threads
    // saving at once must not interleave, and a save that started earlier must
    // not land after a later one and put an older ClOrdID counter on disk.
    std::mutex save_mutex_;

    mutable std::mutex mutex_;
    RecoveryState state_value_ = RecoveryState::Starting;
    std::optional<Halt> halt_;
    bool delivered_ = false;
    // Only this class's own mass-cancel ids come from here, and only on a halt
    // or a reconnect - a handful in a run. Nothing persists it: the run tag in
    // halt_prefix_ is what keeps them unique across a restart.
    std::uint64_t halt_counter_ = 0;
    std::string bookmarked_exec_id_;
    std::string newest_at_exchange_;
    Clock::time_point phase_started_{};
    bool replay_requested_ = false;
    bool stopping_ = false;    // stop() has been called; raise nothing further
    bool flatten_sent_ = false;

    std::atomic<bool> running_{false};
    std::mutex wake_mutex_;
    std::condition_variable wake_;
    std::thread thread_;
};

}  // namespace cpm::oms
