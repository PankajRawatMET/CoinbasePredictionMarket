#pragma once

#include "cpm/common/config.hpp"
#include "cpm/common/logging.hpp"
#include "cpm/oms/orders.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace cpm::oms {

struct SessionEvent {
    enum class Kind {
        LoggedOn,
        LoggedOut,
        // A second Logon for the same API key displaced this session: the
        // gateway sends Logout(5) with "replaced by newer connection". The API
        // key is still live, so cancel-on-disconnect does NOT fire and this
        // session's resting orders stay in the market under the new connection.
        Replaced,
        // A gap fill: the session's sequence was repaired but the business
        // events in it are gone. EventResendRequest(F3) is the repair, keyed on
        // ExecID; ResendRequest(2) only fixes the sequence.
        DataLoss,
    };

    Kind kind;
    std::string reason;  // server Text(58) if any, else a local description
};

// Decoded inbound events. Each is optional and runs on a QuickFIX thread, one
// message at a time, so none of them may block.
struct OeCallbacks {
    std::function<void(const ExecutionReport&)> report;
    std::function<void(const CancelReject&)> cancel_reject;
    std::function<void(const MassCancelReport&)> mass_cancel_report;
    std::function<void(const SettlementReport&)> settlement;
    std::function<void(const LastExecId&)> last_exec_id;
    std::function<void(const ResendComplete&)> resend_complete;
    std::function<void(const ResendReject&)> resend_reject;
    std::function<void(const BusinessReject&)> business_reject;
    std::function<void(const DecodeError&)> decode_error;
};

// Client-side limits, checked before anything reaches the wire. They are not a
// risk engine: they are the cheap guards that stop an obvious mistake becoming
// an order, and they are deliberately not derived from anything the gateway
// says, so a venue change cannot silently widen them.
struct OrderLimits {
    // Nothing is sent unless this is true. A process that is only meant to
    // observe therefore cannot place an order even if asked to.
    bool allow_orders = false;
    Qty max_quantity = 0;   // per order, 0 = no cap
    // quantity x price per order, in amount scale. For a market order the
    // worst admissible price is assumed, since the real one is unknown.
    Amount max_notional = 0;
    // Orders per second. Cancels are exempt, as they are at the gateway.
    double max_orders_per_sec = 0;
};

// Runs both sets of callbacks in order, for wiring several consumers (the
// order state, the reconciler, printing) into one client.
OeCallbacks combine_oe(OeCallbacks first, OeCallbacks second);

struct OeClientOptions {
    OeConfig config;
    std::string dict_dir;              // directory holding the config/ dictionaries
    int reconnect_interval_secs = 30;  // QuickFIX reconnect cadence after a drop
};

// One FIX session to the CPM order entry gateway. RAII: the destructor logs out.
//
// Stage 1 scope: a signed Logon carrying the session options, heartbeats and a
// clean logout. It sends no orders, and inbound application messages are
// visible only in the raw message log until Stage 3 decodes them.
class OeClient {
public:
    using SessionCallback = std::function<void(const SessionEvent&)>;

    // Throws ConfigError if the endpoint is not the order entry sandbox or loopback.
    OeClient(OeClientOptions options, Logger& logger);
    ~OeClient();

    OeClient(const OeClient&) = delete;
    OeClient& operator=(const OeClient&) = delete;

    // Register before start(). Called on a QuickFIX thread.
    void on_session_event(SessionCallback callback);
    void on_events(OeCallbacks callbacks);
    // Consulted before every order: send() refuses while it returns false.
    // Cancels and the recovery requests are never gated - stopping and
    // reconciling must always be possible.
    void set_submission_gate(std::function<bool()> gate);
    // May be changed at any time; the next request sees the new limits.
    void set_limits(OrderLimits limits);

    void start();
    // Sends Logout and waits for the server's reply. Idempotent and safe from
    // any thread, but never from a QuickFIX callback: it waits for the logout.
    void stop();

    // Sends NewOrderSingle(D). Returns the ClOrdID that went on the wire.
    // Throws std::invalid_argument when the order breaks a rule the gateway
    // enforces on receipt or one of the configured limits, and
    // std::runtime_error when the session cannot carry it. Nothing is sent in
    // either case.
    std::string send(const NewOrder& order);
    // OrderCancelRequest(F) and OrderMassCancelRequest(q). Cancels bypass the
    // rate limit and the allow_orders switch: stopping a run must always be
    // possible, even from a process that may not open a position.
    bool cancel(const CancelOrder& cancel);
    bool mass_cancel(const MassCancel& request);

    // The recovery pair: LastExecIDRequest(F1) and EventResendRequest(F3).
    bool request_last_exec_id();
    bool request_event_resend(const std::string& begin_exec_id, const std::string& end_exec_id = {});

    bool wait_logged_on(std::chrono::milliseconds timeout) const;
    bool logged_on() const;
    // Empty until the first logout or disconnect.
    std::string last_logout_reason() const;
    // True once this session has been displaced by a newer connection.
    bool replaced() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cpm::oms
