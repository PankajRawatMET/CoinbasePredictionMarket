#include "fix/oe_application.hpp"

#include "cpm/fix_common/logon_auth.hpp"

#include <quickfix/FixFieldNumbers.h>
#include <quickfix/FixValues.h>
#include <quickfix/Message.h>

namespace cpm::oms::fix {
namespace {

// CPM session options, only read at Logon.
constexpr int kCancelOrdersOnDisconnect = 8013;
constexpr int kReceiveSettlementReports = 20127;

// The gateway's wording when a second Logon for the same API key displaces this
// session (spec "Connection & Session").
constexpr std::string_view kReplacedText = "replaced by newer connection";

std::string msg_type(const FIX::Message& message) {
    return message.getHeader().getField(FIX::FIELD::MsgType);
}

std::string field_or_empty(const FIX::Message& message, int tag) {
    return message.isSetField(tag) ? message.getField(tag) : std::string();
}

}  // namespace

OeApplication::OeApplication(OeConfig config, Logger& logger)
    : config_(std::move(config)), logger_(logger) {}

void OeApplication::set_callback(OeClient::SessionCallback callback) { callback_ = std::move(callback); }

void OeApplication::set_event_callbacks(OeCallbacks callbacks) { events_ = std::move(callbacks); }

std::optional<FIX::SessionID> OeApplication::session_id() const {
    std::lock_guard lock(mutex_);
    return session_id_;
}

void OeApplication::onLogon(const FIX::SessionID& session_id) {
    {
        std::lock_guard lock(mutex_);
        logged_on_ = true;
        session_id_ = session_id;
        logout_sent_ = false;
    }
    logged_on_cv_.notify_all();
    logger_.info("order entry session logged on");
    notify({SessionEvent::Kind::LoggedOn, {}});
}

void OeApplication::onLogout(const FIX::SessionID&) {
    std::string reason;
    bool replaced = false;
    {
        std::lock_guard lock(mutex_);
        logged_on_ = false;
        reason = pending_logout_text_.empty() ? "session disconnected" : pending_logout_text_;
        pending_logout_text_.clear();
        logout_sent_ = false;
        last_logout_reason_ = reason;
        replaced = replaced_;
    }
    logged_on_cv_.notify_all();
    logger_.info("order entry session logged out: " + reason);
    notify({replaced ? SessionEvent::Kind::Replaced : SessionEvent::Kind::LoggedOut, reason});
}

void OeApplication::toAdmin(FIX::Message& message, const FIX::SessionID&) {
    const auto type = msg_type(message);
    if (type == FIX::MsgType_Logon) {
        // Signature and Password, shared with market data: the recipe is the
        // same and takes TargetCompID from the header, which here is CPMOE.
        cpm::fix::apply_logon_auth(message, config_.credentials);
        // Session options. Sent explicitly every time rather than relying on
        // the gateway's defaults, because they cannot be changed afterwards.
        message.setField(kCancelOrdersOnDisconnect,
                         std::string(1, static_cast<char>(config_.cancel_on_disconnect)));
        message.setField(kReceiveSettlementReports, config_.settlement_reports ? "Y" : "N");
        logger_.info(std::string("logon options: CancelOrdersOnDisconnect(8013)=") +
                     static_cast<char>(config_.cancel_on_disconnect) +
                     ", ReceiveSettlementReports(20127)=" + (config_.settlement_reports ? "Y" : "N") +
                     ", ManualOrderIndicator(1028) on orders=" + (config_.manual_orders ? "Y" : "N"));
    } else if (type == FIX::MsgType_Logout) {
        std::lock_guard lock(mutex_);
        logout_sent_ = true;
    }
}

void OeApplication::fromAdmin(const FIX::Message& message, const FIX::SessionID&) {
    const auto type = msg_type(message);
    // A SequenceReset with GapFillFlag=Y repairs the sequence but not the
    // business events: those come back with EventResendRequest(F3), not here.
    if (type == FIX::MsgType_SequenceReset) {
        if (message.isSetField(FIX::FIELD::GapFillFlag) &&
            message.getField(FIX::FIELD::GapFillFlag) == "Y") {
            const auto new_seq = message.isSetField(FIX::FIELD::NewSeqNo)
                                     ? message.getField(FIX::FIELD::NewSeqNo)
                                     : std::string("?");
            const auto reason = "gap fill to MsgSeqNum " + new_seq +
                                ": the events in the gap need an EventResendRequest";
            logger_.warn(reason);
            notify({SessionEvent::Kind::DataLoss, reason});
        }
        return;
    }
    check_sequence(message);
    if (type == FIX::MsgType_Reject) {
        logger_.warn("session Reject(3): " +
                     (message.isSetField(FIX::FIELD::Text) ? message.getField(FIX::FIELD::Text)
                                                           : std::string("no text")));
        return;
    }
    if (type != FIX::MsgType_Logout) return;

    std::string text = field_or_empty(message, FIX::FIELD::Text);
    if (text.empty()) text = "logout from server";
    const bool replaced = text.find(kReplacedText) != std::string::npos;
    bool answer = false;
    {
        std::lock_guard lock(mutex_);
        pending_logout_text_ = text;
        if (replaced) replaced_ = true;
        // A Logout rejecting our Logon never reaches onLogout: we were never on.
        if (!logged_on_) last_logout_reason_ = text;
        answer = logout_sent_;
    }
    if (replaced) {
        // The API key is still live, so cancel-on-disconnect does not fire and
        // this session's resting orders remain under the newer connection.
        logger_.error("displaced by a newer connection for this API key: " + text +
                      " - resting orders are NOT canceled, they now belong to that session");
    } else if (answer) {
        logger_.info("server confirmed Logout: " + text);
    } else {
        logger_.warn("server sent Logout: " + text);
    }
}

void OeApplication::fromApp(const FIX::Message& message, const FIX::SessionID&) {
    check_sequence(message);
    const auto batch = decode_message(message, config_.credentials.api_key);
    if (batch.empty()) {
        logger_.debug("application message 35=" + msg_type(message) + " carried nothing to decode");
        return;
    }
    dispatch(batch);
}

void OeApplication::dispatch(const DecodedBatch& batch) {
    for (const auto& error : batch.errors) {
        logger_.warn("decode error (35=" + error.msg_type + "): " + error.reason + " in " + error.raw);
        if (events_.decode_error) events_.decode_error(error);
    }
    // A BusinessMessageReject is the synchronous rejection path: no
    // ExecutionReport follows, so it is terminal for the ClOrdID it names.
    for (const auto& reject : batch.business_rejects) {
        logger_.warn("BusinessMessageReject 380=" + std::to_string(reject.reason) + " for 35=" +
                     reject.ref_msg_type +
                     (reject.business_reject_ref_id.empty() ? "" : " ref=" + reject.business_reject_ref_id) +
                     (reject.text.empty() ? "" : ": " + reject.text));
        if (events_.business_reject) events_.business_reject(reject);
    }
    if (events_.report) {
        for (const auto& event : batch.reports) events_.report(event);
    }
    if (events_.cancel_reject) {
        for (const auto& event : batch.cancel_rejects) events_.cancel_reject(event);
    }
    if (events_.mass_cancel_report) {
        for (const auto& event : batch.mass_cancel_reports) events_.mass_cancel_report(event);
    }
    if (events_.settlement) {
        for (const auto& event : batch.settlements) events_.settlement(event);
    }
    if (events_.last_exec_id) {
        for (const auto& event : batch.last_exec_ids) events_.last_exec_id(event);
    }
    if (events_.resend_complete) {
        for (const auto& event : batch.resend_completes) events_.resend_complete(event);
    }
    if (events_.resend_reject) {
        for (const auto& event : batch.resend_rejects) events_.resend_reject(event);
    }
}

// A Logon starts a sequence stream and reaches us before onLogon, so the
// expectation is adopted from it rather than compared against it.
void OeApplication::check_sequence(const FIX::Message& message) {
    const auto& header = message.getHeader();
    if (!header.isSetField(FIX::FIELD::MsgSeqNum)) return;
    if (header.isSetField(FIX::FIELD::PossDupFlag) &&
        header.getField(FIX::FIELD::PossDupFlag) == "Y") {
        return;  // a resent message repeats a sequence number
    }
    int seq = 0;
    try {
        seq = std::stoi(header.getField(FIX::FIELD::MsgSeqNum));
    } catch (const std::exception&) {
        return;
    }
    if (msg_type(message) == FIX::MsgType_Logon) {
        std::lock_guard lock(mutex_);
        expected_in_seq_ = seq + 1;
        return;
    }
    std::string reason;
    {
        std::lock_guard lock(mutex_);
        if (seq > expected_in_seq_) {
            reason = "sequence gap: expected MsgSeqNum " + std::to_string(expected_in_seq_) +
                     ", received " + std::to_string(seq);
        }
        if (seq >= expected_in_seq_) expected_in_seq_ = seq + 1;
    }
    if (reason.empty()) return;
    logger_.warn(reason);
    notify({SessionEvent::Kind::DataLoss, reason});
}

bool OeApplication::wait_logged_on(std::chrono::milliseconds timeout) const {
    std::unique_lock lock(mutex_);
    return logged_on_cv_.wait_for(lock, timeout, [this] { return logged_on_; });
}

bool OeApplication::logged_on() const {
    std::lock_guard lock(mutex_);
    return logged_on_;
}

std::string OeApplication::last_logout_reason() const {
    std::lock_guard lock(mutex_);
    return last_logout_reason_;
}

bool OeApplication::replaced() const {
    std::lock_guard lock(mutex_);
    return replaced_;
}

void OeApplication::notify(const SessionEvent& event) {
    if (callback_) callback_(event);
}

}  // namespace cpm::oms::fix
