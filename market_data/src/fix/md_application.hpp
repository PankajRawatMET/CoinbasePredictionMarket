#pragma once

#include "cpm/common/config.hpp"
#include "cpm/common/logging.hpp"
#include "cpm/md/md_client.hpp"
#include "fix/md_decoder.hpp"

#include <quickfix/Application.h>
#include <quickfix/SessionID.h>

#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace cpm::md::fix {

// QuickFIX application for the MD session. Signs the Logon, tracks session
// state, and turns inbound market data into the plain structs of
// cpm/md/events.hpp, which it hands to the registered callbacks.
class MdApplication : public FIX::Application {
public:
    MdApplication(Credentials credentials, Logger& logger);

    void set_callback(MdClient::SessionCallback callback);
    void set_market_data_callbacks(MdCallbacks callbacks);

    bool wait_logged_on(std::chrono::milliseconds timeout) const;
    bool logged_on() const;
    std::string last_logout_reason() const;

    // Instrument discovery (see MdClient::list_instruments).
    void begin_discovery(const std::string& req_id);
    // Sends MarketDataRequest(V): subscribe (263=1) or unsubscribe (263=2).
    bool send_market_data_request(const std::string& req_id, const ChannelRequest& request, bool subscribe);
    InstrumentList wait_discovery(std::chrono::milliseconds quiet, std::chrono::milliseconds timeout);
    void end_discovery();

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
    // Watches inbound MsgSeqNum for a gap, and reports SessionEvent::DataLoss.
    // Nothing is resent for market data (Q12), so a gap is lost data.
    void check_sequence(const FIX::Message& message);
    // Hands one decoded message to the callbacks, logging what it carried.
    void dispatch(const DecodedBatch& batch);
    // Takes instrument names out of a message belonging to the discovery
    // subscription. True when the message was discovery's and is not published.
    bool collect_discovery(const DecodedBatch& batch);
    // Logs a reject; if discovery is running, also ends it with this reason.
    void discovery_reject(const std::string& text);

    const Credentials credentials_;
    Logger& logger_;
    MdClient::SessionCallback callback_;
    MdCallbacks md_callbacks_;  // set before start(), read on the QuickFIX thread

    mutable std::mutex mutex_;
    mutable std::condition_variable logged_on_cv_;
    bool logged_on_ = false;
    bool logout_sent_ = false;         // we sent Logout, so the server's Logout is its answer
    std::string pending_logout_text_;  // Text(58) of a received Logout, until onLogout
    std::string last_logout_reason_;
    std::optional<FIX::SessionID> session_id_;
    int expected_in_seq_ = 1;  // next inbound MsgSeqNum; adopted from each Logon

    // Discovery state; guarded by mutex_, waited on with discovery_cv_.
    std::condition_variable discovery_cv_;
    std::string discovery_req_id_;                  // empty when no discovery is running
    std::map<std::string, std::string> discovered_;  // symbol -> security id
    std::chrono::steady_clock::time_point last_new_instrument_{};
    std::string discovery_reject_;
};

}  // namespace cpm::md::fix
