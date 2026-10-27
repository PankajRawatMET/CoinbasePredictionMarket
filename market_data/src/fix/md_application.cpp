#include "fix/md_application.hpp"

#include "cpm/fix_common/logon_auth.hpp"

#include <quickfix/FixFieldNumbers.h>
#include <quickfix/FixValues.h>
#include <quickfix/Message.h>
#include <quickfix/Session.h>

#include <algorithm>
#include <vector>

namespace cpm::md::fix {
namespace {

std::string msg_type(const FIX::Message& message) {
    return message.getHeader().getField(FIX::FIELD::MsgType);
}

}  // namespace

MdApplication::MdApplication(Credentials credentials, Logger& logger)
    : credentials_(std::move(credentials)), logger_(logger) {}

void MdApplication::set_callback(MdClient::SessionCallback callback) { callback_ = std::move(callback); }

void MdApplication::set_market_data_callbacks(MdCallbacks callbacks) { md_callbacks_ = std::move(callbacks); }

void MdApplication::onLogon(const FIX::SessionID& session_id) {
    {
        std::lock_guard lock(mutex_);
        logged_on_ = true;
        session_id_ = session_id;
        logout_sent_ = false;
    }
    logged_on_cv_.notify_all();
    logger_.info("session logged on");
    notify({SessionEvent::Kind::LoggedOn, {}});
}

// QuickFIX calls this on Logout and on any disconnect of a logged-on session.
void MdApplication::onLogout(const FIX::SessionID&) {
    std::string reason;
    {
        std::lock_guard lock(mutex_);
        logged_on_ = false;
        reason = pending_logout_text_.empty() ? "session disconnected" : pending_logout_text_;
        pending_logout_text_.clear();
        logout_sent_ = false;
        last_logout_reason_ = reason;
    }
    logged_on_cv_.notify_all();
    discovery_cv_.notify_all();
    logger_.info("session logged out: " + reason);
    notify({SessionEvent::Kind::LoggedOut, reason});
}

void MdApplication::toAdmin(FIX::Message& message, const FIX::SessionID&) {
    const auto type = msg_type(message);
    if (type == FIX::MsgType_Logon) {
        cpm::fix::apply_logon_auth(message, credentials_);
    } else if (type == FIX::MsgType_Logout) {
        std::lock_guard lock(mutex_);
        logout_sent_ = true;
    }
}

void MdApplication::fromAdmin(const FIX::Message& message, const FIX::SessionID&) {
    const auto type = msg_type(message);
    // A SequenceReset with GapFillFlag=Y is the gateway telling us the
    // messages we asked to be resent are gone. QuickFIX handles the sequence;
    // the market data in them is lost, which is what matters here.
    if (type == FIX::MsgType_SequenceReset) {
        const bool gap_fill = message.isSetField(FIX::FIELD::GapFillFlag) &&
                              message.getField(FIX::FIELD::GapFillFlag) == "Y";
        const auto new_seq = message.isSetField(FIX::FIELD::NewSeqNo) ? message.getField(FIX::FIELD::NewSeqNo)
                                                                      : std::string("?");
        if (gap_fill) {
            const auto reason = "sequence gap: gap fill to MsgSeqNum " + new_seq +
                                ", market data was lost and is not resent";
            logger_.warn(reason);
            notify({SessionEvent::Kind::DataLoss, reason});
        }
        return;
    }
    check_sequence(message);
    if (type == FIX::MsgType_Reject) {
        discovery_reject("session Reject(3): " +
                         (message.isSetField(FIX::FIELD::Text) ? message.getField(FIX::FIELD::Text)
                                                               : std::string("no text")));
        return;
    }
    if (type != FIX::MsgType_Logout) return;
    std::string text = message.isSetField(FIX::FIELD::Text) ? message.getField(FIX::FIELD::Text)
                                                            : "logout from server";
    bool answer = false;
    {
        std::lock_guard lock(mutex_);
        pending_logout_text_ = text;
        // A Logout rejecting our Logon never reaches onLogout (we were never logged on).
        if (!logged_on_) last_logout_reason_ = text;
        answer = logout_sent_;
    }
    // Answering our own Logout is routine; a Logout the server starts is not.
    if (answer) {
        logger_.info("server confirmed Logout: " + text);
    } else {
        logger_.warn("server sent Logout: " + text);
    }
}

void MdApplication::fromApp(const FIX::Message& message, const FIX::SessionID&) {
    const auto type = msg_type(message);
    check_sequence(message);
    const auto batch = decode_message(message, credentials_.api_key);
    if (batch.empty()) {
        logger_.debug("application message 35=" + type + " carried nothing to decode");
        return;
    }
    if (collect_discovery(batch)) return;
    dispatch(batch);
}

// Instrument discovery is an internal, transient subscription, so its messages
// are consumed here rather than published. The spec says an all-instrument
// volume/OI subscribe is answered with a W per instrument (§4.2), but the
// sandbox answers with X updates and no W at all [OBS 2026-10-26, O8], so
// symbols are taken from either.
bool MdApplication::collect_discovery(const DecodedBatch& batch) {
    if (!batch.rejects.empty() || !batch.errors.empty()) return false;  // dispatch reports those
    bool mine = false;
    bool added = false;
    {
        std::lock_guard lock(mutex_);
        if (discovery_req_id_.empty()) return false;
        const auto record = [&](const std::string& req_id, const std::string& symbol,
                                const std::string& security_id) {
            if (req_id != discovery_req_id_) return;
            mine = true;
            if (symbol.empty()) return;
            const auto [it, inserted] = discovered_.emplace(symbol, security_id);
            added = added || inserted;
        };
        for (const auto& event : batch.book_snapshots) record(event.md_req_id, event.symbol, event.security_id);
        for (const auto& event : batch.volumes) record(event.md_req_id, event.symbol, event.security_id);
        for (const auto& event : batch.open_interest) record(event.md_req_id, event.symbol, event.security_id);
        if (!mine) return false;
        // The wait settles on new instruments going quiet, not on traffic
        // going quiet: volume/OI updates arrive about once a second forever.
        if (added) last_new_instrument_ = std::chrono::steady_clock::now();
    }
    if (added) discovery_cv_.notify_all();
    return true;
}

void MdApplication::dispatch(const DecodedBatch& batch) {
    for (const auto& error : batch.errors) {
        logger_.warn("decode error (35=" + error.msg_type + "): " + error.reason + " in " + error.raw);
        if (md_callbacks_.decode_error) md_callbacks_.decode_error(error);
    }
    for (const auto& reject : batch.rejects) {
        const bool market_data = reject.kind == MdReject::Kind::MarketDataRequest;
        const auto reason = market_data ? "MarketDataRequestReject 262=" + reject.md_req_id + " 281=" +
                                              reject.reason_code
                                        : "BusinessMessageReject 380=" + reject.reason_code;
        const auto text = reason + (reject.text.empty() ? "" : ": " + reject.text);
        // A reject on the discovery request ends the wait; j has no 262, so it
        // ends any discovery in progress.
        bool ours = !market_data;
        if (market_data) {
            std::lock_guard lock(mutex_);
            ours = !discovery_req_id_.empty() && reject.md_req_id == discovery_req_id_;
        }
        if (ours) {
            discovery_reject(text);
        } else {
            logger_.warn(text);
        }
        if (md_callbacks_.reject) md_callbacks_.reject(reject);
    }
    if (md_callbacks_.book_snapshot) {
        for (const auto& event : batch.book_snapshots) md_callbacks_.book_snapshot(event);
    }
    if (md_callbacks_.book_update) {
        for (const auto& event : batch.book_updates) md_callbacks_.book_update(event);
    }
    if (md_callbacks_.trade) {
        for (const auto& event : batch.trades) md_callbacks_.trade(event);
    }
    if (md_callbacks_.volume) {
        for (const auto& event : batch.volumes) md_callbacks_.volume(event);
    }
    if (md_callbacks_.open_interest) {
        for (const auto& event : batch.open_interest) md_callbacks_.open_interest(event);
    }
    if (md_callbacks_.lifecycle) {
        for (const auto& event : batch.lifecycle) md_callbacks_.lifecycle(event);
    }
}

// Backstop for the gap fill above: QuickFIX queues out-of-order messages while
// it resends, so a jump reaching us means data went missing.
void MdApplication::check_sequence(const FIX::Message& message) {
    const auto& header = message.getHeader();
    if (!header.isSetField(FIX::FIELD::MsgSeqNum)) return;
    // A resent message repeats a sequence number we have already seen.
    if (header.isSetField(FIX::FIELD::PossDupFlag) && header.getField(FIX::FIELD::PossDupFlag) == "Y") return;
    int seq = 0;
    try {
        seq = std::stoi(header.getField(FIX::FIELD::MsgSeqNum));
    } catch (const std::exception&) {
        return;
    }
    // A Logon starts a sequence stream: with ResetSeqNumFlag=Y it restarts at
    // 1, and it reaches us before onLogon, so this is where the expectation is
    // adopted rather than compared.
    if (msg_type(message) == FIX::MsgType_Logon) {
        std::lock_guard lock(mutex_);
        expected_in_seq_ = seq + 1;
        return;
    }
    std::string reason;
    {
        std::lock_guard lock(mutex_);
        if (seq > expected_in_seq_) {
            reason = "sequence gap: expected MsgSeqNum " + std::to_string(expected_in_seq_) + ", received " +
                     std::to_string(seq);
        }
        if (seq >= expected_in_seq_) expected_in_seq_ = seq + 1;
    }
    if (reason.empty()) return;
    logger_.warn(reason);
    notify({SessionEvent::Kind::DataLoss, reason});
}

void MdApplication::begin_discovery(const std::string& req_id) {
    std::lock_guard lock(mutex_);
    discovery_req_id_ = req_id;
    discovered_.clear();
    discovery_reject_.clear();
}

bool MdApplication::send_market_data_request(const std::string& req_id, const ChannelRequest& request,
                                             bool subscribe) {
    std::optional<FIX::SessionID> session_id;
    {
        std::lock_guard lock(mutex_);
        session_id = session_id_;
    }
    if (!session_id) return false;

    std::vector<const char*> entry_types;
    switch (request.channel) {
        case Channel::Book: entry_types = {"0", "1"}; break;
        case Channel::Trades: entry_types = {"2"}; break;
        case Channel::VolumeOi: entry_types = {"B", "C"}; break;
        case Channel::Lifecycle: entry_types = {"L"}; break;
    }

    // Body in the spec's field order (§4 examples). MarketDepth(264) and
    // AggregatedBook(266) only on book requests, as in the spec's book examples.
    FIX::Message message(FIX::message_order(FIX::message_order::header),
                         FIX::message_order(FIX::message_order::trailer),
                         FIX::message_order(FIX::FIELD::MDReqID, FIX::FIELD::SubscriptionRequestType,
                                            FIX::FIELD::MarketDepth, FIX::FIELD::AggregatedBook,
                                            FIX::FIELD::NoMDEntryTypes, FIX::FIELD::NoRelatedSym, 0));
    message.getHeader().setField(FIX::FIELD::MsgType, FIX::MsgType_MarketDataRequest);
    message.setField(FIX::FIELD::MDReqID, req_id);
    message.setField(FIX::FIELD::SubscriptionRequestType, subscribe ? "1" : "2");
    if (request.channel == Channel::Book) {
        message.setField(FIX::FIELD::MarketDepth, std::to_string(request.depth));
        message.setField(FIX::FIELD::AggregatedBook, "Y");
    }
    for (const char* entry_type : entry_types) {
        FIX::Group group(FIX::FIELD::NoMDEntryTypes, FIX::FIELD::MDEntryType,
                         FIX::message_order(FIX::FIELD::MDEntryType, 0));
        group.setField(FIX::FIELD::MDEntryType, entry_type);
        message.addGroup(group);
    }
    if (request.symbols.empty()) {
        message.setField(FIX::FIELD::NoRelatedSym, "0");  // all instruments
    }
    for (const auto& symbol : request.symbols) {
        FIX::Group group(FIX::FIELD::NoRelatedSym, FIX::FIELD::Symbol, FIX::message_order(FIX::FIELD::Symbol, 0));
        group.setField(FIX::FIELD::Symbol, symbol);
        message.addGroup(group);
    }
    try {
        return FIX::Session::sendToTarget(message, *session_id);
    } catch (const FIX::SessionNotFound&) {
        return false;
    }
}

InstrumentList MdApplication::wait_discovery(std::chrono::milliseconds quiet, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::unique_lock lock(mutex_);
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        const bool settled = !discovered_.empty() && now - last_new_instrument_ >= quiet;
        if (!discovery_reject_.empty() || settled || now >= deadline || !logged_on_) break;
        const auto wake = discovered_.empty() ? deadline : std::min(deadline, last_new_instrument_ + quiet);
        discovery_cv_.wait_until(lock, wake);
    }
    InstrumentList result;
    result.reject = discovery_reject_;
    for (const auto& [symbol, security_id] : discovered_) result.instruments.push_back({symbol, security_id});
    return result;
}

void MdApplication::end_discovery() {
    std::lock_guard lock(mutex_);
    discovery_req_id_.clear();
}

void MdApplication::discovery_reject(const std::string& text) {
    logger_.warn(text);
    {
        std::lock_guard lock(mutex_);
        if (discovery_req_id_.empty() || !discovery_reject_.empty()) return;
        discovery_reject_ = text;
    }
    discovery_cv_.notify_all();
}

bool MdApplication::wait_logged_on(std::chrono::milliseconds timeout) const {
    std::unique_lock lock(mutex_);
    return logged_on_cv_.wait_for(lock, timeout, [this] { return logged_on_; });
}

bool MdApplication::logged_on() const {
    std::lock_guard lock(mutex_);
    return logged_on_;
}

std::string MdApplication::last_logout_reason() const {
    std::lock_guard lock(mutex_);
    return last_logout_reason_;
}

void MdApplication::notify(const SessionEvent& event) {
    if (callback_) callback_(event);
}

}  // namespace cpm::md::fix
