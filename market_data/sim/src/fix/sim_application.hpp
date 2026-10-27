#pragma once

#include "cpm/common/logging.hpp"
#include "cpm/md/sim/market.hpp"
#include "cpm/md/sim/request.hpp"
#include "cpm/md/sim/simulator.hpp"

#include <quickfix/Application.h>
#include <quickfix/Message.h>
#include <quickfix/SessionID.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cpm::md::sim::fix {

// The gateway side of md_sim. QuickFIX threads call the Application callbacks;
// the simulator's event thread calls tick(). Every outbound market data message
// goes through one queue that only tick() drains, so a subscription's W always
// precedes its X, and no QuickFIX session lock is ever taken while mutex_ is held.
class SimApplication : public FIX::Application {
public:
    SimApplication(const SimOptions& options, Logger& logger, Clock::time_point now);

    // Advances the market (events, rolls, settlements), applies faults and
    // sends everything queued.
    void tick(Clock::time_point now);

    bool wait_logged_on(std::chrono::milliseconds timeout) const;
    int logons() const;

    // FIX::Application
    void onCreate(const FIX::SessionID&) override {}
    void onLogon(const FIX::SessionID& session_id) override;
    void onLogout(const FIX::SessionID& session_id) override;
    void toAdmin(FIX::Message& message, const FIX::SessionID&) override;
    void toApp(FIX::Message& message, const FIX::SessionID& session_id) override;
    void fromAdmin(const FIX::Message& message, const FIX::SessionID&) override;
    void fromApp(const FIX::Message& message, const FIX::SessionID&) override;

private:
    struct ActiveSub {
        Subscription sub;
        bool stale = false;  // silenced by the stale fault
    };

    void handle_request(const FIX::Message& message);
    void publish_step(const Step& step);
    void publish_status(const StatusEvent& event);
    void roll(Clock::time_point now);
    void apply_faults(Clock::time_point now);
    void flush();
    InstrumentSpec next_roll_spec(Clock::time_point window_end);

    const SimOptions options_;
    Logger& logger_;

    mutable std::mutex mutex_;
    mutable std::condition_variable logged_on_cv_;
    Market market_;
    std::map<std::string, ActiveSub> subs_;  // by MDReqID
    std::deque<FIX::Message> queue_;
    std::optional<FIX::SessionID> session_id_;
    bool logged_on_ = false;
    int logons_ = 0;
    bool client_logout_ = false;  // the client started the logout
    Clock::time_point logon_time_{};

    // Schedule (event thread only).
    Clock::time_point next_event_;
    Clock::duration event_interval_;
    std::string roll_symbol_;
    Clock::time_point next_roll_{};
    std::vector<std::pair<Clock::time_point, std::string>> pending_settles_;
    std::uint64_t next_roll_id_ = 800000000000000001ULL;

    // Faults.
    std::atomic<int> app_messages_sent_{0};  // this session
    std::atomic<bool> disconnect_pending_{false};
    bool drop_done_ = false;
    bool disconnect_done_ = false;
    bool stale_done_ = false;
};

}  // namespace cpm::md::sim::fix
