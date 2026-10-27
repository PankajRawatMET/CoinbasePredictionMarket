// Both directions of the order path. Outbound: every message the builder
// produces is re-parsed and validated against config/*-CPMOE.xml, so what we
// would send is known to be dictionary-valid. Inbound: the spec's own worked
// reports decode into the plain structs, and malformed ones are reported.
#include "cpm/fix_common/session_settings.hpp"
#include "fix/oe_decoder.hpp"
#include "fix/oe_messages.hpp"

#include "fix_test_util.hpp"

#include <quickfix/DataDictionary.h>
#include <quickfix/FixFieldNumbers.h>
#include <quickfix/Message.h>

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

namespace F = FIX::FIELD;

using cpm::fix::test::build_fix;
using cpm::oms::CancelOrder;
using cpm::oms::CancelReason;
using cpm::oms::ExecType;
using cpm::oms::MassCancel;
using cpm::oms::NewOrder;
using cpm::oms::OrderType;
using cpm::oms::OrdStatus;
using cpm::oms::PostOnly;
using cpm::oms::Side;
using cpm::oms::SmpStrategy;
using cpm::oms::TimeInForce;
using Fields = std::vector<std::pair<int, std::string>>;

constexpr int kMaxExecutionCost = 21009;
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
constexpr int kBeginExecId = 22003;
constexpr int kEndExecId = 22004;

class OeMessages : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        transport_ = new FIX::DataDictionary(std::string(CPM_DICT_DIR) + "/" + cpm::fix::kOeTransportDictionary);
        app_ = new FIX::DataDictionary(std::string(CPM_DICT_DIR) + "/" + cpm::fix::kOeAppDictionary);
    }
    static void TearDownTestSuite() {
        delete transport_;
        delete app_;
    }

    static cpm::OeConfig config(bool manual = false) {
        cpm::OeConfig c;
        c.subaccount = "my-subaccount";
        c.manual_orders = manual;
        return c;
    }

    // Stamps the header QuickFIX would fill on send, then re-parses and
    // validates: a message that fails here would be rejected by our own engine.
    static FIX::Message round_trip(FIX::Message built) {
        auto& header = built.getHeader();
        // The session normally supplies these; without BeginString the wire
        // starts at BodyLength and the header is "out of order".
        header.setField(F::BeginString, "FIXT.1.1");
        header.setField(F::SenderCompID, "test-key");
        header.setField(F::TargetCompID, "CPMOE");
        header.setField(F::MsgSeqNum, "2");
        header.setField(F::SendingTime, "20240701-12:00:01.000000");
        const auto wire = built.toString();
        FIX::Message parsed(wire, *transport_, *app_, true);
        EXPECT_NO_THROW(FIX::DataDictionary::validate(parsed, transport_, app_)) << wire;
        return parsed;
    }

    static cpm::oms::fix::DecodedBatch decode(const std::string& type, const Fields& body) {
        Fields fields{{35, type}, {49, "CPMOE"}, {56, "test-key"}, {34, "2"},
                      {52, "20240701-12:00:01.000000"}};
        fields.insert(fields.end(), body.begin(), body.end());
        FIX::Message message(build_fix(fields), *transport_, *app_, true);
        return cpm::oms::fix::decode_message(message, "test-key");
    }

    static inline FIX::DataDictionary* transport_ = nullptr;
    static inline FIX::DataDictionary* app_ = nullptr;
};

NewOrder limit_buy() {
    NewOrder order;
    order.cl_ord_id = "order-001";
    order.symbol = "MKT-A";
    order.side = Side::Buy;
    order.quantity = 100;
    order.type = OrderType::Limit;
    order.price = 650;
    order.time_in_force = TimeInForce::Gtc;
    return order;
}

// --- outbound --------------------------------------------------------------

// The spec's NewOrderSingle example, field for field.
TEST_F(OeMessages, BuildsTheSpecsExampleOrder) {
    const auto m = round_trip(cpm::oms::fix::new_order_single(limit_buy(), config()));
    EXPECT_EQ(m.getHeader().getField(F::MsgType), "D");
    EXPECT_EQ(m.getField(F::ClOrdID), "order-001");
    EXPECT_EQ(m.getField(F::Symbol), "MKT-A");
    EXPECT_EQ(m.getField(F::Side), "1");
    EXPECT_EQ(m.getField(F::OrderQty), "1");
    EXPECT_EQ(m.getField(F::Price), "0.65");
    EXPECT_EQ(m.getField(F::OrdType), "2");
    EXPECT_EQ(m.getField(F::TimeInForce), "1");
    EXPECT_EQ(m.getField(F::ManualOrderIndicator), "N");
    EXPECT_FALSE(m.isSetField(F::ExecInst));

    // Exactly one Parties entry, PartyID first since it is the delimiter.
    ASSERT_EQ(m.groupCount(F::NoPartyIDs), 1u);
    FIX::Group party(F::NoPartyIDs, F::PartyID);
    m.getGroup(1, party);
    EXPECT_EQ(party.getField(F::PartyID), "my-subaccount");
    EXPECT_EQ(party.getField(F::PartyRole), "24");
}

TEST_F(OeMessages, AMarketOrderCarriesNoPrice) {
    auto order = limit_buy();
    order.type = OrderType::Market;
    order.price = 0;
    const auto m = round_trip(cpm::oms::fix::new_order_single(order, config()));
    EXPECT_EQ(m.getField(F::OrdType), "1");
    EXPECT_FALSE(m.isSetField(F::Price)) << "a market order carrying 44 is rejected";
}

TEST_F(OeMessages, OptionalOrderFieldsAppearOnlyWhenSet) {
    auto order = limit_buy();
    order.time_in_force = TimeInForce::Fok;
    order.post_only = PostOnly::Reject;
    order.max_execution_cost = 1250000;  // 1.25 USDC
    order.smp_id = 7;
    order.smp_strategy = SmpStrategy::CancelResting;
    const auto m = round_trip(cpm::oms::fix::new_order_single(order, config(true)));
    EXPECT_EQ(m.getField(F::TimeInForce), "4");
    EXPECT_EQ(m.getField(F::ExecInst), "A");
    EXPECT_EQ(m.getField(kMaxExecutionCost), "1.25");
    EXPECT_EQ(m.getField(kSelfMatchPreventionId), "7");
    EXPECT_EQ(m.getField(kSelfMatchPreventionStrategy), "O");
    EXPECT_EQ(m.getField(F::ManualOrderIndicator), "Y");

    auto slide = limit_buy();
    slide.post_only = PostOnly::Slide;
    EXPECT_EQ(round_trip(cpm::oms::fix::new_order_single(slide, config())).getField(F::ExecInst), "6");

    // Nothing optional leaks onto a plain order.
    const auto plain = round_trip(cpm::oms::fix::new_order_single(limit_buy(), config()));
    EXPECT_FALSE(plain.isSetField(kMaxExecutionCost));
    EXPECT_FALSE(plain.isSetField(kSelfMatchPreventionId));
    EXPECT_FALSE(plain.isSetField(kSelfMatchPreventionStrategy));
}

TEST_F(OeMessages, QuantitiesAndPricesAreWrittenAtTheirScales) {
    auto order = limit_buy();
    order.quantity = 250;  // 2.50 contracts
    order.price = 435;     // 0.435
    const auto m = round_trip(cpm::oms::fix::new_order_single(order, config()));
    EXPECT_EQ(m.getField(F::OrderQty), "2.5");
    EXPECT_EQ(m.getField(F::Price), "0.435");

    order.quantity = 1;  // the 0.01 minimum increment
    order.price = 1;     // 0.001
    const auto small = round_trip(cpm::oms::fix::new_order_single(order, config()));
    EXPECT_EQ(small.getField(F::OrderQty), "0.01");
    EXPECT_EQ(small.getField(F::Price), "0.001");
}

TEST_F(OeMessages, BuildsCancelsByEitherIdentifier) {
    CancelOrder by_clordid{"cancel-001", "order-001", "", "MKT-A", Side::Buy};
    const auto a = round_trip(cpm::oms::fix::order_cancel_request(by_clordid, config()));
    EXPECT_EQ(a.getHeader().getField(F::MsgType), "F");
    EXPECT_EQ(a.getField(F::ClOrdID), "cancel-001");
    EXPECT_EQ(a.getField(F::OrigClOrdID), "order-001");
    EXPECT_FALSE(a.isSetField(F::OrderID));
    EXPECT_EQ(a.getField(F::ManualOrderIndicator), "N");

    CancelOrder by_order_id{"cancel-002", "", "1001", "MKT-A", Side::Sell};
    const auto b = round_trip(cpm::oms::fix::order_cancel_request(by_order_id, config()));
    EXPECT_EQ(b.getField(F::OrderID), "1001");
    EXPECT_FALSE(b.isSetField(F::OrigClOrdID));
    EXPECT_EQ(b.getField(F::Side), "2");
}

TEST_F(OeMessages, BuildsEveryMassCancelScope) {
    MassCancel all;
    all.cl_ord_id = "m1";
    all.scope = MassCancel::Scope::All;
    const auto a = round_trip(cpm::oms::fix::order_mass_cancel_request(all, config()));
    EXPECT_EQ(a.getHeader().getField(F::MsgType), "q");
    EXPECT_EQ(a.getField(F::MassCancelRequestType), "7");
    EXPECT_FALSE(a.isSetField(F::Symbol));
    EXPECT_FALSE(a.isSetField(F::Side));
    EXPECT_EQ(a.groupCount(F::NoPartyIDs), 0u);

    MassCancel per_security;
    per_security.cl_ord_id = "m2";
    per_security.scope = MassCancel::Scope::Security;
    per_security.symbol = "MKT-A";
    per_security.side = Side::Buy;
    per_security.filter_by_subaccount = true;
    const auto b = round_trip(cpm::oms::fix::order_mass_cancel_request(per_security, config()));
    EXPECT_EQ(b.getField(F::MassCancelRequestType), "1");
    EXPECT_EQ(b.getField(F::Symbol), "MKT-A");
    EXPECT_EQ(b.getField(F::Side), "1");
    EXPECT_EQ(b.groupCount(F::NoPartyIDs), 1u);

    MassCancel session;
    session.cl_ord_id = "m3";
    session.scope = MassCancel::Scope::TradingSession;
    EXPECT_EQ(round_trip(cpm::oms::fix::order_mass_cancel_request(session, config()))
                  .getField(F::MassCancelRequestType), "6");
}

TEST_F(OeMessages, BuildsTheRecoveryPair) {
    const auto f1 = round_trip(cpm::oms::fix::last_exec_id_request());
    EXPECT_EQ(f1.getHeader().getField(F::MsgType), "F1");

    const auto open_ended = round_trip(cpm::oms::fix::event_resend_request("120"));
    EXPECT_EQ(open_ended.getHeader().getField(F::MsgType), "F3");
    EXPECT_EQ(open_ended.getField(kBeginExecId), "120");
    EXPECT_FALSE(open_ended.isSetField(kEndExecId)) << "omitted means everything since begin";

    const auto bounded = round_trip(cpm::oms::fix::event_resend_request("120", "150"));
    EXPECT_EQ(bounded.getField(kEndExecId), "150");
}

// --- inbound ---------------------------------------------------------------

// The spec's ExecutionReport - New example.
TEST_F(OeMessages, DecodesTheSpecsNewReport) {
    const auto batch = decode("8", {{11, "order-001"}, {37, "1001"}, {17, "150"}, {150, "0"}, {39, "0"},
                                    {55, "MKT-A"}, {54, "1"}, {38, "1.00"}, {44, "0.650"}, {40, "2"},
                                    {59, "1"}, {151, "1.00"}, {14, "0.00"}, {6, "0.000"},
                                    {60, "20240701-12:00:01.001200"}, {1028, "N"}});
    ASSERT_EQ(batch.errors.size(), 0u) << batch.errors.front().reason;
    ASSERT_EQ(batch.reports.size(), 1u);
    const auto& r = batch.reports.front();
    EXPECT_EQ(r.cl_ord_id, "order-001");
    EXPECT_EQ(r.order_id, "1001");
    EXPECT_EQ(r.exec_id, "150");
    EXPECT_EQ(r.exec_type, ExecType::New);
    EXPECT_EQ(r.status, OrdStatus::New);
    EXPECT_EQ(r.side, Side::Buy);
    EXPECT_EQ(r.leaves_quantity, 100);
    EXPECT_EQ(r.cumulative_quantity, 0);
    EXPECT_EQ(r.price, 650);
    EXPECT_EQ(r.order_quantity, 100);
    EXPECT_EQ(r.type, OrderType::Limit);
    EXPECT_EQ(r.time_in_force, TimeInForce::Gtc);
    EXPECT_FALSE(r.fill.has_value());
    EXPECT_FALSE(r.manual_order);
    EXPECT_FALSE(r.replayed);
}

// The spec's ExecutionReport - Trade Fill example, including a maker rebate.
TEST_F(OeMessages, DecodesTheSpecsFillWithItsFee) {
    const auto batch = decode("8", {{11, "order-001"}, {37, "1001"}, {17, "181"}, {150, "F"}, {39, "2"},
                                    {55, "MKT-A"}, {54, "1"}, {32, "1.00"}, {31, "0.650"},
                                    {151, "0.00"}, {14, "1.00"}, {6, "0.650"}, {880, "3391027"},
                                    {1057, "N"}, {136, "1"}, {137, "-0.325000"}, {138, "USDC"},
                                    {139, "4"}, {60, "20240701-12:01:00.004800"}, {1028, "N"}});
    ASSERT_EQ(batch.errors.size(), 0u) << batch.errors.front().reason;
    ASSERT_EQ(batch.reports.size(), 1u);
    const auto& r = batch.reports.front();
    EXPECT_EQ(r.exec_type, ExecType::Trade);
    EXPECT_EQ(r.status, OrdStatus::Filled);
    EXPECT_EQ(r.cumulative_quantity, 100);
    EXPECT_EQ(r.leaves_quantity, 0);
    EXPECT_EQ(r.average_price, 650);
    ASSERT_TRUE(r.fill.has_value());
    EXPECT_EQ(r.fill->quantity, 100);
    EXPECT_EQ(r.fill->price, 650);
    EXPECT_EQ(r.fill->trade_match_id, "3391027");
    EXPECT_FALSE(r.fill->aggressor) << "1057=N is the maker side";
    ASSERT_TRUE(r.fill->fee.has_value());
    EXPECT_EQ(*r.fill->fee, -325000) << "negative is a maker rebate";
    EXPECT_EQ(cpm::format_amount(*r.fill->fee), "-0.325");
}

// The two terminal paths that are easy to confuse: an IOC remainder and an
// unfillable FOK both Expire with no reject reason, while a cap breach Rejects.
TEST_F(OeMessages, DistinguishesExpiredFromRejected) {
    const auto expired = decode("8", {{11, "o1"}, {37, "1"}, {17, "2"}, {150, "C"}, {39, "C"},
                                      {55, "MKT-A"}, {54, "1"}, {151, "0.00"}, {14, "0.50"},
                                      {58, "TIME_IN_FORCE"}, {60, "20240701-12:00:00.000000"},
                                      {1028, "N"}});
    ASSERT_EQ(expired.reports.size(), 1u);
    EXPECT_EQ(expired.reports.front().exec_type, ExecType::Expired);
    EXPECT_FALSE(expired.reports.front().reject_reason.has_value()) << "no 103 on an expiry";
    EXPECT_EQ(expired.reports.front().cumulative_quantity, 50);

    const auto rejected = decode("8", {{11, "o2"}, {37, "0"}, {17, "3"}, {150, "8"}, {39, "8"},
                                       {55, "MKT-A"}, {54, "1"}, {151, "0.00"}, {14, "0.00"},
                                       {103, "3"},
                                       {58, "MAX_EXECUTION_COST_EXCEEDED - insufficient liquidity"},
                                       {60, "20240701-12:00:00.000000"}, {1028, "N"}});
    ASSERT_EQ(rejected.reports.size(), 1u);
    EXPECT_EQ(rejected.reports.front().exec_type, ExecType::Rejected);
    EXPECT_EQ(rejected.reports.front().reject_reason, 3);
    EXPECT_NE(rejected.reports.front().text.find("MAX_EXECUTION_COST_EXCEEDED"), std::string::npos);
}

TEST_F(OeMessages, DecodesUnsolicitedCancelReasons) {
    const auto reason_for = [this](const char* code) {
        const auto batch = decode("8", {{11, "o1"}, {37, "1001"}, {17, "4"}, {150, "4"}, {39, "4"},
                                        {55, "MKT-A"}, {54, "1"}, {151, "0.00"}, {14, "0.00"},
                                        {378, code}, {60, "20240701-12:00:00.000000"}, {1028, "N"}});
        return batch.reports.size() == 1u ? batch.reports.front().cancel_reason : CancelReason::Unknown;
    };
    EXPECT_EQ(reason_for("1"), CancelReason::CancelOnDisconnect);
    EXPECT_EQ(reason_for("2"), CancelReason::InstrumentLifecycle);
    EXPECT_EQ(reason_for("7"), CancelReason::SelfMatchPrevention);
    EXPECT_EQ(reason_for("105"), CancelReason::AdminCancel);

    // A mass cancel and a time-in-force expiry carry no 378 at all.
    const auto batch = decode("8", {{11, "o1"}, {37, "1001"}, {17, "5"}, {150, "4"}, {39, "4"},
                                    {55, "MKT-A"}, {54, "1"}, {151, "0.00"}, {14, "0.00"},
                                    {58, "USER_MASS_CANCEL"}, {60, "20240701-12:00:00.000000"},
                                    {1028, "N"}});
    ASSERT_EQ(batch.reports.size(), 1u);
    EXPECT_EQ(batch.reports.front().cancel_reason, CancelReason::None);
}

// A replayed event is distinguished by PossResend(97), not by its timestamp,
// which is the original event's and is preserved exactly.
TEST_F(OeMessages, DecodesReplayedEvents) {
    Fields body{{97, "Y"}};  // header field, so it goes first in our builder
    const auto batch = decode("8", {{11, "o1"}, {37, "1001"}, {17, "181"}, {150, "0"}, {39, "0"},
                                    {55, "MKT-A"}, {54, "1"}, {151, "1.00"}, {14, "0.00"},
                                    {60, "20240701-12:01:00.004800"}, {1028, "N"}});
    EXPECT_FALSE(batch.reports.front().replayed);

    Fields fields{{35, "8"}, {49, "CPMOE"}, {56, "test-key"}, {34, "40"}, {97, "Y"},
                  {52, "20240701-14:00:00.000000"}, {11, "o1"}, {37, "1001"}, {17, "181"},
                  {150, "0"}, {39, "0"}, {55, "MKT-A"}, {54, "1"}, {151, "1.00"}, {14, "0.00"},
                  {60, "20240701-12:01:00.004800"}, {1028, "N"}};
    FIX::Message message(build_fix(fields), *transport_, *app_, true);
    const auto replayed = cpm::oms::fix::decode_message(message, "test-key");
    ASSERT_EQ(replayed.reports.size(), 1u);
    EXPECT_TRUE(replayed.reports.front().replayed);
}

TEST_F(OeMessages, DecodesCancelRejects) {
    const auto batch = decode("9", {{11, "c1"}, {41, "o1"}, {37, "0"}, {39, "U"}, {102, "1"},
                                    {434, "1"}, {58, "UNKNOWN_ORDER"}});
    ASSERT_EQ(batch.errors.size(), 0u) << batch.errors.front().reason;
    ASSERT_EQ(batch.cancel_rejects.size(), 1u);
    const auto& reject = batch.cancel_rejects.front();
    EXPECT_EQ(reject.cl_ord_id, "c1");
    EXPECT_EQ(reject.orig_cl_ord_id, "o1");
    EXPECT_EQ(reject.order_id, "0") << "the sentinel for an unknown order";
    EXPECT_EQ(reject.reason, 1);
    EXPECT_EQ(reject.text, "UNKNOWN_ORDER");
}

TEST_F(OeMessages, DecodesMassCancelReports) {
    const auto accepted = decode("r", {{11, "m1"}, {1369, "900"}, {530, "7"}, {531, "7"}, {533, "4"},
                                       {60, "20240701-12:00:07.000000"}});
    ASSERT_EQ(accepted.mass_cancel_reports.size(), 1u);
    EXPECT_TRUE(accepted.mass_cancel_reports.front().accepted);
    EXPECT_EQ(accepted.mass_cancel_reports.front().affected_orders, 4);
    EXPECT_EQ(accepted.mass_cancel_reports.front().report_id, "900");

    const auto rejected = decode("r", {{11, "m2"}, {1369, "0"}, {530, "1"}, {531, "0"}, {532, "1"},
                                       {533, "0"}, {60, "20240701-12:00:08.000000"},
                                       {58, "unknown security"}});
    ASSERT_EQ(rejected.mass_cancel_reports.size(), 1u);
    EXPECT_FALSE(rejected.mass_cancel_reports.front().accepted);
    EXPECT_EQ(rejected.mass_cancel_reports.front().reject_reason, 1);
    EXPECT_EQ(rejected.mass_cancel_reports.front().affected_orders, 0);
}

TEST_F(OeMessages, DecodesSettlementReportsAndTheirPayoutIdentity) {
    const auto batch = decode("UMS", {{20105, "1000"}, {55, "MKT-A"}, {453, "2"}, {448, "FCM1"},
                                      {452, "4"}, {448, "FIRM1"}, {452, "1"}, {21052, "Y"},
                                      {730, "1.000"}, {21053, "N"}, {21054, "3.00"}, {21055, "0.00"},
                                      {21056, "3.000000"}, {60, "20240701-13:00:00.000000"}});
    ASSERT_EQ(batch.errors.size(), 0u) << batch.errors.front().reason;
    ASSERT_EQ(batch.settlements.size(), 1u);
    const auto& s = batch.settlements.front();
    EXPECT_EQ(s.report_id, "1000");
    EXPECT_EQ(s.clearing_firm, "FCM1");
    EXPECT_EQ(s.executing_firm, "FIRM1");
    EXPECT_EQ(s.outcome, cpm::oms::SettlementReport::Outcome::Yes);
    EXPECT_EQ(s.settlement_price, 1000);
    EXPECT_TRUE(s.net_aggregation);
    EXPECT_EQ(s.long_quantity, 300);
    EXPECT_EQ(s.short_quantity, 0);
    EXPECT_EQ(s.payout, 3000000);
    // payout = price*long + (1-price)*short, in the scales we decoded them to.
    const auto expected = (s.settlement_price * s.long_quantity +
                           (cpm::kPriceOne - s.settlement_price) * s.short_quantity) * 10;
    EXPECT_EQ(s.payout, expected);

    // Gross aggregation can show both legs at once.
    const auto gross = decode("UMS", {{20105, "1001"}, {55, "MKT-A"}, {453, "2"}, {448, "FCM1"},
                                      {452, "4"}, {448, "FIRM1"}, {452, "1"}, {21052, "S"},
                                      {730, "0.400"}, {21053, "G"}, {21054, "2.00"}, {21055, "1.00"},
                                      {21056, "1.400000"}, {60, "20240701-13:00:00.000000"}});
    ASSERT_EQ(gross.settlements.size(), 1u);
    EXPECT_FALSE(gross.settlements.front().net_aggregation);
    EXPECT_EQ(gross.settlements.front().outcome, cpm::oms::SettlementReport::Outcome::Scalar);
    EXPECT_EQ(gross.settlements.front().payout, 1400000);
}

TEST_F(OeMessages, DecodesTheRecoveryReplies) {
    const auto f2 = decode("F2", {{45, "7"}, {17, "150"}});
    ASSERT_EQ(f2.last_exec_ids.size(), 1u);
    EXPECT_EQ(f2.last_exec_ids.front().exec_id, "150");

    const auto empty = decode("F2", {{45, "7"}, {17, "0"}});
    EXPECT_EQ(empty.last_exec_ids.front().exec_id, "0") << "nothing in the retention window";

    const auto f4 = decode("F4", {{45, "9"}, {22005, "2"}});
    ASSERT_EQ(f4.resend_completes.size(), 1u);
    EXPECT_EQ(f4.resend_completes.front().event_count, 2);

    const auto f5 = decode("F5", {{45, "9"}, {22006, "3"}, {58, "RESEND_ALREADY_IN_PROGRESS"}});
    ASSERT_EQ(f5.resend_rejects.size(), 1u);
    EXPECT_EQ(f5.resend_rejects.front().reason, 3);
}

// The synchronous rejection path: no ExecutionReport follows, so this is
// terminal for the ClOrdID it names.
TEST_F(OeMessages, DecodesBusinessRejects) {
    const auto batch = decode("j", {{45, "4"}, {372, "D"}, {379, "order-009"}, {380, "0"},
                                    {58, "INVALID_FIELD_VALUE: Price(44) must be omitted"}});
    ASSERT_EQ(batch.business_rejects.size(), 1u);
    EXPECT_EQ(batch.business_rejects.front().ref_msg_type, "D");
    EXPECT_EQ(batch.business_rejects.front().business_reject_ref_id, "order-009");
    EXPECT_EQ(batch.business_rejects.front().reason, 0);
}

TEST_F(OeMessages, ReportsWhatItCannotDecode) {
    // Missing LeavesQty: the state machine cannot work without it.
    auto batch = decode("8", {{11, "o1"}, {37, "1"}, {17, "2"}, {150, "0"}, {39, "0"}, {55, "MKT-A"},
                              {54, "1"}, {14, "0.00"}, {60, "20240701-12:00:00.000000"}, {1028, "N"}});
    EXPECT_TRUE(batch.reports.empty());
    ASSERT_EQ(batch.errors.size(), 1u);
    EXPECT_NE(batch.errors.front().reason.find("151"), std::string::npos) << batch.errors.front().reason;
    EXPECT_EQ(batch.errors.front().cl_ord_id, "o1");

    // A malformed TransactTime.
    batch = decode("8", {{11, "o1"}, {37, "1"}, {17, "2"}, {150, "0"}, {39, "0"}, {55, "MKT-A"},
                         {54, "1"}, {151, "1.00"}, {14, "0.00"}, {60, "2024-07-01 12:00:00"},
                         {1028, "N"}});
    EXPECT_TRUE(batch.reports.empty());
    EXPECT_NE(batch.errors.front().reason.find("TransactTime"), std::string::npos);

    // A fee that is not a decimal.
    batch = decode("8", {{11, "o1"}, {37, "1"}, {17, "2"}, {150, "F"}, {39, "2"}, {55, "MKT-A"},
                         {54, "1"}, {32, "1.00"}, {31, "0.650"}, {151, "0.00"}, {14, "1.00"},
                         {136, "1"}, {137, "free"}, {138, "USDC"}, {139, "4"},
                         {60, "20240701-12:00:00.000000"}, {1028, "N"}});
    EXPECT_NE(batch.errors.front().reason.find("MiscFeeAmt(137)=free"), std::string::npos);

    // The raw text is kept for the log, with the key redacted.
    EXPECT_EQ(batch.errors.front().raw.find("test-key"), std::string::npos);
}

// The gateway truncates excess precision, so the decoder must too (OE-Q8).
TEST_F(OeMessages, ExcessPrecisionIsTruncatedNotRefused) {
    const auto batch = decode("8", {{11, "o1"}, {37, "1"}, {17, "2"}, {150, "0"}, {39, "0"},
                                    {55, "MKT-A"}, {54, "1"}, {44, "0.6505"}, {151, "1.004"},
                                    {14, "0.00"}, {60, "20240701-12:00:00.000000"}, {1028, "N"}});
    ASSERT_EQ(batch.errors.size(), 0u) << batch.errors.front().reason;
    ASSERT_EQ(batch.reports.size(), 1u);
    EXPECT_EQ(batch.reports.front().price, 650);
    EXPECT_EQ(batch.reports.front().leaves_quantity, 100);
}

TEST_F(OeMessages, AnUnhandledMessageTypeDecodesToNothing) {
    EXPECT_TRUE(decode("0", {}).empty());
}

}  // namespace
