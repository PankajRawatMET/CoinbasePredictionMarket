#include "cpm/oms/reconciler.hpp"

#include "cpm/oms/order_rules.hpp"

namespace cpm::oms {

const char* to_string(Halt::Reason reason) {
    switch (reason) {
        case Halt::Reason::DataLoss: return "data loss";
        case Halt::Reason::Reject: return "reject";
        case Halt::Reason::Disconnect: return "disconnect";
        case Halt::Reason::Replaced: return "replaced by a newer connection";
        case Halt::Reason::Anomaly: return "order flow anomaly";
    }
    return "unknown";
}

const char* to_string(RecoveryState state) {
    switch (state) {
        case RecoveryState::Starting: return "starting";
        case RecoveryState::Reconciling: return "reconciling";
        case RecoveryState::Live: return "live";
        case RecoveryState::Flattening: return "flattening";
        case RecoveryState::Halted: return "halted";
    }
    return "unknown";
}

Reconciler::Reconciler(Options options, OeClient& client, OrderState& state, BookmarkFile bookmark,
                       Logger& logger, HaltHandler on_halt)
    : options_(options),
      client_(client),
      state_(state),
      bookmark_(std::move(bookmark)),
      logger_(logger),
      on_halt_(std::move(on_halt)) {
    // A missing bookmark is a first run. A corrupt one throws, because
    // restarting from zero would replay nothing and silently lose history.
    if (const auto stored = bookmark_.read()) {
        cl_ord_counter_ = stored->cl_ord_counter;
        bookmarked_exec_id_ = stored->last_exec_id;
        logger_.info("bookmark " + bookmark_.path() + ": last ExecID " +
                     (bookmarked_exec_id_.empty() ? "<none>" : bookmarked_exec_id_) +
                     ", ClOrdID counter " + std::to_string(cl_ord_counter_));
    } else {
        logger_.info("no bookmark at " + bookmark_.path() + ": starting fresh");
    }
}

Reconciler::~Reconciler() { stop(); }

std::uint64_t Reconciler::cl_ord_counter() const {
    std::lock_guard lock(mutex_);
    return cl_ord_counter_;
}

void Reconciler::note_cl_ord_counter(std::uint64_t counter) {
    {
        std::lock_guard lock(mutex_);
        if (counter <= cl_ord_counter_) return;
        cl_ord_counter_ = counter;
    }
    save_bookmark();
}

void Reconciler::save_bookmark() {
    Bookmark bookmark;
    {
        std::lock_guard lock(mutex_);
        bookmark.cl_ord_counter = cl_ord_counter_;
    }
    // The state's newest ExecID is the authority: it is what has actually been
    // processed, which is what a replay must resume from.
    bookmark.last_exec_id = state_.last_exec_id();
    if (bookmark.last_exec_id.empty()) {
        std::lock_guard lock(mutex_);
        bookmark.last_exec_id = bookmarked_exec_id_;
    }
    try {
        bookmark_.write(bookmark);
    } catch (const std::exception& e) {
        // Losing the bookmark is serious but not a reason to abandon a live
        // session, so it is reported and the run continues.
        logger_.error(std::string("cannot save the bookmark: ") + e.what());
    }
}

OeClient::SessionCallback Reconciler::session_callback() {
    return [this](const SessionEvent& event) {
        switch (event.kind) {
            case SessionEvent::Kind::LoggedOn: begin_reconcile(); break;
            case SessionEvent::Kind::Replaced:
                raise(Halt::Reason::Replaced, event.reason);
                break;
            case SessionEvent::Kind::LoggedOut:
                raise(Halt::Reason::Disconnect, event.reason);
                break;
        }
    };
}

OeCallbacks Reconciler::callbacks() {
    OeCallbacks callbacks;
    // F2: how far the exchange has got. Ahead of our bookmark means we missed
    // events, so they are replayed before anything may be submitted.
    callbacks.last_exec_id = [this](const LastExecId& event) {
        std::string begin;
        {
            std::lock_guard lock(mutex_);
            newest_at_exchange_ = event.exec_id;
            begin = bookmarked_exec_id_;
        }
        const bool nothing_retained = event.exec_id.empty() || event.exec_id == "0";
        if (nothing_retained || begin.empty()) {
            // Nothing to replay: either the window is empty or we have no
            // bookmark to resume from.
            {
                std::lock_guard lock(mutex_);
                if (state_value_ == RecoveryState::Reconciling) {
                    state_value_ = halt_ ? RecoveryState::Halted : RecoveryState::Live;
                }
            }
            logger_.info(nothing_retained ? "nothing in the exchange's retention window"
                                          : "no bookmark to replay from; starting live");
            return;
        }
        if (begin == event.exec_id) {
            std::lock_guard lock(mutex_);
            if (state_value_ == RecoveryState::Reconciling) {
                state_value_ = halt_ ? RecoveryState::Halted : RecoveryState::Live;
            }
            logger_.info("up to date with the exchange at ExecID " + begin);
            return;
        }
        logger_.warn("behind the exchange: ours " + begin + ", theirs " + event.exec_id +
                     " - replaying");
        {
            std::lock_guard lock(mutex_);
            replay_requested_ = true;
            phase_started_ = Clock::now();
        }
        if (!client_.request_event_resend(begin)) {
            raise(Halt::Reason::DataLoss, "could not request a replay from ExecID " + begin);
        }
    };
    callbacks.resend_complete = [this](const ResendComplete& event) {
        logger_.info("replay complete: " + std::to_string(event.event_count) + " event(s)");
        save_bookmark();
        std::lock_guard lock(mutex_);
        replay_requested_ = false;
        if (state_value_ == RecoveryState::Reconciling) {
            // A replay that completes after a halt leaves the run stopped, not
            // live: nothing is ever submitted again (OE-Q6).
            state_value_ = halt_ ? RecoveryState::Halted : RecoveryState::Live;
        }
    };
    callbacks.resend_reject = [this](const ResendReject& event) {
        // Reason 1 means our bookmark predates the window: history is gone, so
        // there is no way to rebuild and the run must stop.
        raise(Halt::Reason::DataLoss, "replay rejected, reason " + std::to_string(event.reason) +
                                          (event.text.empty() ? "" : ": " + event.text));
    };
    // Any reject halts (OE-Q27 for market data's rule, OE-Q6 here).
    callbacks.cancel_reject = [this](const CancelReject& event) {
        raise(Halt::Reason::Reject, "OrderCancelReject for " + event.cl_ord_id + ": " + event.text);
    };
    callbacks.business_reject = [this](const BusinessReject& event) {
        raise(Halt::Reason::Reject, "BusinessMessageReject for 35=" + event.ref_msg_type + ": " +
                                        event.text);
    };
    callbacks.report = [this](const ExecutionReport& report) {
        // A rejected order is a reject like any other.
        if (report.exec_type == ExecType::Rejected) {
            raise(Halt::Reason::Reject, "order " + report.cl_ord_id + " rejected: " + report.text);
            return;
        }
        // Each processed event moves the bookmark.
        if (!report.replayed) save_bookmark();
    };
    return callbacks;
}

OrderState::AnomalyHandler Reconciler::anomaly_handler() {
    return [this](const OrderAnomaly& anomaly) {
        logger_.warn(std::string("order anomaly: ") + to_string(anomaly.kind) +
                     (anomaly.cl_ord_id.empty() ? "" : " on " + anomaly.cl_ord_id) +
                     (anomaly.detail.empty() ? "" : " (" + anomaly.detail + ")"));
        if (options_.halt_on_anomaly) {
            raise(Halt::Reason::Anomaly, std::string(to_string(anomaly.kind)) + ": " + anomaly.detail);
        }
    };
}

void Reconciler::begin_reconcile() {
    bool flatten_now = false;
    {
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        state_value_ = RecoveryState::Reconciling;
        phase_started_ = Clock::now();
        replay_requested_ = false;
        // A halt raised while the session was down could not flatten then, so
        // the reconnect is the first chance to do it (OE-Q6).
        if (halt_ && !flatten_sent_) {
            flatten_sent_ = true;
            flatten_now = true;
        }
    }
    if (flatten_now) {
        MassCancel request;
        std::uint64_t counter = 0;
        {
            std::lock_guard lock(mutex_);
            counter = ++cl_ord_counter_;
        }
        try {
            request.cl_ord_id = make_cl_ord_id("halt-", counter);
            request.scope = MassCancel::Scope::All;
            if (client_.mass_cancel(request)) {
                logger_.warn("mass cancel sent on reconnect: " + request.cl_ord_id);
            }
        } catch (const std::exception& e) {
            logger_.error(std::string("mass cancel could not be built: ") + e.what());
        }
    }
    logger_.info("reconciling before submitting anything");
    if (!client_.request_last_exec_id()) {
        raise(Halt::Reason::DataLoss, "could not ask the exchange for its newest ExecID");
    }
}

void Reconciler::raise(Halt::Reason reason, std::string detail) {
    bool flatten = false;
    {
        std::lock_guard lock(mutex_);
        // A shutdown the owner asked for is not a failure: its logout would
        // otherwise arrive here as a Disconnect.
        if (stopping_) return;
        if (halt_) return;  // the first condition is the one that matters
        halt_ = Halt{reason, std::move(detail), Clock::now()};
        // Submission stops immediately, whatever happens next.
        state_value_ = RecoveryState::Flattening;
        // A session displaced by a newer connection is the exception: its
        // orders now belong to that session, so cancelling from here would
        // cancel theirs.
        flatten = reason != Halt::Reason::Replaced;
        if (flatten) flatten_sent_ = client_.logged_on();
    }
    const auto halt = *this->halt();
    logger_.error(std::string("halting: ") + to_string(halt.reason) + " - " + halt.detail);

    // OE-Q6: flatten, then reconcile, then stop. A session displaced by a newer
    // connection is the exception: its orders now belong to that session, and a
    // mass cancel from here would cancel theirs.
    if (flatten && client_.logged_on()) {
        MassCancel request;
        std::uint64_t counter = 0;
        {
            std::lock_guard lock(mutex_);
            counter = ++cl_ord_counter_;
        }
        try {
            request.cl_ord_id = make_cl_ord_id("halt-", counter);
            request.scope = MassCancel::Scope::All;
            if (client_.mass_cancel(request)) {
                logger_.warn("mass cancel sent: " + request.cl_ord_id);
            } else {
                logger_.error("mass cancel could not be sent; orders may be resting");
            }
        } catch (const std::exception& e) {
            logger_.error(std::string("mass cancel could not be built: ") + e.what());
        }
    } else if (flatten) {
        logger_.error("not logged on, so nothing could be cancelled; orders may be resting");
    }
    save_bookmark();
    deliver();
}

void Reconciler::deliver() {
    Halt halt;
    {
        std::lock_guard lock(mutex_);
        if (delivered_ || !halt_) return;
        delivered_ = true;
        halt = *halt_;
    }
    if (on_halt_) on_halt_(halt);
}

void Reconciler::tick(Clock::time_point now) {
    bool timed_out = false;
    {
        std::lock_guard lock(mutex_);
        if (state_value_ == RecoveryState::Reconciling &&
            now - phase_started_ > options_.reconcile_timeout) {
            timed_out = true;
        }
    }
    if (timed_out) {
        raise(Halt::Reason::DataLoss, "reconciliation did not finish within the timeout");
    }
}

void Reconciler::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread([this] { run(); });
}

void Reconciler::stop() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    if (!running_.exchange(false)) return;
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void Reconciler::run() {
    while (running_.load()) {
        {
            std::unique_lock lock(wake_mutex_);
            wake_.wait_for(lock, std::chrono::milliseconds(100));
        }
        if (!running_.load()) return;
        tick(Clock::now());
    }
}

RecoveryState Reconciler::state() const {
    std::lock_guard lock(mutex_);
    return state_value_;
}

std::optional<Halt> Reconciler::halt() const {
    std::lock_guard lock(mutex_);
    return halt_;
}

bool Reconciler::submission_allowed() const {
    std::lock_guard lock(mutex_);
    return state_value_ == RecoveryState::Live && !halt_;
}

}  // namespace cpm::oms
