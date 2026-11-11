#include "cpm/md/feed_monitor.hpp"

#include <algorithm>

namespace cpm::md {
namespace {

// Closed, Determined and Settled instruments stop sending book updates and
// never resume (O6/O10). Unknown counts as live: without a lifecycle
// subscription nothing says otherwise, and halting is the safe direction.
bool finished(LifecycleStatus status) {
    switch (status) {
        case LifecycleStatus::Closed:
        case LifecycleStatus::Determined:
        case LifecycleStatus::Settled:
        case LifecycleStatus::Delisted: return true;
        default: return false;
    }
}

}  // namespace

const char* to_string(FeedHalt::Reason reason) {
    switch (reason) {
        case FeedHalt::Reason::Gap: return "sequence gap";
        case FeedHalt::Reason::StaleFeed: return "stale feed";
        case FeedHalt::Reason::Reject: return "reject";
        case FeedHalt::Reason::Disconnect: return "disconnect";
    }
    return "unknown";
}

FeedMonitor::FeedMonitor(Options options, HaltHandler on_halt, ClockFn clock)
    : options_(options),
      on_halt_(std::move(on_halt)),
      clock_(clock ? std::move(clock) : ClockFn([] { return Clock::now(); })) {}

FeedMonitor::~FeedMonitor() { stop(); }

MdCallbacks FeedMonitor::callbacks() {
    MdCallbacks callbacks;
    const auto now = [this] { return clock_(); };
    callbacks.book_snapshot = [this, now](const BookSnapshot& e) { stamp(e.md_req_id, now()); };
    callbacks.book_update = [this, now](const BookUpdate& e) { stamp(e.md_req_id, now()); };
    callbacks.trade = [this, now](const Trade& e) { stamp(e.md_req_id, now()); };
    callbacks.volume = [this, now](const Volume& e) { stamp(e.md_req_id, now()); };
    callbacks.open_interest = [this, now](const OpenInterest& e) { stamp(e.md_req_id, now()); };
    callbacks.lifecycle = [this, now](const InstrumentLifecycle& e) {
        {
            std::lock_guard lock(mutex_);
            statuses_[e.symbol] = e.status;
        }
        stamp(e.md_req_id, now());
    };
    // Q27: any reject halts, whatever its code.
    callbacks.reject = [this, now](const MdReject& e) {
        const bool market_data = e.kind == MdReject::Kind::MarketDataRequest;
        raise(FeedHalt::Reason::Reject, e.md_req_id, {},
              (market_data ? "MarketDataRequestReject 281=" : "BusinessMessageReject 380=") + e.reason_code +
                  (e.text.empty() ? "" : ": " + e.text),
              now());
    };
    return callbacks;
}

MdClient::SessionCallback FeedMonitor::session_callback() {
    return [this](const SessionEvent& event) {
        const auto now = clock_();
        switch (event.kind) {
            case SessionEvent::Kind::LoggedOn: {
                std::lock_guard lock(mutex_);
                logged_on_ = true;
                // Nothing has been missed yet; restart every subscription's clock.
                for (auto& [id, subscription] : subscriptions_) subscription.last_message = now;
                break;
            }
            case SessionEvent::Kind::LoggedOut:
                {
                    std::lock_guard lock(mutex_);
                    logged_on_ = false;
                }
                // Q28: a disconnect means data was missed, and QuickFIX would
                // log back on with no subscriptions. A clean shutdown stops
                // the monitor first, so this only fires when we did not ask.
                raise(FeedHalt::Reason::Disconnect, {}, {}, event.reason, now);
                break;
            case SessionEvent::Kind::DataLoss:
                // Q24: a gap is not repaired by resending; the strategy stops.
                raise(FeedHalt::Reason::Gap, {}, {}, event.reason, now);
                break;
        }
    };
}

void FeedMonitor::track(const std::string& md_req_id, const ChannelRequest& request) {
    const auto now = clock_();
    std::lock_guard lock(mutex_);
    subscriptions_[md_req_id] = Subscription{request.channel, request.symbols, request.depth, now};
}

void FeedMonitor::forget(const std::string& md_req_id) {
    std::lock_guard lock(mutex_);
    subscriptions_.erase(md_req_id);
}

void FeedMonitor::start() {
    if (running_.exchange(true)) return;
    phase_.store(Phase::Running);
    thread_ = std::thread([this] { run(); });
}

void FeedMonitor::allow_quiet_books(std::vector<std::string> symbols) {
    std::lock_guard lock(mutex_);
    quiet_books_ = std::move(symbols);
}

void FeedMonitor::stop() {
    if (!running_.exchange(false)) return;
    phase_.store(Phase::Stopped);
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void FeedMonitor::run() {
    while (running_.load()) {
        {
            std::unique_lock lock(wake_mutex_);
            wake_.wait_for(lock, options_.poll);
        }
        if (!running_.load()) return;
        tick(clock_());
        if (delivery_pending()) {
            deliver();
            return;  // nothing after the first halt is worth watching
        }
    }
}

void FeedMonitor::tick(Clock::time_point now) {
    std::string stale_id;
    std::string stale_symbol;
    std::string detail;
    {
        std::lock_guard lock(mutex_);
        if (halt_ || !logged_on_) return;
        for (const auto& [id, subscription] : subscriptions_) {
            // Only a book is expected to keep speaking; see the class comment.
            if (subscription.channel != Channel::Book) continue;
            const auto silence = now - subscription.last_message;
            if (silence < options_.stale_after) continue;
            if (all_instruments_finished(subscription)) continue;
            if (!subscription.symbols.empty() &&
                std::all_of(subscription.symbols.begin(), subscription.symbols.end(), [this](const auto& symbol) {
                    return std::find(quiet_books_.begin(), quiet_books_.end(), symbol) != quiet_books_.end();
                })) continue;
            stale_id = id;
            stale_symbol = subscription.symbols.empty() ? std::string() : subscription.symbols.front();
            detail = "no book update for " +
                     std::to_string(std::chrono::duration_cast<std::chrono::seconds>(silence).count()) + "s";
            break;
        }
    }
    if (!stale_id.empty()) raise(FeedHalt::Reason::StaleFeed, stale_id, stale_symbol, detail, now);
}

bool FeedMonitor::all_instruments_finished(const Subscription& subscription) const {
    if (subscription.symbols.empty()) return false;  // all instruments: never exempt
    return std::all_of(subscription.symbols.begin(), subscription.symbols.end(), [this](const std::string& symbol) {
        const auto it = statuses_.find(symbol);
        return it != statuses_.end() && finished(it->second);
    });
}

void FeedMonitor::raise(FeedHalt::Reason reason, std::string md_req_id, std::string symbol, std::string detail,
                        Clock::time_point now) {
    {
        std::lock_guard lock(mutex_);
        if (halt_) return;  // the first condition is the one that matters
        halt_ = FeedHalt{reason, std::move(md_req_id), std::move(symbol), std::move(detail), now};
    }
    // Rejects and session events arrive on a QuickFIX thread, where stopping
    // the client would deadlock, so the driver thread delivers them. With no
    // driver thread (tests driving tick() by hand) deliver inline. Once
    // stopped, record only: see the header.
    switch (phase_.load()) {
        case Phase::Fresh: deliver(); break;
        case Phase::Running: wake_.notify_all(); break;
        case Phase::Stopped: break;
    }
}

void FeedMonitor::deliver() {
    FeedHalt halt;
    {
        std::lock_guard lock(mutex_);
        if (delivered_ || !halt_) return;
        delivered_ = true;
        halt = *halt_;
    }
    if (on_halt_) on_halt_(halt);
}

bool FeedMonitor::delivery_pending() const {
    std::lock_guard lock(mutex_);
    return halt_.has_value() && !delivered_;
}

void FeedMonitor::stamp(const std::string& md_req_id, Clock::time_point time) {
    std::lock_guard lock(mutex_);
    const auto it = subscriptions_.find(md_req_id);
    if (it != subscriptions_.end()) it->second.last_message = time;
}

bool FeedMonitor::halted() const {
    std::lock_guard lock(mutex_);
    return halt_.has_value();
}

std::optional<FeedHalt> FeedMonitor::halt() const {
    std::lock_guard lock(mutex_);
    return halt_;
}

}  // namespace cpm::md
