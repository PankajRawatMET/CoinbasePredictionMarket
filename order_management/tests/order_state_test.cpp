// The order state machine: one record per order, built from the reports, with
// positions derived from fills because the venue publishes none until
// settlement (OE-Q11).
#include "cpm/oms/order_state.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

using cpm::oms::CancelOrder;
using cpm::oms::CancelReject;
using cpm::oms::ExecType;
using cpm::oms::ExecutionReport;
using cpm::oms::Fill;
using cpm::oms::NewOrder;
using cpm::oms::OrderAnomaly;
using cpm::oms::OrderState;
using cpm::oms::OrderType;
using cpm::oms::OrdStatus;
using cpm::oms::Side;
using cpm::oms::TimeInForce;

const std::string kSym = "SIMOE-A";

cpm::oms::Clock::time_point at(int secs) {
    return cpm::oms::Clock::time_point{} + std::chrono::seconds(secs);
}

NewOrder sent(const std::string& id, Side side = Side::Buy, cpm::Qty quantity = 100,
              cpm::Price price = 400) {
    NewOrder o;
    o.cl_ord_id = id;
    o.symbol = kSym;
    o.side = side;
    o.quantity = quantity;
    o.price = price;
    o.type = OrderType::Limit;
    o.time_in_force = TimeInForce::Gtc;
    return o;
}

ExecutionReport report(const std::string& cl_ord_id, ExecType type, OrdStatus status,
                       cpm::Qty leaves, cpm::Qty cumulative, const std::string& exec_id) {
    ExecutionReport r;
    r.cl_ord_id = cl_ord_id;
    r.order_id = "1001";
    r.exec_id = exec_id;
    r.exec_type = type;
    r.status = status;
    r.symbol = kSym;
    r.side = Side::Buy;
    r.order_quantity = 100;
    r.price = 400;
    r.leaves_quantity = leaves;
    r.cumulative_quantity = cumulative;
    r.transact_time = at(2);
    return r;
}

struct Rig {
    Rig() {
        state.on_anomaly([this](const OrderAnomaly& a) { anomalies.push_back(a); });
    }
    std::string first_anomaly() const {
        return anomalies.empty() ? std::string() : std::string(to_string(anomalies.front().kind)) + ": " +
                                                        anomalies.front().detail;
    }

    OrderState state;
    std::vector<OrderAnomaly> anomalies;
};

TEST(OrderStateTest, TracksAnOrderFromSendToResting) {
    Rig rig;
    rig.state.record_sent(sent("o1"));
    // Known before any report, so a reject that carries none still lands.
    auto record = rig.state.order("o1");
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->order_quantity, 100);
    EXPECT_EQ(record->leaves_quantity, 100);
    EXPECT_FALSE(record->terminal);

    rig.state.apply(report("o1", ExecType::New, OrdStatus::New, 100, 0, "10"));
    record = rig.state.order("o1");
    EXPECT_EQ(record->status, OrdStatus::New);
    EXPECT_EQ(record->order_id, "1001");
    EXPECT_EQ(record->last_exec_id, "10");
    EXPECT_FALSE(record->terminal);
    EXPECT_EQ(rig.state.open_orders().size(), 1u);
    EXPECT_EQ(rig.state.last_exec_id(), "10");
    EXPECT_EQ(rig.state.anomalies(), 0u) << rig.first_anomaly();
}

TEST(OrderStateTest, AccumulatesFillsIntoTheOrderAndThePosition) {
    Rig rig;
    rig.state.record_sent(sent("o1"));
    rig.state.apply(report("o1", ExecType::New, OrdStatus::New, 100, 0, "10"));

    auto first = report("o1", ExecType::Trade, OrdStatus::PartiallyFilled, 60, 40, "11");
    first.fill = Fill{40, 500, "t1", true, 2000};  // 0.4 contracts at 0.500, fee 0.002
    rig.state.apply(first);

    auto second = report("o1", ExecType::Trade, OrdStatus::Filled, 0, 100, "12");
    second.fill = Fill{60, 510, "t2", true, 3060};
    rig.state.apply(second);

    const auto record = rig.state.order("o1");
    EXPECT_EQ(record->status, OrdStatus::Filled);
    EXPECT_TRUE(record->terminal);
    EXPECT_EQ(record->fills, 2u);
    EXPECT_EQ(record->cumulative_quantity, 100);
    EXPECT_EQ(record->leaves_quantity, 0);
    // VWAP of 40 at 0.500 and 60 at 0.510.
    EXPECT_EQ(record->average_price, (40 * 500 + 60 * 510) / 100);
    EXPECT_EQ(record->fees, 5060);
    EXPECT_TRUE(rig.state.open_orders().empty());

    const auto position = rig.state.position(kSym);
    ASSERT_TRUE(position.has_value());
    EXPECT_EQ(position->net, 100) << "bought 1.00 contract net";
    EXPECT_EQ(position->bought, 100);
    EXPECT_EQ(position->sold, 0);
    EXPECT_EQ(position->fills, 2u);
    // Paid 40x0.500 + 60x0.510 plus fees.
    EXPECT_EQ(position->cash, 40 * 500 * 10 + 60 * 510 * 10 + 5060);
    EXPECT_EQ(position->bought_value, 40 * 500 * 10 + 60 * 510 * 10) << "without the fees";
    EXPECT_EQ(position->sold_value, 0);
    EXPECT_EQ(rig.state.anomalies(), 0u) << rig.first_anomaly();
}

TEST(OrderStateTest, RestoredFillsSettleAfterRestartAndDeduplicateFixOverlap) {
    Rig rig;
    auto first = report("", ExecType::Trade, OrdStatus::Unknown, 0, 0, "");
    first.side = Side::Sell;
    first.fill = Fill{100, 600, "1", false, -1000};
    auto second = first;
    second.fill = Fill{100, 550, "2", true, 2000};

    EXPECT_FALSE(rig.state.recover_fills(kSym, {first, second}, -100).empty());
    EXPECT_FALSE(rig.state.position(kSym).has_value());
    ASSERT_EQ(rig.state.recover_fills(kSym, {first, second}, -200), "");
    ASSERT_EQ(rig.state.fills().size(), 2u);
    EXPECT_TRUE(rig.state.last_exec_id().empty());
    EXPECT_TRUE(rig.state.orders().empty());

    second.exec_id = "12";
    second.replayed = true;
    rig.state.apply(second);
    EXPECT_EQ(rig.state.last_exec_id(), "12");

    cpm::oms::SettlementReport settlement;
    settlement.symbol = kSym;
    settlement.outcome = cpm::oms::SettlementReport::Outcome::No;
    settlement.short_quantity = 200;
    settlement.payout = 2000000;
    rig.state.apply(settlement);
    const auto position = rig.state.position(kSym);
    ASSERT_TRUE(position);
    EXPECT_EQ(position->net, -200);
    EXPECT_EQ(position->fills, 2u);
    EXPECT_EQ(position->cash, -1149000);
    EXPECT_EQ(position->fees, 1000);
    EXPECT_EQ(position->settlement_payout, 2000000);
    EXPECT_TRUE(rig.anomalies.empty()) << rig.first_anomaly();
}

TEST(OrderStateTest, SellsSubtractFromThePosition) {
    Rig rig;
    rig.state.record_sent(sent("buy", Side::Buy));
    auto bought = report("buy", ExecType::Trade, OrdStatus::Filled, 0, 100, "10");
    bought.fill = Fill{100, 400, "t1", true, 0};
    rig.state.apply(bought);

    rig.state.record_sent(sent("sell", Side::Sell, 150, 600));
    auto sold = report("sell", ExecType::Trade, OrdStatus::Filled, 0, 150, "11");
    sold.side = Side::Sell;
    sold.order_quantity = 150;
    sold.fill = Fill{150, 600, "t2", false, -1000};  // a maker rebate
    rig.state.apply(sold);

    const auto position = rig.state.position(kSym);
    EXPECT_EQ(position->net, -50) << "short 0.50 YES contracts";
    EXPECT_EQ(position->bought, 100);
    EXPECT_EQ(position->sold, 150);
    EXPECT_EQ(position->fees, -1000) << "the rebate is received";
    // Paid 1.00 at 0.400, received 1.50 at 0.600, less the rebate.
    EXPECT_EQ(position->cash, 100 * 400 * 10 - 150 * 600 * 10 - 1000);
    // And the two halves of it, kept apart and without the rebate in them,
    // which is what an average cost needs (GUI-Q15).
    EXPECT_EQ(position->bought_value, 100 * 400 * 10);
    EXPECT_EQ(position->sold_value, 150 * 600 * 10);
    EXPECT_EQ(position->cash, position->bought_value - position->sold_value + position->fees)
        << "the invariant pnl_of() leans on";
}

// A cancel's report names the CANCEL in ClOrdID, with the order in
// OrigClOrdID (OE-O2), so resolution has to look there first.
TEST(OrderStateTest, ACancelsReportIsAttributedToTheOrderNotTheCancel) {
    Rig rig;
    rig.state.record_sent(sent("o1"));
    rig.state.apply(report("o1", ExecType::New, OrdStatus::New, 100, 0, "10"));
    rig.state.record_cancel_sent(CancelOrder{"c1", "o1", "", kSym, Side::Buy});
    EXPECT_TRUE(rig.state.order("o1")->cancel_pending);

    auto canceled = report("c1", ExecType::Canceled, OrdStatus::Canceled, 0, 0, "11");
    canceled.orig_cl_ord_id = "o1";
    canceled.text = "USER_REQUEST";
    rig.state.apply(canceled);

    const auto record = rig.state.order("o1");
    EXPECT_EQ(record->status, OrdStatus::Canceled);
    EXPECT_TRUE(record->terminal);
    EXPECT_FALSE(record->cancel_pending);
    EXPECT_EQ(record->text, "USER_REQUEST");
    // No separate record was invented for the cancel's own id.
    EXPECT_FALSE(rig.state.order("c1").has_value());
    EXPECT_EQ(rig.state.anomalies(), 0u) << rig.first_anomaly();
}

TEST(OrderStateTest, ACancelRejectLeavesTheOrderAlone) {
    Rig rig;
    rig.state.record_sent(sent("o1"));
    rig.state.apply(report("o1", ExecType::New, OrdStatus::New, 100, 0, "10"));
    rig.state.record_cancel_sent(CancelOrder{"c1", "o1", "", kSym, Side::Buy});

    CancelReject reject;
    reject.cl_ord_id = "c1";
    reject.orig_cl_ord_id = "o1";
    reject.order_id = "1001";
    reject.reason = 0;  // too late to cancel
    reject.text = "ORDER_ALREADY_FILLED";
    rig.state.apply(reject);

    const auto record = rig.state.order("o1");
    EXPECT_EQ(record->status, OrdStatus::New) << "the order is whatever it was";
    EXPECT_FALSE(record->cancel_pending);
    EXPECT_FALSE(record->terminal);
    EXPECT_EQ(record->text, "ORDER_ALREADY_FILLED");
}

// The synchronous path carries no ExecutionReport at all, so it is the order's
// terminal state.
TEST(OrderStateTest, ABusinessRejectIsTerminalForTheOrder) {
    Rig rig;
    rig.state.record_sent(sent("o1"));
    cpm::oms::BusinessReject reject;
    reject.ref_msg_type = "D";
    reject.business_reject_ref_id = "o1";
    reject.reason = 0;
    reject.text = "INVALID_FIELD_VALUE: Price(44) must be omitted";
    rig.state.apply(reject);

    const auto record = rig.state.order("o1");
    EXPECT_EQ(record->status, OrdStatus::Rejected);
    EXPECT_TRUE(record->terminal);
    EXPECT_EQ(record->leaves_quantity, 0);
    EXPECT_NE(record->text.find("INVALID_FIELD_VALUE"), std::string::npos);
    EXPECT_TRUE(rig.state.open_orders().empty());
}

// Event Resend delivers the same events again; an ExecID already applied must
// change nothing, or a replayed fill would double the position.
TEST(OrderStateTest, ReplayedReportsAreIgnored) {
    Rig rig;
    rig.state.record_sent(sent("o1"));
    auto fill = report("o1", ExecType::Trade, OrdStatus::Filled, 0, 100, "11");
    fill.fill = Fill{100, 500, "t1", true, 5000};
    rig.state.apply(fill);

    auto replay = fill;
    replay.replayed = true;
    rig.state.apply(replay);
    rig.state.apply(replay);

    EXPECT_EQ(rig.state.duplicate_reports(), 2u);
    EXPECT_EQ(rig.state.order("o1")->fills, 1u);
    EXPECT_EQ(rig.state.position(kSym)->net, 100) << "the position did not double";
    EXPECT_EQ(rig.state.anomalies(), 0u) << rig.first_anomaly();
}

TEST(OrderStateTest, TheBookmarkIsTheNewestExecId) {
    Rig rig;
    rig.state.record_sent(sent("o1"));
    rig.state.apply(report("o1", ExecType::New, OrdStatus::New, 100, 0, "480837776594436096"));
    rig.state.apply(report("o1", ExecType::Canceled, OrdStatus::Canceled, 0, 0, "480837843678134272"));
    EXPECT_EQ(rig.state.last_exec_id(), "480837843678134272");

    // An older one arriving late, as a replay would, does not move it back.
    rig.state.apply(report("o1", ExecType::New, OrdStatus::New, 100, 0, "480837776594436000"));
    EXPECT_EQ(rig.state.last_exec_id(), "480837843678134272");
}

TEST(OrderStateTest, ReportsForAnUnknownOrderAreKeptAndFlagged) {
    Rig rig;
    // An unsolicited cancel for an order from a previous run.
    auto canceled = report("from-before", ExecType::Canceled, OrdStatus::Canceled, 0, 0, "10");
    canceled.cancel_reason = cpm::oms::CancelReason::CancelOnDisconnect;
    canceled.text = "CLIENT_DISCONNECT";
    rig.state.apply(canceled);

    ASSERT_EQ(rig.anomalies.size(), 1u);
    EXPECT_EQ(rig.anomalies.front().kind, OrderAnomaly::Kind::UnknownOrder);
    // Kept anyway, so the position and the audit trail are still right.
    const auto record = rig.state.order("from-before");
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->status, OrdStatus::Canceled);
    EXPECT_EQ(record->cancel_reason, cpm::oms::CancelReason::CancelOnDisconnect);
}

TEST(OrderStateTest, FlagsReportsThatContradictTheOrder) {
    Rig rig;
    rig.state.record_sent(sent("o1"));
    rig.state.apply(report("o1", ExecType::Trade, OrdStatus::Filled, 0, 100, "10"));
    EXPECT_EQ(rig.anomalies.size(), 1u) << "a trade with no fill fields";
    EXPECT_EQ(rig.anomalies.front().kind, OrderAnomaly::Kind::MissingFill);

    // A further report after a terminal state.
    rig.anomalies.clear();
    rig.state.apply(report("o1", ExecType::New, OrdStatus::New, 100, 0, "11"));
    ASSERT_GE(rig.anomalies.size(), 1u);
    const auto kinds = rig.anomalies;
    EXPECT_TRUE(std::any_of(kinds.begin(), kinds.end(), [](const auto& a) {
        return a.kind == OrderAnomaly::Kind::AfterTerminal;
    }));
    EXPECT_TRUE(std::any_of(kinds.begin(), kinds.end(), [](const auto& a) {
        return a.kind == OrderAnomaly::Kind::StatusRegression;
    }));
}

TEST(OrderStateTest, FlagsQuantitiesThatDoNotAddUp) {
    Rig rig;
    rig.state.record_sent(sent("o1"));
    // 30 + 30 is not the 100 that was sent, and this is not a terminal report.
    rig.state.apply(report("o1", ExecType::Trade, OrdStatus::PartiallyFilled, 30, 30, "10"));
    ASSERT_GE(rig.anomalies.size(), 1u);
    EXPECT_TRUE(std::any_of(rig.anomalies.begin(), rig.anomalies.end(), [](const auto& a) {
        return a.kind == OrderAnomaly::Kind::QuantityMismatch;
    })) << rig.first_anomaly();

    // A terminal report may give up the remainder without complaint.
    rig.anomalies.clear();
    rig.state.record_sent(sent("o2"));
    rig.state.apply(report("o2", ExecType::Expired, OrdStatus::Expired, 0, 40, "11"));
    EXPECT_TRUE(rig.anomalies.empty()) << rig.first_anomaly();
}

TEST(OrderStateTest, FlagsAChangedOrderId) {
    Rig rig;
    rig.state.record_sent(sent("o1"));
    rig.state.apply(report("o1", ExecType::New, OrdStatus::New, 100, 0, "10"));
    auto second = report("o1", ExecType::Trade, OrdStatus::Filled, 0, 100, "11");
    second.order_id = "2002";
    second.fill = Fill{100, 400, "t1", true, 0};
    rig.state.apply(second);
    EXPECT_TRUE(std::any_of(rig.anomalies.begin(), rig.anomalies.end(), [](const auto& a) {
        return a.kind == OrderAnomaly::Kind::OrderIdChanged;
    })) << rig.first_anomaly();
}

TEST(OrderStateTest, SeveralInstrumentsKeepSeparatePositions) {
    Rig rig;
    rig.state.record_sent(sent("a"));
    auto first = report("a", ExecType::Trade, OrdStatus::Filled, 0, 100, "10");
    first.fill = Fill{100, 400, "t1", true, 0};
    rig.state.apply(first);

    auto other = sent("b");
    other.symbol = "SIMOE-B";
    rig.state.record_sent(other);
    auto second = report("b", ExecType::Trade, OrdStatus::Filled, 0, 100, "11");
    second.symbol = "SIMOE-B";
    second.fill = Fill{100, 600, "t2", true, 0};
    rig.state.apply(second);

    EXPECT_EQ(rig.state.positions().size(), 2u);
    EXPECT_EQ(rig.state.position(kSym)->net, 100);
    EXPECT_EQ(rig.state.position("SIMOE-B")->net, 100);
    EXPECT_EQ(rig.state.orders().size(), 2u);
}

}  // namespace
