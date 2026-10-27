#pragma once

#include "cpm/common/config.hpp"
#include "cpm/common/logging.hpp"
#include "cpm/oms/oe_client.hpp"

#include "fix/oe_decoder.hpp"

#include <quickfix/Application.h>
#include <quickfix/SessionID.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>

namespace cpm::oms::fix {

// QuickFIX application for the order entry session. Signs the Logon, adds the
// session options the gateway only reads at Logon, tracks session state and
// reports session events. Order handling arrives in Stage 3.
class OeApplication : public FIX::Application {
public:
    OeApplication(OeConfig config, Logger& logger);

    void set_callback(OeClient::SessionCallback callback);
    void set_event_callbacks(OeCallbacks callbacks);

    // The session to send on, once logged on.
    std::optional<FIX::SessionID> session_id() const;

    bool wait_logged_on(std::chrono::milliseconds timeout) const;
    bool logged_on() const;
    std::string last_logout_reason() const;
    bool replaced() const;

    // FIX::Application
    void onCreate(const FIX::SessionID&) override {}
    void onLogon(const FIX::SessionID&) override;
    void onLogout(const FIX::SessionID&) override;
    void toAdmin(FIX::Message& message, const FIX::SessionID&) override;
    void toApp(FIX::Message&, const FIX::SessionID&) override {}
    void fromAdmin(const FIX::Message& message, const FIX::SessionID&) override;
    void fromApp(const FIX::Message& message, const FIX::SessionID&) override;

private:
    void notify(const SessionEvent& event);
    // Watches inbound MsgSeqNum and the gap fill that answers a ResendRequest:
    // the sequence is repaired, but the events in the gap are lost.
    void check_sequence(const FIX::Message& message);
    // Hands one decoded message to the callbacks, logging what it carried.
    void dispatch(const DecodedBatch& batch);

    const OeConfig config_;
    Logger& logger_;
    OeClient::SessionCallback callback_;
    OeCallbacks events_;  // set before start(), read on the QuickFIX thread

    mutable std::mutex mutex_;
    mutable std::condition_variable logged_on_cv_;
    bool logged_on_ = false;
    bool logout_sent_ = false;         // we sent Logout, so the server's Logout is its answer
    bool replaced_ = false;            // displaced by a newer connection for the same API key
    std::string pending_logout_text_;  // Text(58) of a received Logout, until onLogout
    std::string last_logout_reason_;
    std::optional<FIX::SessionID> session_id_;
    int expected_in_seq_ = 1;  // adopted from each Logon
};

}  // namespace cpm::oms::fix
