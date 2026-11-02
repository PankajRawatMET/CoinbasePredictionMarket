// The record files (docs/design-logs.md): one row per fill, one per anything
// else, and the latency columns - how long the venue took to add an order, and
// how long that order then rested before it was hit.
#include "cpm/oms/record_log.hpp"

#include <gtest/gtest.h>

#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using cpm::oms::AppliedReport;
using cpm::oms::CancelOrder;
using cpm::oms::CancelReject;
using cpm::oms::ExecType;
using cpm::oms::ExecutionReport;
using cpm::oms::Fill;
using cpm::oms::NewOrder;
using cpm::oms::OrderState;
using cpm::oms::OrderType;
using cpm::oms::OrdStatus;
using cpm::oms::RecordLog;
using cpm::oms::Side;
using cpm::oms::TimeInForce;

const std::string kSym = "SIMOE-A";

class TempDir {
public:
    explicit TempDir(const std::string& name)
        : path_(std::filesystem::temp_directory_path() / (name + std::to_string(::getpid()))) {
        std::filesystem::remove_all(path_);
    }
    ~TempDir() { std::filesystem::remove_all(path_); }
    std::string operator()() const { return path_.string(); }

private:
    std::filesystem::path path_;
};

std::vector<std::string> lines_of(const std::string& path) {
    std::ifstream in(path);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) lines.push_back(line);
    return lines;
}

// The value of one named column of one row, by the header above it.
std::string column(const std::vector<std::string>& lines, std::size_t row,
                   const std::string& name) {
    if (lines.size() <= row) return "<no such row>";
    // Enough of a CSV reader to read what this writer writes: quoted fields
    // hold the commas the naive split would have torn a row apart on.
    const auto split = [](const std::string& line) {
        std::vector<std::string> fields;
        std::string field;
        bool quoted = false;
        for (std::size_t i = 0; i < line.size(); ++i) {
            const char c = line[i];
            if (quoted && c == '"' && i + 1 < line.size() && line[i + 1] == '"') {
                field.push_back('"');
                ++i;
            } else if (c == '"') {
                quoted = !quoted;
            } else if (c == ',' && !quoted) {
                fields.push_back(field);
                field.clear();
            } else {
                field.push_back(c);
            }
        }
        fields.push_back(field);
        return fields;
    };
    const auto header = split(lines.front());
    const auto at = std::find(header.begin(), header.end(), name);
    if (at == header.end()) return "<no such column>";
    const auto fields = split(lines[row]);
    const auto index = static_cast<std::size_t>(at - header.begin());
    return index < fields.size() ? fields[index] : "";
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
    r.transact_time = cpm::oms::Clock::time_point{} + 2s;
    return r;
}

// An OrderState with the applied reports collected, which is what the record
// files are written from.
struct Rig {
    Rig() {
        state.on_applied([this](const AppliedReport& a) { applied.push_back(a); });
    }
    OrderState state;
    std::vector<AppliedReport> applied;
};

// --- the latency itself ---------------------------------------------------

TEST(LatencyTest, MeasuresTheOrderBeingAddedFromTheMomentItWentOut) {
    Rig rig;
    rig.state.record_sent(sent("o1"));
    std::this_thread::sleep_for(2ms);
    rig.state.apply(report("o1", ExecType::New, OrdStatus::New, 100, 0, "10"));

    ASSERT_EQ(rig.applied.size(), 1u);
    const auto& added = rig.applied.front();
    ASSERT_TRUE(added.latency.has_value());
    // Send to added, from this process's own monotonic clock: at least the
    // 2 ms that actually passed, and not a wall-clock subtraction that an
    // NTP step could have made negative.
    EXPECT_GE(added.latency->count(), 2000);
    EXPECT_LT(added.latency->count(), 2'000'000) << "a sane upper bound, not a timing assertion";
    EXPECT_EQ(rig.state.order("o1")->ack_latency, added.latency);
    EXPECT_FALSE(added.rested.has_value()) << "an add is not a fill";
}

TEST(LatencyTest, KeepsTheSpeedBumpApartFromTheAdd) {
    Rig rig;
    rig.state.record_sent(sent("o1"));
    // An instrument with a speed bump answers twice: taken, then matchable.
    std::this_thread::sleep_for(1ms);
    rig.state.apply(report("o1", ExecType::PendingNew, OrdStatus::PendingNew, 100, 0, "9"));
    std::this_thread::sleep_for(3ms);
    rig.state.apply(report("o1", ExecType::New, OrdStatus::New, 100, 0, "10"));

    const auto record = rig.state.order("o1");
    ASSERT_TRUE(record->pending_latency.has_value());
    ASSERT_TRUE(record->ack_latency.has_value());
    EXPECT_GT(record->ack_latency->count(), record->pending_latency->count())
        << "the order rests after the bump, not before it";
    EXPECT_GE((*record->ack_latency - *record->pending_latency).count(), 3000)
        << "the gap between the two is the speed bump, and is worth seeing";
}

TEST(LatencyTest, MeasuresACancelFromTheCancelAndNotFromTheOrder) {
    Rig rig;
    rig.state.record_sent(sent("o1"));
    rig.state.apply(report("o1", ExecType::New, OrdStatus::New, 100, 0, "10"));
    std::this_thread::sleep_for(5ms);

    CancelOrder cancel;
    cancel.cl_ord_id = "c1";
    cancel.orig_cl_ord_id = "o1";
    cancel.symbol = kSym;
    cancel.side = Side::Buy;
    rig.state.record_cancel_sent(cancel);

    auto confirmed = report("c1", ExecType::Canceled, OrdStatus::Canceled, 0, 0, "11");
    confirmed.orig_cl_ord_id = "o1";
    rig.state.apply(confirmed);

    ASSERT_EQ(rig.applied.size(), 2u);
    const auto& cancelled = rig.applied.back();
    ASSERT_TRUE(cancelled.latency.has_value());
    // The 5 ms the order rested is not part of how long the cancel took.
    EXPECT_LT(cancelled.latency->count(), 5000)
        << "measured from the cancel going out, not from the order";
}

TEST(LatencyTest, LeavesItEmptyWhenNothingOfOursWasBeingAnswered) {
    Rig rig;
    rig.state.record_sent(sent("o1"));
    rig.state.apply(report("o1", ExecType::New, OrdStatus::New, 100, 0, "10"));

    // The venue cancelling by itself: cancel-on-disconnect, a lifecycle
    // close, an admin cancel. There is no request of ours to measure from.
    auto unsolicited = report("o1", ExecType::Canceled, OrdStatus::Canceled, 0, 0, "11");
    unsolicited.cancel_reason = cpm::oms::CancelReason::InstrumentLifecycle;
    rig.state.apply(unsolicited);
    EXPECT_FALSE(rig.applied.back().latency.has_value());
}

TEST(LatencyTest, LeavesItEmptyForAReportReplayedFromAnotherRun) {
    Rig rig;
    // No record_sent: after a restart the send happened in a process that is
    // gone, and its stamp means nothing to this one.
    auto replayed = report("o1", ExecType::New, OrdStatus::New, 100, 0, "10");
    replayed.replayed = true;
    rig.state.apply(replayed);

    ASSERT_EQ(rig.applied.size(), 1u);
    EXPECT_FALSE(rig.applied.front().latency.has_value());
}

TEST(LatencyTest, TimesHowLongTheQuoteRestedBeforeItWasHit) {
    Rig rig;
    rig.state.record_sent(sent("o1"));
    rig.state.apply(report("o1", ExecType::New, OrdStatus::New, 100, 0, "10"));
    std::this_thread::sleep_for(4ms);

    auto filled = report("o1", ExecType::Trade, OrdStatus::Filled, 0, 100, "11");
    Fill fill;
    fill.quantity = 100;
    fill.price = 400;
    fill.trade_match_id = "t-1";
    filled.fill = fill;
    rig.state.apply(filled);

    const auto& hit = rig.applied.back();
    ASSERT_TRUE(hit.rested.has_value());
    EXPECT_GE(hit.rested->count(), 4000);
    EXPECT_FALSE(hit.latency.has_value()) << "a fill answers nobody's request";
}

// --- the files ------------------------------------------------------------

TEST(RecordLogTest, WritesAFillWithItsOrdersLatencyAndThePositionItLeft) {
    const TempDir dir("cpm-records-fill-");
    Rig rig;
    rig.state.record_sent(sent("o1"));
    rig.state.apply(report("o1", ExecType::New, OrdStatus::New, 100, 0, "10"));
    std::this_thread::sleep_for(2ms);
    auto filled = report("o1", ExecType::Trade, OrdStatus::Filled, 0, 100, "11");
    Fill fill;
    fill.quantity = 100;
    fill.price = 400;
    fill.fee = 1200;
    fill.aggressor = false;
    fill.trade_match_id = "t-1";
    filled.fill = fill;
    rig.state.apply(filled);

    RecordLog records(dir());
    ASSERT_EQ(records.open_now("2026-11-02T12:45:00.000Z"), "");
    for (const auto& applied : rig.applied) records.write(applied, "cryptomm-1");

    const auto fills = lines_of(records.fills_path());
    ASSERT_EQ(fills.size(), 2u) << "a header and the one fill";
    EXPECT_EQ(column(fills, 1, "strategy_id"), "cryptomm-1");
    EXPECT_EQ(column(fills, 1, "symbol"), kSym);
    EXPECT_EQ(column(fills, 1, "side"), "buy");
    // The same text the page and the console show, trailing zeros trimmed:
    // exact either way, and a spreadsheet reads both as the number.
    EXPECT_EQ(column(fills, 1, "qty"), "1");
    EXPECT_EQ(column(fills, 1, "price"), "0.4");
    EXPECT_EQ(column(fills, 1, "fee"), "0.0012");
    EXPECT_EQ(column(fills, 1, "aggressor"), "maker");
    EXPECT_EQ(column(fills, 1, "net_after"), "1");
    EXPECT_EQ(column(fills, 1, "trd_match_id"), "t-1");
    EXPECT_EQ(column(fills, 1, "cl_ord_id"), "o1");
    EXPECT_EQ(column(fills, 1, "replayed"), "N");
    // The order's placement latency, carried onto the fill, and the time it
    // then rested - two different numbers, neither of them blank.
    EXPECT_FALSE(column(fills, 1, "ack_latency_us").empty());
    EXPECT_GE(std::stoll(column(fills, 1, "rest_us")), 2000);
    // The New went to the other file, not this one.
    const auto orders = lines_of(records.orders_path());
    ASSERT_EQ(orders.size(), 2u);
    EXPECT_EQ(column(orders, 1, "event"), "new");
}

TEST(RecordLogTest, WritesTheOrderRowsWithTheLatencyOfWhatEachAnswers) {
    const TempDir dir("cpm-records-orders-");
    Rig rig;
    rig.state.record_sent(sent("o1"));
    rig.state.apply(report("o1", ExecType::PendingNew, OrdStatus::PendingNew, 100, 0, "9"));
    rig.state.apply(report("o1", ExecType::New, OrdStatus::New, 100, 0, "10"));

    RecordLog records(dir());
    ASSERT_EQ(records.open_now("2026-11-02T12:45:00.000Z"), "");
    for (const auto& applied : rig.applied) records.write(applied, "cryptomm-1");

    const auto orders = lines_of(records.orders_path());
    ASSERT_EQ(orders.size(), 3u);
    EXPECT_EQ(column(orders, 1, "event"), "pending-new");
    EXPECT_FALSE(column(orders, 1, "latency_us").empty());
    EXPECT_EQ(column(orders, 2, "event"), "new");
    EXPECT_EQ(column(orders, 2, "status"), "new");
    EXPECT_EQ(column(orders, 2, "leaves"), "1");
    // Both columns on the row that matters: what the add took, and what the
    // bump before it took.
    EXPECT_FALSE(column(orders, 2, "latency_us").empty());
    EXPECT_FALSE(column(orders, 2, "pending_latency_us").empty());
}

TEST(RecordLogTest, WritesACancelRejectWithNoOrderBehindIt) {
    const TempDir dir("cpm-records-reject-");
    Rig rig;
    CancelReject reject;
    reject.cl_ord_id = "c1";
    reject.orig_cl_ord_id = "o-gone";
    reject.order_id = "0";
    reject.reason = 1;
    reject.text = "UNKNOWN_ORDER, already filled";  // a comma, to be escaped
    rig.state.apply(reject);

    RecordLog records(dir());
    ASSERT_EQ(records.open_now("2026-11-02T12:45:00.000Z"), "");
    ASSERT_EQ(rig.applied.size(), 1u);
    records.write(rig.applied.front(), "");

    const auto orders = lines_of(records.orders_path());
    ASSERT_EQ(orders.size(), 2u);
    EXPECT_EQ(column(orders, 1, "event"), "cancel-reject");
    EXPECT_EQ(column(orders, 1, "cl_ord_id"), "c1");
    EXPECT_EQ(column(orders, 1, "orig_cl_ord_id"), "o-gone");
    EXPECT_EQ(column(orders, 1, "reject_reason"), "1");
    // The text held a comma, so the row has to have quoted it rather than
    // grown a column.
    EXPECT_NE(orders[1].find("\"UNKNOWN_ORDER, already filled\""), std::string::npos);
    EXPECT_EQ(column(orders, 1, "text"), "UNKNOWN_ORDER, already filled");
    EXPECT_EQ(column(orders, 1, "replayed"), "N");
}

// --- the P&L series ------------------------------------------------------

TEST(RecordLogTest, WritesAPnlRowPerPositionWithTheMarkBehindIt) {
    const TempDir dir("cpm-records-pnl-");
    RecordLog records(dir());
    ASSERT_EQ(records.open_now("2026-11-02T12:45:00.000Z"), "");

    cpm::oms::Position position;
    position.symbol = kSym;
    position.net = 100;
    position.bought = 100;
    position.bought_value = 40000;
    position.cash = 40120;
    position.fees = 120;
    position.fills = 1;

    records.pnl_row(position, 500, cpm::oms::pnl_of(position, 500));
    // The same position with no mid to mark against: the unrealized half is
    // unknown, which is not zero (GUI-Q15).
    records.pnl_row(position, std::nullopt, cpm::oms::pnl_of(position, std::nullopt));

    const auto rows = lines_of(records.pnl_path());
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(column(rows, 1, "symbol"), kSym);
    EXPECT_EQ(column(rows, 1, "net"), "1");
    EXPECT_EQ(column(rows, 1, "fees"), "0.00012");
    EXPECT_EQ(column(rows, 1, "mark"), "0.5");
    EXPECT_FALSE(column(rows, 1, "unrealized").empty());
    EXPECT_FALSE(column(rows, 1, "total").empty());

    EXPECT_EQ(column(rows, 2, "mark"), "");
    EXPECT_EQ(column(rows, 2, "unrealized"), "");
    EXPECT_EQ(column(rows, 2, "total"), "");
    EXPECT_FALSE(column(rows, 2, "realized").empty()) << "realized needs no mark";
}

TEST(RecordLogTest, WritesAShortPositionAsNegativeRatherThanFlat) {
    const TempDir dir("cpm-records-short-");
    RecordLog records(dir());
    ASSERT_EQ(records.open_now("2026-11-02T12:45:00.000Z"), "");

    cpm::oms::Position position;
    position.symbol = kSym;
    position.net = -250;  // short YES
    position.sold = 250;
    position.sold_value = 125000;
    position.cash = -125000;
    records.pnl_row(position, 500, cpm::oms::pnl_of(position, 500));

    const auto rows = lines_of(records.pnl_path());
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(column(rows, 1, "net"), "-2.5") << "format_qty would have made this a flat 0";
}

// --- the quotes file -----------------------------------------------------

TEST(RecordLogTest, WritesNoQuotesFileUnlessItWasAskedFor) {
    const TempDir dir("cpm-records-noquotes-");
    RecordLog records(dir());
    ASSERT_EQ(records.open_now("2026-11-02T12:45:00.000Z"), "");
    EXPECT_FALSE(records.quotes_enabled());

    RecordLog::Quote quote;
    quote.strategy_id = "cryptomm-1";
    quote.symbol = kSym;
    records.quote_row(quote);  // does nothing, and the caller need not ask

    EXPECT_FALSE(std::filesystem::exists(dir() + "/20261102/quotes.csv"))
        << "a run that did not ask for quotes gets no file, not an empty one";
}

TEST(RecordLogTest, WritesWhatAStrategyIsShowingAndWhyWhenItIsNot) {
    const TempDir dir("cpm-records-quotes-");
    RecordLog records(dir(), true);
    ASSERT_EQ(records.open_now("2026-11-02T12:45:00.000Z"), "");
    ASSERT_TRUE(records.quotes_enabled());

    RecordLog::Quote quoting;
    quoting.strategy_id = "cryptomm-1";
    quoting.symbol = kSym;
    quoting.state = "Running";
    quoting.armed = true;
    quoting.bid = 530;
    quoting.bid_qty = 200;
    quoting.ask = 570;
    quoting.ask_qty = 200;
    quoting.mid = 550;
    quoting.reference_mid = "68421.50";
    records.quote_row(quoting);

    RecordLog::Quote withdrawn;
    withdrawn.strategy_id = "cryptomm-1";
    withdrawn.symbol = kSym;
    withdrawn.state = "Running";
    withdrawn.armed = true;
    withdrawn.reason = "reference is stale, so the guard says do not quote";
    records.quote_row(withdrawn);

    const auto rows = lines_of(dir() + "/20261102/quotes.csv");
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(column(rows, 1, "bid"), "0.53");
    EXPECT_EQ(column(rows, 1, "ask"), "0.57");
    EXPECT_EQ(column(rows, 1, "bid_qty"), "2");
    EXPECT_EQ(column(rows, 1, "mid"), "0.55");
    // The reference venue's own scale, formatted by the caller: this file
    // does not know which venue is behind it.
    EXPECT_EQ(column(rows, 1, "reference_mid"), "68421.50");
    EXPECT_EQ(column(rows, 1, "armed"), "Y");
    EXPECT_EQ(column(rows, 1, "reason"), "");

    // Nothing resting and a reason why: both sides empty rather than zero.
    EXPECT_EQ(column(rows, 2, "bid"), "");
    EXPECT_EQ(column(rows, 2, "ask"), "");
    EXPECT_EQ(column(rows, 2, "reason"), "reference is stale, so the guard says do not quote");
}

TEST(RecordLogTest, WritesNoRowForAReplayedDuplicate) {
    const TempDir dir("cpm-records-dup-");
    Rig rig;
    rig.state.record_sent(sent("o1"));
    rig.state.apply(report("o1", ExecType::New, OrdStatus::New, 100, 0, "10"));
    // Event Resend delivers the same ExecID again: already applied, so it
    // changes nothing and must not double a row either.
    auto again = report("o1", ExecType::New, OrdStatus::New, 100, 0, "10");
    again.replayed = true;
    rig.state.apply(again);

    EXPECT_EQ(rig.applied.size(), 1u);
    EXPECT_EQ(rig.state.duplicate_reports(), 1u);
}

}  // namespace
