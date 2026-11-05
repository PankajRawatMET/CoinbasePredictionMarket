#pragma once

#include "cpm/common/logging.hpp"
#include "cpm/oms/sim/simulator.hpp"

#include <quickfix/Application.h>
#include <quickfix/Message.h>
#include <quickfix/SessionID.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace cpm::oms::sim::fix {

// The gateway's FIX side: verifies the Logon, turns D/F/q into engine calls and
// engine events into reports, and answers F1/F3 from the events it has already
// sent.
class SimApplication : public FIX::Application {
public:
    SimApplication(const SimOptions& options, Logger& logger);

    bool wait_logged_on(std::chrono::milliseconds timeout) const;
    int logons() const;
    std::vector<RestingOrder> book(const std::string& symbol, Side side) const;
    // Releases speed-bumped orders whose delay has passed. Called on a timer.
    void tick(Clock::time_point now);
    void settle(const std::string& symbol, SettlementReport::Outcome outcome, Price price,
                Qty long_quantity, Qty short_quantity);
    Qty trade(const std::string& symbol, Side side, Qty quantity, Price price);
    void rest(const std::string& symbol, Side side, Qty quantity, Price price);

    void onCreate(const FIX::SessionID&) override {}
    void onLogon(const FIX::SessionID&) override;
    void onLogout(const FIX::SessionID&) override;
    void toAdmin(FIX::Message& message, const FIX::SessionID&) override;
    void toApp(FIX::Message& message, const FIX::SessionID&) override;
    void fromAdmin(const FIX::Message& message, const FIX::SessionID&) override;
    void fromApp(const FIX::Message& message, const FIX::SessionID&) override;

private:
    void handle_new_order(const FIX::Message& message);
    void handle_cancel(const FIX::Message& message);
    void handle_mass_cancel(const FIX::Message& message);
    void handle_last_exec_id_request(const FIX::Message& message);
    void handle_event_resend_request(const FIX::Message& message);
    // Turns engine events into messages and queues them, in order.
    void publish(const std::vector<Event>& events);
    void send(FIX::Message message, bool replayable);
    std::string next_exec_id();

    const SimOptions& options_;
    Logger& logger_;

    mutable std::mutex mutex_;
    mutable std::condition_variable logged_on_cv_;
    Engine engine_;
    bool logged_on_ = false;
    int logons_ = 0;
    bool client_logout_ = false;
    std::optional<FIX::SessionID> session_id_;
    std::uint64_t next_exec_ = 480837776594436096ULL;
    std::uint64_t takes_ = 0;  // ClOrdIDs for the taker in trade()
    // Everything replayable, newest last, as (ExecID, message).
    std::vector<std::pair<std::uint64_t, FIX::Message>> history_;
};

}  // namespace cpm::oms::sim::fix
