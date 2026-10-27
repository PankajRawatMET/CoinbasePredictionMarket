#include "fix/sim_application.hpp"

#include "cpm/common/signing.hpp"
#include "cpm/fix_common/logon_auth.hpp"

#include <quickfix/FixFieldNumbers.h>
#include <quickfix/FixValues.h>
#include <quickfix/Group.h>
#include <quickfix/Session.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <limits>

namespace cpm::oms::sim::fix {
namespace {

namespace F = FIX::FIELD;

constexpr int kMaxExecutionCost = 21009;
constexpr int kSelfMatchPreventionId = 7928;
constexpr int kSelfMatchPreventionStrategy = 8000;
constexpr int kBeginExecId = 22003;
constexpr int kEndExecId = 22004;
constexpr int kResentEventCount = 22005;
constexpr int kEventResendRejectReason = 22006;

std::string utc_timestamp(Clock::time_point t) {
    const std::time_t secs = Clock::to_time_t(t);
    std::tm tm{};
    gmtime_r(&secs, &tm);
    const auto us =
        std::chrono::duration_cast<std::chrono::microseconds>(t.time_since_epoch()).count() % 1000000;
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d-%02d:%02d:%02d.%06lld", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<long long>(us));
    return buf;
}

std::optional<Clock::time_point> parse_sending_time(const std::string& text) {
    std::tm tm{};
    int ms = 0;
    if (std::sscanf(text.c_str(), "%4d%2d%2d-%2d:%2d:%2d.%3d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                    &tm.tm_hour, &tm.tm_min, &tm.tm_sec, &ms) < 6) {
        return std::nullopt;
    }
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    return Clock::from_time_t(timegm(&tm)) + std::chrono::milliseconds(ms);
}

std::string field_or_empty(const FIX::FieldMap& map, int tag) {
    return map.isSetField(tag) ? map.getField(tag) : std::string();
}

const char* side_value(Side side) { return side == Side::Buy ? "1" : "2"; }

std::optional<Side> side_of(const std::string& value) {
    if (value == "1") return Side::Buy;
    if (value == "2") return Side::Sell;
    return std::nullopt;
}

// ExecType and OrdStatus for one event. A resting order that has already
// partially filled reports PartiallyFilled, not New.
std::pair<const char*, const char*> status_of(const Event& event) {
    switch (event.kind) {
        case Event::Kind::PendingNew: return {"A", "A"};
        case Event::Kind::New: return {"0", event.cumulative > 0 ? "1" : "0"};
        case Event::Kind::Fill: return {"F", event.leaves == 0 ? "2" : "1"};
        case Event::Kind::Canceled: return {"4", "4"};
        case Event::Kind::Expired: return {"C", "C"};
        case Event::Kind::Rejected: return {"8", "8"};
        default: return {"8", "8"};
    }
}

FIX::Message new_message(const char* type, const FIX::message_order& body) {
    FIX::Message message(FIX::message_order(FIX::message_order::header),
                         FIX::message_order(FIX::message_order::trailer), body);
    message.getHeader().setField(F::MsgType, type);
    return message;
}

}  // namespace

SimApplication::SimApplication(const SimOptions& options, Logger& logger)
    : options_(options), logger_(logger), engine_(options.instruments, options.fees) {
    if (!options_.seed_book) return;
    // A little resting liquidity on both sides of every instrument, so a taker
    // has something to trade against.
    const auto now = Clock::now();
    for (const auto& instrument : options_.instruments) {
        for (int step = 0; step < 3; ++step) {
            NewOrder bid;
            bid.cl_ord_id = "seed-b" + std::to_string(step) + instrument.symbol.substr(0, 3);
            bid.symbol = instrument.symbol;
            bid.side = Side::Buy;
            bid.quantity = 200;
            bid.price = 450 - step * 10;
            engine_.submit(bid, "seed-account", now, "seed");

            NewOrder ask = bid;
            ask.cl_ord_id = "seed-a" + std::to_string(step) + instrument.symbol.substr(0, 3);
            ask.side = Side::Sell;
            ask.price = 550 + step * 10;
            engine_.submit(ask, "seed-account", now, "seed");
        }
    }
}

void SimApplication::onLogon(const FIX::SessionID& session_id) {
    {
        std::lock_guard lock(mutex_);
        session_id_ = session_id;
        logged_on_ = true;
        ++logons_;
        client_logout_ = false;
    }
    logged_on_cv_.notify_all();
    logger_.info("oe_sim: client logged on");
}

void SimApplication::onLogout(const FIX::SessionID&) {
    std::lock_guard lock(mutex_);
    logged_on_ = false;
}

void SimApplication::toAdmin(FIX::Message& message, const FIX::SessionID&) {
    message.getHeader().setField(F::SenderSubID, "TEST");  // as the sandbox does
    if (message.getHeader().getField(F::MsgType) == FIX::MsgType_Logout && !message.isSetField(F::Text)) {
        std::lock_guard lock(mutex_);
        if (client_logout_) message.setField(F::Text, "CLIENT_LOGOUT");
    }
}

void SimApplication::toApp(FIX::Message& message, const FIX::SessionID&) {
    message.getHeader().setField(F::SenderSubID, "TEST");
}

void SimApplication::fromAdmin(const FIX::Message& message, const FIX::SessionID&) {
    const auto type = message.getHeader().getField(F::MsgType);
    if (type == FIX::MsgType_Logout) {
        std::lock_guard lock(mutex_);
        client_logout_ = true;
        return;
    }
    if (type != FIX::MsgType_Logon) return;
    // Spec "Authentication": Password(554) plus an HMAC over
    // SendingTime(ms) + ApiKey + TargetCompID + Password, with TargetCompID=CPMOE.
    const auto& h = message.getHeader();
    const auto& creds = options_.credentials;
    if (field_or_empty(message, F::Password) != creds.passphrase) throw FIX::RejectLogon("invalid passphrase");
    const auto aligned = cpm::fix::align_sending_time(h.getField(F::SendingTime));
    const auto expected = cpm::fix_logon_signature(aligned.signed_, h.getField(F::SenderCompID),
                                                   h.getField(F::TargetCompID), creds.passphrase,
                                                   creds.secret_b64);
    if (field_or_empty(message, F::Text) != expected) throw FIX::RejectLogon("invalid signature");
    const auto sent = parse_sending_time(h.getField(F::SendingTime));
    const auto skew = sent ? Clock::now() - *sent : Clock::duration::max();
    if (!sent || std::chrono::abs(skew) > std::chrono::seconds(options_.clock_window_secs)) {
        throw FIX::RejectLogon("SendingTime outside the signature window");
    }
    logger_.info(std::string("oe_sim: logon options 8013=") + field_or_empty(message, 8013) +
                 " 20127=" + field_or_empty(message, 20127));
}

void SimApplication::fromApp(const FIX::Message& message, const FIX::SessionID&) {
    const auto type = message.getHeader().getField(F::MsgType);
    if (type == FIX::MsgType_NewOrderSingle) {
        handle_new_order(message);
    } else if (type == FIX::MsgType_OrderCancelRequest) {
        handle_cancel(message);
    } else if (type == FIX::MsgType_OrderMassCancelRequest) {
        handle_mass_cancel(message);
    } else if (type == "F1") {
        handle_last_exec_id_request(message);
    } else if (type == "F3") {
        handle_event_resend_request(message);
    } else {
        // An unrecognized application MsgType is answered with j.
        FIX::Message reject = new_message(FIX::MsgType_BusinessMessageReject,
                                          FIX::message_order(F::RefSeqNum, F::RefMsgType,
                                                             F::BusinessRejectReason, F::Text, 0));
        reject.setField(F::RefSeqNum, message.getHeader().getField(F::MsgSeqNum));
        reject.setField(F::RefMsgType, type);
        reject.setField(F::BusinessRejectReason, "0");
        reject.setField(F::Text, "unsupported message type");
        send(std::move(reject), false);
    }
}

std::string SimApplication::next_exec_id() { return std::to_string(next_exec_ += 2); }

void SimApplication::handle_new_order(const FIX::Message& message) {
    NewOrder order;
    order.cl_ord_id = field_or_empty(message, F::ClOrdID);
    order.symbol = field_or_empty(message, F::Symbol);
    if (const auto side = side_of(field_or_empty(message, F::Side))) order.side = *side;
    order.quantity = cpm::parse_signed_truncating(field_or_empty(message, F::OrderQty), kQtyScale).value_or(0);
    const auto price_text = field_or_empty(message, F::Price);
    order.price = price_text.empty()
                      ? 0
                      : cpm::parse_signed_truncating(price_text, kPriceScale).value_or(0);
    order.type = field_or_empty(message, F::OrdType) == "1" ? OrderType::Market : OrderType::Limit;
    const auto tif = field_or_empty(message, F::TimeInForce);
    order.time_in_force = tif == "3" ? TimeInForce::Ioc : (tif == "4" ? TimeInForce::Fok : TimeInForce::Gtc);
    const auto exec_inst = field_or_empty(message, F::ExecInst);
    if (exec_inst.find('A') != std::string::npos) order.post_only = PostOnly::Reject;
    else if (exec_inst.find('6') != std::string::npos) order.post_only = PostOnly::Slide;
    if (const auto cap = field_or_empty(message, kMaxExecutionCost); !cap.empty()) {
        order.max_execution_cost = cpm::parse_amount(cap).value_or(0);
    }
    if (const auto smp = field_or_empty(message, kSelfMatchPreventionId); !smp.empty()) {
        // Accepted and echoed, but not simulated: see oe_sim's NOTES.
        order.smp_id = std::stoi(smp);
    }

    // Exactly one Parties entry with PartyRole=24 is required.
    std::string subaccount;
    FIX::Group party(F::NoPartyIDs, F::PartyID);
    for (std::size_t i = 1; i <= message.groupCount(F::NoPartyIDs); ++i) {
        message.getGroup(static_cast<unsigned>(i), party);
        if (field_or_empty(party, F::PartyRole) == "24") subaccount = field_or_empty(party, F::PartyID);
    }

    std::lock_guard lock(mutex_);
    publish(engine_.submit(order, subaccount, Clock::now()));
}

void SimApplication::handle_cancel(const FIX::Message& message) {
    CancelOrder request;
    request.cl_ord_id = field_or_empty(message, F::ClOrdID);
    request.orig_cl_ord_id = field_or_empty(message, F::OrigClOrdID);
    request.order_id = field_or_empty(message, F::OrderID);
    request.symbol = field_or_empty(message, F::Symbol);
    if (const auto side = side_of(field_or_empty(message, F::Side))) request.side = *side;

    std::lock_guard lock(mutex_);
    publish(engine_.cancel(request, Clock::now()));
}

void SimApplication::handle_mass_cancel(const FIX::Message& message) {
    MassCancel request;
    request.cl_ord_id = field_or_empty(message, F::ClOrdID);
    const auto scope = field_or_empty(message, F::MassCancelRequestType);
    request.symbol = field_or_empty(message, F::Symbol);
    if (const auto side = side_of(field_or_empty(message, F::Side))) request.side = *side;
    request.filter_by_subaccount = message.groupCount(F::NoPartyIDs) > 0;
    std::string subaccount;
    if (request.filter_by_subaccount) {
        FIX::Group party(F::NoPartyIDs, F::PartyID);
        message.getGroup(1, party);
        subaccount = field_or_empty(party, F::PartyID);
    }

    std::lock_guard lock(mutex_);
    const auto now = Clock::now();
    FIX::Message report = new_message(FIX::MsgType_OrderMassCancelReport,
                                      FIX::message_order(F::ClOrdID, F::MassActionReportID,
                                                         F::MassCancelRequestType, F::MassCancelResponse,
                                                         F::MassCancelRejectReason,
                                                         F::TotalAffectedOrders, F::TransactTime,
                                                         F::Text, 0));
    report.setField(F::ClOrdID, request.cl_ord_id);
    report.setField(F::MassCancelRequestType, scope.empty() ? "7" : scope);

    if (scope != "1" && scope != "6" && scope != "7") {
        // Rejected on receipt: MassActionReportID is the sentinel 0, the count
        // is forced to 0, and the request is not available for F3 replay.
        report.setField(F::MassActionReportID, "0");
        report.setField(F::MassCancelResponse, "0");
        report.setField(F::MassCancelRejectReason, "99");
        report.setField(F::TotalAffectedOrders, "0");
        report.setField(F::TransactTime, utc_timestamp(now));
        report.setField(F::Text, "unsupported MassCancelRequestType");
        send(std::move(report), false);
        return;
    }
    if (scope == "1" && engine_.find(request.symbol) == nullptr) {
        report.setField(F::MassActionReportID, "0");
        report.setField(F::MassCancelResponse, "0");
        report.setField(F::MassCancelRejectReason, "1");
        report.setField(F::TotalAffectedOrders, "0");
        report.setField(F::TransactTime, utc_timestamp(now));
        report.setField(F::Text, "unknown security");
        send(std::move(report), false);
        return;
    }
    request.scope = scope == "1" ? MassCancel::Scope::Security
                                 : (scope == "6" ? MassCancel::Scope::TradingSession
                                                 : MassCancel::Scope::All);

    // The individual reports come first, then the summary.
    const auto canceled = engine_.mass_cancel(request, subaccount, now);
    publish(canceled);
    report.setField(F::MassActionReportID, next_exec_id());
    // An accepted mass cancel always reports 7, whatever scope was asked for.
    report.setField(F::MassCancelResponse, "7");
    report.setField(F::TotalAffectedOrders, std::to_string(canceled.size()));
    report.setField(F::TransactTime, utc_timestamp(now));
    send(std::move(report), true);
}

void SimApplication::handle_last_exec_id_request(const FIX::Message& message) {
    std::lock_guard lock(mutex_);
    FIX::Message reply = new_message("F2", FIX::message_order(F::RefSeqNum, F::ExecID, 0));
    reply.setField(F::RefSeqNum, message.getHeader().getField(F::MsgSeqNum));
    // The sentinel 0 when the retention window holds nothing for this user.
    reply.setField(F::ExecID, history_.empty() ? "0" : std::to_string(history_.back().first));
    send(std::move(reply), false);
}

void SimApplication::handle_event_resend_request(const FIX::Message& message) {
    std::lock_guard lock(mutex_);
    const auto ref_seq = message.getHeader().getField(F::MsgSeqNum);
    const auto begin_text = field_or_empty(message, kBeginExecId);
    const auto end_text = field_or_empty(message, kEndExecId);
    std::uint64_t begin = 0;
    std::uint64_t end = std::numeric_limits<std::uint64_t>::max();
    try {
        begin = std::stoull(begin_text);
        if (!end_text.empty()) end = std::stoull(end_text);
    } catch (const std::exception&) {
        begin = 1;
        end = 0;  // forces the no-overlap reject below
    }

    std::vector<FIX::Message> replay;
    for (const auto& [exec_id, stored] : history_) {
        if (exec_id >= begin && exec_id <= end) replay.push_back(stored);
    }
    if (replay.empty()) {
        // Rejected only when the range overlaps no stored event.
        FIX::Message reject = new_message("F5", FIX::message_order(F::RefSeqNum, kEventResendRejectReason,
                                                                   F::Text, 0));
        reject.setField(F::RefSeqNum, ref_seq);
        reject.setField(kEventResendRejectReason, "1");
        reject.setField(F::Text, "BEGIN_EXEC_ID_TOO_SMALL");
        send(std::move(reject), false);
        return;
    }
    for (auto& stored : replay) {
        // A replay is a new message with a fresh MsgSeqNum carrying
        // PossResend(97)=Y; it is not a PossDupFlag gap fill, and TransactTime
        // keeps the original event's value.
        stored.getHeader().setField(F::PossResend, "Y");
        send(std::move(stored), false);
    }
    FIX::Message complete = new_message("F4", FIX::message_order(F::RefSeqNum, kResentEventCount, 0));
    complete.setField(F::RefSeqNum, ref_seq);
    complete.setField(kResentEventCount, std::to_string(replay.size()));
    send(std::move(complete), false);
}

void SimApplication::publish(const std::vector<Event>& events) {
    for (const auto& event : events) {
        // Each side of a trade is reported to its own owner, so the session is
        // never told about the simulator's seeded liquidity. (A drop-copy feed
        // would see both sides; this is the order entry session.)
        if (!event.owner.empty() && event.owner != "client") continue;
        if (event.kind == Event::Kind::BusinessRejected) {
            FIX::Message reject = new_message(FIX::MsgType_BusinessMessageReject,
                                              FIX::message_order(F::RefMsgType, F::BusinessRejectRefID,
                                                                 F::BusinessRejectReason, F::Text, 0));
            reject.setField(F::RefMsgType, "D");
            reject.setField(F::BusinessRejectRefID, event.cl_ord_id);
            reject.setField(F::BusinessRejectReason, "0");
            reject.setField(F::Text, event.text);
            send(std::move(reject), false);
            continue;
        }
        if (event.kind == Event::Kind::CancelRejected) {
            FIX::Message reject = new_message(FIX::MsgType_OrderCancelReject,
                                              FIX::message_order(F::ClOrdID, F::OrigClOrdID, F::OrderID,
                                                                 F::OrdStatus, F::CxlRejReason,
                                                                 F::CxlRejResponseTo, F::Text, 0));
            reject.setField(F::ClOrdID, event.cl_ord_id);
            if (!event.orig_cl_ord_id.empty()) reject.setField(F::OrigClOrdID, event.orig_cl_ord_id);
            reject.setField(F::OrderID, event.order_id);
            reject.setField(F::OrdStatus, "U");  // the reject carries no order state
            reject.setField(F::CxlRejReason, std::to_string(event.cancel_reject_reason));
            reject.setField(F::CxlRejResponseTo, "1");
            reject.setField(F::Text, event.text);
            send(std::move(reject), true);
            continue;
        }

        // ExecutionReport, in the field order observed on the sandbox (OE-O2);
        // the fill fields are placed by the spec's example, since no fill has
        // been observed yet.
        FIX::Message report = new_message(
            FIX::MsgType_ExecutionReport,
            FIX::message_order(F::ClOrdID, F::OrigClOrdID, F::OrderID, F::ExecID, F::ExecType,
                               F::OrdStatus, F::Symbol, F::Side, F::OrderQty, F::Price, F::OrdType,
                               F::TimeInForce, F::ExecInst, F::ManualOrderIndicator, F::LastQty,
                               F::LastPx, F::LeavesQty, F::CumQty, F::AvgPx, F::TrdMatchID,
                               F::AggressorIndicator, F::NoMiscFees, F::OrdRejReason,
                               F::TransactTime, F::Text, F::NoPartyIDs, 0));
        const auto [exec_type, ord_status] = status_of(event);
        report.setField(F::ClOrdID, event.cl_ord_id);
        if (!event.orig_cl_ord_id.empty()) report.setField(F::OrigClOrdID, event.orig_cl_ord_id);
        report.setField(F::OrderID, event.order_id.empty() ? "0" : event.order_id);
        const auto exec_id = next_exec_id();
        report.setField(F::ExecID, exec_id);
        report.setField(F::ExecType, exec_type);
        report.setField(F::OrdStatus, ord_status);
        report.setField(F::Symbol, event.symbol);
        report.setField(F::Side, side_value(event.side));
        report.setField(F::OrderQty, format_qty(event.order_quantity));
        if (event.price > 0) report.setField(F::Price, format_price(event.price));
        if (event.type) report.setField(F::OrdType, *event.type == OrderType::Market ? "1" : "2");
        if (event.time_in_force) {
            report.setField(F::TimeInForce, *event.time_in_force == TimeInForce::Ioc
                                                ? "3"
                                                : (*event.time_in_force == TimeInForce::Fok ? "4" : "1"));
        }
        if (event.post_only == PostOnly::Reject) report.setField(F::ExecInst, "A");
        if (event.post_only == PostOnly::Slide) report.setField(F::ExecInst, "6");
        report.setField(F::ManualOrderIndicator, "N");
        report.setField(F::LeavesQty, format_qty(event.leaves));
        report.setField(F::CumQty, format_qty(event.cumulative));
        report.setField(F::AvgPx, format_price(event.average_price));
        if (event.kind == Event::Kind::Fill) {
            report.setField(F::LastQty, format_qty(event.last_quantity));
            report.setField(F::LastPx, format_price(event.last_price));
            report.setField(F::TrdMatchID, event.trade_match_id);
            report.setField(F::AggressorIndicator, event.aggressor ? "Y" : "N");
            // Exactly one fee entry: positive a fee, negative a maker rebate.
            FIX::Group fee(F::NoMiscFees, F::MiscFeeAmt,
                           FIX::message_order(F::MiscFeeAmt, F::MiscFeeCurr, F::MiscFeeType, 0));
            fee.setField(F::MiscFeeAmt, format_amount(event.fee));
            fee.setField(F::MiscFeeCurr, "USDC");
            fee.setField(F::MiscFeeType, "4");
            report.addGroup(fee);
        }
        // OrdRejReason is on a Rejected report only, never on Expired.
        if (event.kind == Event::Kind::Rejected && event.reject_reason > 0) {
            report.setField(F::OrdRejReason, std::to_string(event.reject_reason));
        }
        report.setField(F::TransactTime, utc_timestamp(event.time));
        if (!event.text.empty()) report.setField(F::Text, event.text);

        // The five entries the gateway populates, in the observed order and
        // with PartyID before PartyRole (OE-O2).
        const std::pair<const char*, std::string> parties[] = {
            {"4", options_.clearing_firm},
            {"1", options_.executing_firm},
            {"24", event.subaccount},
            {"11", options_.trader},
            {"55", options_.credentials.api_key}};
        for (const auto& [role, id] : parties) {
            FIX::Group party(F::NoPartyIDs, F::PartyID,
                             FIX::message_order(F::PartyID, F::PartyRole, 0));
            party.setField(F::PartyID, id);
            party.setField(F::PartyRole, role);
            report.addGroup(party);
        }
        send(std::move(report), true);
    }
}

void SimApplication::send(FIX::Message message, bool replayable) {
    if (replayable && message.isSetField(F::ExecID)) {
        try {
            history_.emplace_back(std::stoull(message.getField(F::ExecID)), message);
        } catch (const std::exception&) {
        }
    } else if (replayable && message.isSetField(20105)) {
        try {
            history_.emplace_back(std::stoull(message.getField(20105)), message);
        } catch (const std::exception&) {
        }
    } else if (replayable && message.isSetField(F::MassActionReportID)) {
        try {
            history_.emplace_back(std::stoull(message.getField(F::MassActionReportID)), message);
        } catch (const std::exception&) {
        }
    }
    if (!session_id_) return;
    try {
        FIX::Session::sendToTarget(message, *session_id_);
    } catch (const FIX::SessionNotFound&) {
    }
}

void SimApplication::settle(const std::string& symbol, SettlementReport::Outcome outcome, Price price,
                            Qty long_quantity, Qty short_quantity) {
    std::lock_guard lock(mutex_);
    FIX::Message report = new_message("UMS",
                                      FIX::message_order(20105, F::Symbol, F::NoPartyIDs, 21052,
                                                         F::SettlPrice, 21053, 21054, 21055, 21056,
                                                         F::TransactTime, 0));
    report.setField(20105, next_exec_id());
    report.setField(F::Symbol, symbol);
    // Two entries: ClearingFirm(4) then ExecutingFirm(1).
    for (const auto& [role, id] : {std::pair<const char*, std::string>{"4", options_.clearing_firm},
                                   std::pair<const char*, std::string>{"1", options_.executing_firm}}) {
        FIX::Group party(F::NoPartyIDs, F::PartyID, FIX::message_order(F::PartyID, F::PartyRole, 0));
        party.setField(F::PartyID, id);
        party.setField(F::PartyRole, role);
        report.addGroup(party);
    }
    const char* outcome_value = outcome == SettlementReport::Outcome::Yes
                                    ? "Y"
                                    : (outcome == SettlementReport::Outcome::No ? "N" : "S");
    report.setField(21052, outcome_value);
    report.setField(F::SettlPrice, format_price(price));
    report.setField(21053, "N");  // net aggregation
    report.setField(21054, format_qty(long_quantity));
    report.setField(21055, format_qty(short_quantity));
    // payout = price x long + (1 - price) x short
    const Amount payout = (price * long_quantity + (kPriceOne - price) * short_quantity) * 10;
    report.setField(21056, format_amount(payout));
    report.setField(F::TransactTime, utc_timestamp(Clock::now()));
    send(std::move(report), true);
}

void SimApplication::tick(Clock::time_point now) {
    std::lock_guard lock(mutex_);
    publish(engine_.release_speed_bumped(now));
}

bool SimApplication::wait_logged_on(std::chrono::milliseconds timeout) const {
    std::unique_lock lock(mutex_);
    return logged_on_cv_.wait_for(lock, timeout, [this] { return logged_on_; });
}

int SimApplication::logons() const {
    std::lock_guard lock(mutex_);
    return logons_;
}

std::vector<RestingOrder> SimApplication::book(const std::string& symbol, Side side) const {
    std::lock_guard lock(mutex_);
    return engine_.book(symbol, side);
}

}  // namespace cpm::oms::sim::fix
