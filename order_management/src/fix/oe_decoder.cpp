#include "fix/oe_decoder.hpp"

#include "cpm/fix_common/raw_log.hpp"

#include <quickfix/FixFieldNumbers.h>
#include <quickfix/FixValues.h>
#include <quickfix/Group.h>

#include <algorithm>
#include <ctime>
#include <optional>

namespace cpm::oms::fix {
namespace {

namespace F = FIX::FIELD;

constexpr int kSelfMatchPreventionId = 7928;
constexpr int kSelfMatchPreventionStrategy = 8000;
constexpr int kMarketSettlementReportId = 20105;
constexpr int kSettlementOutcome = 21052;
constexpr int kPositionAggregationMode = 21053;
constexpr int kLongQty = 21054;
constexpr int kShortQty = 21055;
constexpr int kPayoutAmount = 21056;
constexpr int kResentEventCount = 22005;
constexpr int kEventResendRejectReason = 22006;

std::string field(const FIX::FieldMap& map, int tag) {
    return map.isSetField(tag) ? map.getField(tag) : std::string();
}

std::string shown(const std::string& value) { return value.empty() ? "<missing>" : value; }

bool digits(const std::string& text, std::size_t pos, std::size_t count) {
    return std::all_of(text.begin() + static_cast<std::ptrdiff_t>(pos),
                       text.begin() + static_cast<std::ptrdiff_t>(pos + count),
                       [](char c) { return c >= '0' && c <= '9'; });
}

// TransactTime(60): "yyyyMMdd-HH:mm:ss[.S+]" UTC. The gateway sends
// microseconds and preserves the original value exactly on a replay.
std::optional<Clock::time_point> parse_utc_timestamp(const std::string& text) {
    constexpr std::size_t kBase = 17;
    if (text.size() < kBase) return std::nullopt;
    if (!digits(text, 0, 8) || text[8] != '-' || !digits(text, 9, 2) || text[11] != ':' ||
        !digits(text, 12, 2) || text[14] != ':' || !digits(text, 15, 2)) {
        return std::nullopt;
    }
    std::int64_t micros = 0;
    if (text.size() > kBase) {
        if (text[kBase] != '.') return std::nullopt;
        std::string fraction = text.substr(kBase + 1);
        if (fraction.empty() || fraction.size() > 9 || !digits(fraction, 0, fraction.size())) {
            return std::nullopt;
        }
        fraction.resize(6, '0');
        micros = std::stoll(fraction);
    }
    std::tm tm{};
    tm.tm_year = std::stoi(text.substr(0, 4)) - 1900;
    tm.tm_mon = std::stoi(text.substr(4, 2)) - 1;
    tm.tm_mday = std::stoi(text.substr(6, 2));
    tm.tm_hour = std::stoi(text.substr(9, 2));
    tm.tm_min = std::stoi(text.substr(12, 2));
    tm.tm_sec = std::stoi(text.substr(15, 2));
    const std::time_t secs = timegm(&tm);
    if (secs == static_cast<std::time_t>(-1)) return std::nullopt;
    return Clock::from_time_t(secs) + std::chrono::microseconds(micros);
}

ExecType exec_type_of(const std::string& value) {
    if (value == "0") return ExecType::New;
    if (value == "4") return ExecType::Canceled;
    if (value == "5") return ExecType::Replaced;
    if (value == "6") return ExecType::PendingCancel;
    if (value == "8") return ExecType::Rejected;
    if (value == "A") return ExecType::PendingNew;
    if (value == "C") return ExecType::Expired;
    if (value == "F") return ExecType::Trade;
    return ExecType::Unknown;
}

OrdStatus ord_status_of(const std::string& value) {
    if (value == "0") return OrdStatus::New;
    if (value == "1") return OrdStatus::PartiallyFilled;
    if (value == "2") return OrdStatus::Filled;
    if (value == "4") return OrdStatus::Canceled;
    if (value == "6") return OrdStatus::PendingCancel;
    if (value == "8") return OrdStatus::Rejected;
    if (value == "A") return OrdStatus::PendingNew;
    if (value == "C") return OrdStatus::Expired;
    if (value == "U") return OrdStatus::Undefined;
    return OrdStatus::Unknown;
}

CancelReason cancel_reason_of(const std::string& value) {
    if (value.empty()) return CancelReason::None;
    if (value == "1") return CancelReason::CancelOnDisconnect;
    if (value == "2") return CancelReason::InstrumentLifecycle;
    if (value == "7") return CancelReason::SelfMatchPrevention;
    if (value == "105") return CancelReason::AdminCancel;
    return CancelReason::Unknown;
}

SmpStrategy smp_of(const std::string& value) {
    if (value == "N") return SmpStrategy::CancelAggressing;
    if (value == "O") return SmpStrategy::CancelResting;
    if (value == "Q") return SmpStrategy::CancelBoth;
    return SmpStrategy::Unset;
}

PostOnly post_only_of(const std::string& value) {
    // MULTIPLECHARVALUE, space delimited; reject mode wins if both are set.
    if (value.find('A') != std::string::npos) return PostOnly::Reject;
    if (value.find('6') != std::string::npos) return PostOnly::Slide;
    return PostOnly::None;
}

// One message's worth of decoding. QuickFIX stays inside this class.
class Decoder {
public:
    Decoder(const FIX::Message& message, std::string_view api_key)
        : message_(message),
          api_key_(api_key),
          type_(field(message.getHeader(), F::MsgType)),
          cl_ord_id_(field(message, F::ClOrdID)),
          replayed_(field(message.getHeader(), F::PossResend) == "Y") {}

    DecodedBatch run() {
        try {
            if (type_ == FIX::MsgType_ExecutionReport) {
                execution_report();
            } else if (type_ == FIX::MsgType_OrderCancelReject) {
                cancel_reject();
            } else if (type_ == FIX::MsgType_OrderMassCancelReport) {
                mass_cancel_report();
            } else if (type_ == "UMS") {
                settlement_report();
            } else if (type_ == "F2") {
                out_.last_exec_ids.push_back({field(message_, F::RefSeqNum), field(message_, F::ExecID)});
            } else if (type_ == "F4") {
                out_.resend_completes.push_back(
                    {field(message_, F::RefSeqNum), integer(kResentEventCount).value_or(0)});
            } else if (type_ == "F5") {
                out_.resend_rejects.push_back({field(message_, F::RefSeqNum),
                                               integer(kEventResendRejectReason).value_or(0),
                                               field(message_, F::Text)});
            } else if (type_ == FIX::MsgType_BusinessMessageReject) {
                out_.business_rejects.push_back({field(message_, F::RefSeqNum),
                                                 field(message_, F::RefMsgType),
                                                 field(message_, F::BusinessRejectRefID),
                                                 integer(F::BusinessRejectReason).value_or(0),
                                                 field(message_, F::Text)});
            }
        } catch (const std::exception& e) {
            fail(std::string("malformed message: ") + e.what());
        }
        return std::move(out_);
    }

private:
    void fail(const std::string& reason) {
        out_.errors.push_back(
            {reason, type_, cl_ord_id_, cpm::fix::format_raw(message_.toString(), api_key_)});
    }

    std::optional<int> integer(int tag) {
        const auto text = field(message_, tag);
        if (text.empty()) return std::nullopt;
        const auto value = cpm::parse_signed_truncating(text, 0);
        if (!value) {
            fail("tag " + std::to_string(tag) + " is not an integer: " + text);
            return std::nullopt;
        }
        return static_cast<int>(*value);
    }

    std::optional<Qty> quantity(int tag, bool required) {
        const auto text = field(message_, tag);
        if (text.empty()) {
            if (required) fail("35=" + type_ + " without tag " + std::to_string(tag));
            return std::nullopt;
        }
        const auto value = cpm::parse_signed_truncating(text, kQtyScale);
        if (!value) fail("tag " + std::to_string(tag) + " is not a quantity: " + text);
        return value;
    }

    std::optional<Price> price(int tag) {
        const auto text = field(message_, tag);
        if (text.empty()) return std::nullopt;
        const auto value = cpm::parse_signed_truncating(text, kPriceScale);
        if (!value) fail("tag " + std::to_string(tag) + " is not a price: " + text);
        return value;
    }

    std::optional<Clock::time_point> transact_time(bool required) {
        const auto text = field(message_, F::TransactTime);
        if (text.empty()) {
            if (required) fail("35=" + type_ + " without TransactTime(60)");
            return Clock::time_point{};
        }
        const auto time = parse_utc_timestamp(text);
        if (!time) fail("35=" + type_ + " with malformed TransactTime(60)=" + text);
        return time;
    }

    void execution_report() {
        ExecutionReport report;
        report.cl_ord_id = cl_ord_id_;
        report.orig_cl_ord_id = field(message_, F::OrigClOrdID);
        report.order_id = field(message_, F::OrderID);
        report.exec_id = field(message_, F::ExecID);
        report.symbol = field(message_, F::Symbol);
        report.replayed = replayed_;
        if (report.order_id.empty() || report.exec_id.empty() || report.symbol.empty()) {
            fail("ExecutionReport missing OrderID(37), ExecID(17) or Symbol(55)");
            return;
        }

        const auto exec_type_text = field(message_, F::ExecType);
        report.exec_type = exec_type_of(exec_type_text);
        if (report.exec_type == ExecType::Unknown) {
            fail("ExecutionReport with ExecType(150)=" + shown(exec_type_text));
            return;
        }
        const auto status_text = field(message_, F::OrdStatus);
        report.status = ord_status_of(status_text);
        if (report.status == OrdStatus::Unknown) {
            fail("ExecutionReport with OrdStatus(39)=" + shown(status_text));
            return;
        }

        const auto side_text = field(message_, F::Side);
        if (side_text == "1") {
            report.side = Side::Buy;
        } else if (side_text == "2") {
            report.side = Side::Sell;
        } else {
            fail("ExecutionReport with Side(54)=" + shown(side_text));
            return;
        }

        const auto leaves = quantity(F::LeavesQty, true);
        const auto cumulative = quantity(F::CumQty, true);
        const auto time = transact_time(true);
        if (!leaves || !cumulative || !time) return;
        report.leaves_quantity = *leaves;
        report.cumulative_quantity = *cumulative;
        report.transact_time = *time;

        report.order_quantity = quantity(F::OrderQty, false);
        report.price = price(F::Price);
        report.average_price = price(F::AvgPx);
        report.manual_order = field(message_, F::ManualOrderIndicator) == "Y";
        report.cancel_reason = cancel_reason_of(field(message_, F::ExecRestatementReason));
        report.text = field(message_, F::Text);
        report.post_only = post_only_of(field(message_, F::ExecInst));
        if (const auto smp = field(message_, kSelfMatchPreventionId); !smp.empty()) {
            report.smp_id = integer(kSelfMatchPreventionId);
        }
        report.smp_strategy = smp_of(field(message_, kSelfMatchPreventionStrategy));

        if (const auto tif = field(message_, F::TimeInForce); !tif.empty()) {
            if (tif == "1") report.time_in_force = TimeInForce::Gtc;
            else if (tif == "3") report.time_in_force = TimeInForce::Ioc;
            else if (tif == "4") report.time_in_force = TimeInForce::Fok;
            else fail("ExecutionReport with TimeInForce(59)=" + tif);
        }
        if (const auto ord_type = field(message_, F::OrdType); !ord_type.empty()) {
            if (ord_type == "1") report.type = OrderType::Market;
            else if (ord_type == "2") report.type = OrderType::Limit;
            else fail("ExecutionReport with OrdType(40)=" + ord_type);
        }
        // OrdRejReason is present on a Rejected report only, never on an
        // Expired or Canceled one.
        if (!field(message_, F::OrdRejReason).empty()) report.reject_reason = integer(F::OrdRejReason);

        if (report.exec_type == ExecType::Trade) {
            Fill fill;
            const auto last_qty = quantity(F::LastQty, true);
            const auto last_px = price(F::LastPx);
            if (!last_qty || !last_px) {
                fail("trade report without LastQty(32) or LastPx(31)");
                return;
            }
            fill.quantity = *last_qty;
            fill.price = *last_px;
            fill.trade_match_id = field(message_, F::TrdMatchID);
            fill.aggressor = field(message_, F::AggressorIndicator) == "Y";
            // Exactly one fee entry: positive a fee, negative a maker rebate.
            if (message_.groupCount(F::NoMiscFees) > 0) {
                FIX::Group fees(F::NoMiscFees, F::MiscFeeAmt);
                message_.getGroup(1, fees);
                const auto text = field(fees, F::MiscFeeAmt);
                if (!text.empty()) {
                    const auto amount = cpm::parse_amount(text);
                    if (amount) {
                        fill.fee = *amount;
                    } else {
                        fail("trade report with MiscFeeAmt(137)=" + text);
                    }
                }
            }
            report.fill = fill;
        }
        out_.reports.push_back(std::move(report));
    }

    void cancel_reject() {
        CancelReject reject;
        reject.cl_ord_id = cl_ord_id_;
        reject.orig_cl_ord_id = field(message_, F::OrigClOrdID);
        reject.order_id = field(message_, F::OrderID);
        reject.text = field(message_, F::Text);
        reject.replayed = replayed_;
        if (!field(message_, F::CxlRejReason).empty()) reject.reason = integer(F::CxlRejReason);
        out_.cancel_rejects.push_back(std::move(reject));
    }

    void mass_cancel_report() {
        MassCancelReport report;
        report.cl_ord_id = cl_ord_id_;
        report.report_id = field(message_, F::MassActionReportID);
        report.replayed = replayed_;
        const auto scope = field(message_, F::MassCancelRequestType);
        if (scope == "1") report.requested_scope = MassCancel::Scope::Security;
        else if (scope == "6") report.requested_scope = MassCancel::Scope::TradingSession;
        else if (scope == "7") report.requested_scope = MassCancel::Scope::All;
        else fail("OrderMassCancelReport with MassCancelRequestType(530)=" + shown(scope));

        const auto response = field(message_, F::MassCancelResponse);
        if (response == "0") {
            report.accepted = false;
        } else if (response == "7" || response == "1") {
            report.accepted = true;
        } else {
            fail("OrderMassCancelReport with MassCancelResponse(531)=" + shown(response));
            return;
        }
        if (!field(message_, F::MassCancelRejectReason).empty()) {
            report.reject_reason = integer(F::MassCancelRejectReason);
        }
        if (!field(message_, F::TotalAffectedOrders).empty()) {
            report.affected_orders = integer(F::TotalAffectedOrders);
        }
        report.text = field(message_, F::Text);
        const auto time = transact_time(true);
        if (!time) return;
        report.transact_time = *time;
        out_.mass_cancel_reports.push_back(std::move(report));
    }

    void settlement_report() {
        SettlementReport settlement;
        settlement.report_id = field(message_, kMarketSettlementReportId);
        settlement.symbol = field(message_, F::Symbol);
        settlement.replayed = replayed_;
        if (settlement.symbol.empty()) {
            fail("MarketSettlementReport without Symbol(55)");
            return;
        }
        // Two entries: ClearingFirm(4) then ExecutingFirm(1).
        FIX::Group party(F::NoPartyIDs, F::PartyID);
        for (std::size_t i = 1; i <= message_.groupCount(F::NoPartyIDs); ++i) {
            message_.getGroup(static_cast<unsigned>(i), party);
            const auto role = field(party, F::PartyRole);
            if (role == "4") settlement.clearing_firm = field(party, F::PartyID);
            if (role == "1") settlement.executing_firm = field(party, F::PartyID);
        }

        const auto outcome = field(message_, kSettlementOutcome);
        if (outcome == "Y") settlement.outcome = SettlementReport::Outcome::Yes;
        else if (outcome == "N") settlement.outcome = SettlementReport::Outcome::No;
        else if (outcome == "S") settlement.outcome = SettlementReport::Outcome::Scalar;
        else fail("MarketSettlementReport with SettlementOutcome(21052)=" + shown(outcome));

        settlement.net_aggregation = field(message_, kPositionAggregationMode) != "G";
        const auto settlement_price = price(F::SettlPrice);
        const auto long_qty = quantity(kLongQty, true);
        const auto short_qty = quantity(kShortQty, true);
        const auto payout_text = field(message_, kPayoutAmount);
        const auto payout = payout_text.empty() ? std::nullopt : cpm::parse_amount(payout_text);
        const auto time = transact_time(true);
        if (!settlement_price || !long_qty || !short_qty || !payout || !time) {
            if (!payout) fail("MarketSettlementReport with PayoutAmount(21056)=" + shown(payout_text));
            return;
        }
        settlement.settlement_price = *settlement_price;
        settlement.long_quantity = *long_qty;
        settlement.short_quantity = *short_qty;
        settlement.payout = *payout;
        settlement.transact_time = *time;
        out_.settlements.push_back(std::move(settlement));
    }

    const FIX::Message& message_;
    std::string_view api_key_;
    std::string type_;
    std::string cl_ord_id_;
    bool replayed_ = false;
    DecodedBatch out_;
};

}  // namespace

bool DecodedBatch::empty() const {
    return reports.empty() && cancel_rejects.empty() && mass_cancel_reports.empty() &&
           settlements.empty() && last_exec_ids.empty() && resend_completes.empty() &&
           resend_rejects.empty() && business_rejects.empty() && errors.empty();
}

DecodedBatch decode_message(const FIX::Message& message, std::string_view api_key) {
    return Decoder(message, api_key).run();
}

}  // namespace cpm::oms::fix
