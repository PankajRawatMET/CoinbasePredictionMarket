// oe_client against oe_sim over loopback: the whole order path, from a signed
// Logon to decoded reports. This is what Stages 4-6 will be built on, so the
// assertions are on the decoded events rather than the raw log.
#include "cpm/oms/oe_client.hpp"
#include "cpm/oms/order_rules.hpp"
#include "cpm/oms/sim/simulator.hpp"

#include <gtest/gtest.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using cpm::oms::CancelOrder;
using cpm::oms::ExecType;
using cpm::oms::MassCancel;
using cpm::oms::NewOrder;
using cpm::oms::OrderType;
using cpm::oms::OrdStatus;
using cpm::oms::PostOnly;
using cpm::oms::Side;
using cpm::oms::TimeInForce;

const cpm::Credentials kCreds{"oe-sim-key", "oe-sim-pass", "b2Utc2ltLXNlY3JldA=="};
const std::string kSym = "SIMOE-26NOV01HOMAWY-HOM";

std::uint16_t free_port() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    socklen_t len = sizeof(addr);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    ::close(fd);
    return ntohs(addr.sin_port);
}

template <typename Pred>
bool eventually(Pred pred, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(20ms);
    }
    return pred();
}

// Everything the client decoded, collected off the QuickFIX thread.
class Events {
public:
    cpm::oms::OeCallbacks callbacks() {
        cpm::oms::OeCallbacks c;
        c.report = [this](const cpm::oms::ExecutionReport& e) { push(reports_, e); };
        c.cancel_reject = [this](const cpm::oms::CancelReject& e) { push(cancel_rejects_, e); };
        c.mass_cancel_report = [this](const cpm::oms::MassCancelReport& e) { push(mass_cancels_, e); };
        c.last_exec_id = [this](const cpm::oms::LastExecId& e) { push(last_exec_ids_, e); };
        c.resend_complete = [this](const cpm::oms::ResendComplete& e) { push(resend_completes_, e); };
        c.resend_reject = [this](const cpm::oms::ResendReject& e) { push(resend_rejects_, e); };
        c.business_reject = [this](const cpm::oms::BusinessReject& e) { push(business_rejects_, e); };
        c.decode_error = [this](const cpm::oms::DecodeError& e) { push(errors_, e); };
        return c;
    }

    std::vector<cpm::oms::ExecutionReport> reports() const { return copy(reports_); }
    std::vector<cpm::oms::CancelReject> cancel_rejects() const { return copy(cancel_rejects_); }
    std::vector<cpm::oms::MassCancelReport> mass_cancels() const { return copy(mass_cancels_); }
    std::vector<cpm::oms::LastExecId> last_exec_ids() const { return copy(last_exec_ids_); }
    std::vector<cpm::oms::ResendComplete> resend_completes() const { return copy(resend_completes_); }
    std::vector<cpm::oms::ResendReject> resend_rejects() const { return copy(resend_rejects_); }
    std::vector<cpm::oms::BusinessReject> business_rejects() const { return copy(business_rejects_); }
    std::vector<cpm::oms::DecodeError> errors() const { return copy(errors_); }

    std::string error_text() const {
        const auto all = errors();
        return all.empty() ? std::string() : all.front().reason + " in " + all.front().raw;
    }
    // Reports for one order, by ClOrdID or by the OrigClOrdID a cancel carries.
    std::vector<cpm::oms::ExecutionReport> for_order(const std::string& cl_ord_id) const {
        std::vector<cpm::oms::ExecutionReport> out;
        for (const auto& report : reports()) {
            if (report.cl_ord_id == cl_ord_id || report.orig_cl_ord_id == cl_ord_id) out.push_back(report);
        }
        return out;
    }

private:
    template <typename T>
    void push(std::vector<T>& into, const T& event) {
        std::lock_guard lock(mutex_);
        into.push_back(event);
    }
    template <typename T>
    std::vector<T> copy(const std::vector<T>& from) const {
        std::lock_guard lock(mutex_);
        return from;
    }

    mutable std::mutex mutex_;
    std::vector<cpm::oms::ExecutionReport> reports_;
    std::vector<cpm::oms::CancelReject> cancel_rejects_;
    std::vector<cpm::oms::MassCancelReport> mass_cancels_;
    std::vector<cpm::oms::LastExecId> last_exec_ids_;
    std::vector<cpm::oms::ResendComplete> resend_completes_;
    std::vector<cpm::oms::ResendReject> resend_rejects_;
    std::vector<cpm::oms::BusinessReject> business_rejects_;
    std::vector<cpm::oms::DecodeError> errors_;
};

struct Rig {
    explicit Rig(cpm::oms::sim::SimOptions sim_options = {})
        : port(free_port()), sim_logger(sim_log), client_logger(client_log) {
        sim_options.credentials = kCreds;
        sim_options.port = port;
        sim_options.dict_dir = CPM_DICT_DIR;
        sim = std::make_unique<cpm::oms::sim::Simulator>(std::move(sim_options), sim_logger);
        sim->start();

        cpm::oms::OeClientOptions o;
        o.config.credentials = kCreds;
        o.config.endpoint = {"127.0.0.1", port, false, ""};
        o.config.subaccount = "dev-1";
        o.dict_dir = CPM_DICT_DIR;
        o.reconnect_interval_secs = 1;
        client = std::make_unique<cpm::oms::OeClient>(std::move(o), client_logger);
        client->on_events(events.callbacks());
        cpm::oms::OrderLimits limits;
        limits.allow_orders = true;
        client->set_limits(limits);
        client->start();
    }
    ~Rig() {
        client->stop();
        sim->stop();
    }

    // A limit order with a fresh ClOrdID.
    NewOrder order(const std::string& id, Side side, cpm::Qty quantity, cpm::Price price,
                   const std::string& symbol = kSym) {
        NewOrder o;
        o.cl_ord_id = id;
        o.symbol = symbol;
        o.side = side;
        o.quantity = quantity;
        o.price = price;
        o.type = OrderType::Limit;
        o.time_in_force = TimeInForce::Gtc;
        return o;
    }

    std::uint16_t port;
    cpm::oms::sim::SimOptions options;
    std::ostringstream sim_log;
    std::ostringstream client_log;
    cpm::Logger sim_logger;
    cpm::Logger client_logger;
    Events events;
    std::unique_ptr<cpm::oms::sim::Simulator> sim;
    std::unique_ptr<cpm::oms::OeClient> client;
};

TEST(OeSimSession, PlacesARestingOrderAndSeesItAccepted) {
    Rig rig;
    ASSERT_TRUE(rig.client->wait_logged_on(5s)) << rig.client_log.str();
    // The seeded book rests asks from 0.550, so 0.400 cannot cross.
    ASSERT_EQ(rig.client->send(rig.order("o1", Side::Buy, 100, 400)), "o1");

    ASSERT_TRUE(eventually([&] { return !rig.events.reports().empty(); }, 5s)) << rig.client_log.str();
    const auto report = rig.events.reports().front();
    EXPECT_EQ(report.exec_type, ExecType::New);
    EXPECT_EQ(report.status, OrdStatus::New);
    EXPECT_EQ(report.cl_ord_id, "o1");
    EXPECT_EQ(report.symbol, kSym);
    EXPECT_EQ(report.side, Side::Buy);
    EXPECT_EQ(report.price, 400);
    EXPECT_EQ(report.leaves_quantity, 100);
    EXPECT_EQ(report.cumulative_quantity, 0);
    EXPECT_FALSE(report.order_id.empty());
    EXPECT_FALSE(report.exec_id.empty());
    EXPECT_FALSE(report.fill.has_value());
    EXPECT_TRUE(rig.events.errors().empty()) << rig.events.error_text();

    // And the simulator really is holding it.
    const auto book = rig.sim->book(kSym, Side::Buy);
    EXPECT_TRUE(std::any_of(book.begin(), book.end(),
                            [](const auto& o) { return o.cl_ord_id == "o1"; }));
}

TEST(OeSimSession, CancelsTheOrderItPlaced) {
    Rig rig;
    ASSERT_TRUE(rig.client->wait_logged_on(5s)) << rig.client_log.str();
    rig.client->send(rig.order("o1", Side::Buy, 100, 400));
    ASSERT_TRUE(eventually([&] { return !rig.events.reports().empty(); }, 5s));

    CancelOrder cancel{"c1", "o1", "", kSym, Side::Buy};
    ASSERT_TRUE(rig.client->cancel(cancel));
    ASSERT_TRUE(eventually([&] { return rig.events.reports().size() >= 2; }, 5s)) << rig.client_log.str();

    const auto canceled = rig.events.reports().back();
    EXPECT_EQ(canceled.exec_type, ExecType::Canceled);
    EXPECT_EQ(canceled.status, OrdStatus::Canceled);
    // The report names the cancel, with the order in OrigClOrdID (OE-O2).
    EXPECT_EQ(canceled.cl_ord_id, "c1");
    EXPECT_EQ(canceled.orig_cl_ord_id, "o1");
    EXPECT_EQ(canceled.leaves_quantity, 0);
    EXPECT_EQ(canceled.text, "USER_REQUEST");
    EXPECT_EQ(canceled.cancel_reason, cpm::oms::CancelReason::None);
    // Looking the order up by its own id finds both reports.
    EXPECT_EQ(rig.events.for_order("o1").size(), 2u);
    // One call: begin() and end() from different temporaries would dangle.
    const auto book = rig.sim->book(kSym, Side::Buy);
    EXPECT_TRUE(std::none_of(book.begin(), book.end(),
                             [](const auto& o) { return o.cl_ord_id == "o1"; }));
}

TEST(OeSimSession, ACrossingOrderFillsWithItsFee) {
    Rig rig;
    ASSERT_TRUE(rig.client->wait_logged_on(5s)) << rig.client_log.str();
    // 0.600 crosses the seeded ask at 0.550.
    rig.client->send(rig.order("taker", Side::Buy, 100, 600));
    ASSERT_TRUE(eventually([&] { return !rig.events.for_order("taker").empty(); }, 5s))
        << rig.client_log.str();

    const auto reports = rig.events.for_order("taker");
    ASSERT_GE(reports.size(), 1u);
    const auto& fill = reports.front();
    EXPECT_EQ(fill.exec_type, ExecType::Trade);
    EXPECT_EQ(fill.status, OrdStatus::Filled);
    ASSERT_TRUE(fill.fill.has_value());
    EXPECT_EQ(fill.fill->price, 550) << "the resting price, not the taker's limit";
    EXPECT_EQ(fill.fill->quantity, 100);
    EXPECT_TRUE(fill.fill->aggressor);
    EXPECT_FALSE(fill.fill->trade_match_id.empty());
    ASSERT_TRUE(fill.fill->fee.has_value());
    EXPECT_GT(*fill.fill->fee, 0) << "a taker pays";
    EXPECT_EQ(fill.cumulative_quantity, 100);
    EXPECT_EQ(fill.leaves_quantity, 0);
    EXPECT_EQ(fill.average_price, 550);
    EXPECT_TRUE(rig.events.errors().empty()) << rig.events.error_text();
}

TEST(OeSimSession, AnIocRemainderComesBackAsExpired) {
    Rig rig;
    ASSERT_TRUE(rig.client->wait_logged_on(5s)) << rig.client_log.str();
    auto ioc = rig.order("ioc", Side::Buy, 100000, 560);  // far more than the book holds
    ioc.time_in_force = TimeInForce::Ioc;
    rig.client->send(ioc);

    ASSERT_TRUE(eventually(
        [&] {
            const auto reports = rig.events.for_order("ioc");
            return std::any_of(reports.begin(), reports.end(),
                               [](const auto& r) { return r.exec_type == ExecType::Expired; });
        },
        5s))
        << rig.client_log.str();

    const auto reports = rig.events.for_order("ioc");
    const auto expired = std::find_if(reports.begin(), reports.end(), [](const auto& r) {
        return r.exec_type == ExecType::Expired;
    });
    ASSERT_NE(expired, reports.end());
    EXPECT_EQ(expired->status, OrdStatus::Expired);
    EXPECT_EQ(expired->text, "TIME_IN_FORCE");
    EXPECT_FALSE(expired->reject_reason.has_value()) << "an expiry carries no OrdRejReason";
    EXPECT_GT(expired->cumulative_quantity, 0) << "it filled what it could first";
}

TEST(OeSimSession, ARejectedOrderCarriesItsReason) {
    Rig rig;
    ASSERT_TRUE(rig.client->wait_logged_on(5s)) << rig.client_log.str();
    rig.client->send(rig.order("dup", Side::Buy, 100, 400));
    ASSERT_TRUE(eventually([&] { return !rig.events.reports().empty(); }, 5s));
    // The same ClOrdID again, which the engine rejects rather than refusing on
    // receipt, so it comes back as an ExecutionReport.
    rig.client->send(rig.order("dup", Side::Buy, 100, 410));

    ASSERT_TRUE(eventually(
        [&] {
            const auto reports = rig.events.for_order("dup");
            return std::any_of(reports.begin(), reports.end(),
                               [](const auto& r) { return r.exec_type == ExecType::Rejected; });
        },
        5s))
        << rig.client_log.str();
    const auto reports = rig.events.for_order("dup");
    const auto rejected = std::find_if(reports.begin(), reports.end(), [](const auto& r) {
        return r.exec_type == ExecType::Rejected;
    });
    ASSERT_NE(rejected, reports.end());
    EXPECT_EQ(rejected->status, OrdStatus::Rejected);
    EXPECT_EQ(rejected->reject_reason, 6);
    EXPECT_EQ(rejected->text, "DUPLICATE_CLIENT_ORDER_ID");
}

TEST(OeSimSession, CancellingAnUnknownOrderIsRejected) {
    Rig rig;
    ASSERT_TRUE(rig.client->wait_logged_on(5s)) << rig.client_log.str();
    CancelOrder cancel{"c1", "never-existed", "", kSym, Side::Buy};
    ASSERT_TRUE(rig.client->cancel(cancel));

    ASSERT_TRUE(eventually([&] { return !rig.events.cancel_rejects().empty(); }, 5s))
        << rig.client_log.str();
    const auto reject = rig.events.cancel_rejects().front();
    EXPECT_EQ(reject.cl_ord_id, "c1");
    EXPECT_EQ(reject.orig_cl_ord_id, "never-existed");
    EXPECT_EQ(reject.order_id, "0");
    EXPECT_EQ(reject.reason, 1);
    EXPECT_EQ(reject.text, "UNKNOWN_ORDER");
    EXPECT_TRUE(rig.events.reports().empty()) << "a cancel reject is not an ExecutionReport";
}

TEST(OeSimSession, MassCancelReportsEveryOrderThenASummary) {
    Rig rig;
    ASSERT_TRUE(rig.client->wait_logged_on(5s)) << rig.client_log.str();
    rig.client->send(rig.order("m-a", Side::Buy, 100, 400));
    rig.client->send(rig.order("m-b", Side::Buy, 100, 390));
    rig.client->send(rig.order("m-c", Side::Sell, 100, 700));
    ASSERT_TRUE(eventually([&] { return rig.events.reports().size() >= 3; }, 5s)) << rig.client_log.str();

    MassCancel request;
    request.cl_ord_id = "kill-1";
    request.scope = MassCancel::Scope::All;
    request.side = Side::Buy;
    ASSERT_TRUE(rig.client->mass_cancel(request));

    ASSERT_TRUE(eventually([&] { return !rig.events.mass_cancels().empty(); }, 5s)) << rig.client_log.str();
    const auto summary = rig.events.mass_cancels().front();
    EXPECT_TRUE(summary.accepted);
    EXPECT_EQ(summary.cl_ord_id, "kill-1");
    // TotalAffectedOrders is the authoritative signal for what actually ran.
    ASSERT_TRUE(summary.affected_orders.has_value());
    EXPECT_EQ(*summary.affected_orders, 2) << "the two buys, not the sell";
    // An accepted mass cancel always reports scope 7, whatever was asked for.
    EXPECT_EQ(summary.requested_scope, MassCancel::Scope::All);

    // Each canceled order got its own report first, labelled as a mass cancel.
    for (const auto& id : {"m-a", "m-b"}) {
        const auto reports = rig.events.for_order(id);
        const auto canceled = std::find_if(reports.begin(), reports.end(), [](const auto& r) {
            return r.exec_type == ExecType::Canceled;
        });
        ASSERT_NE(canceled, reports.end()) << id;
        EXPECT_EQ(canceled->text, "USER_MASS_CANCEL");
    }
    const auto sells = rig.sim->book(kSym, Side::Sell);
    EXPECT_GE(sells.size(), 1u) << "the sell survived";
}

// The recovery pair, which Stage 5 is built on.
TEST(OeSimSession, ReplaysPastEventsOnRequest) {
    Rig rig;
    ASSERT_TRUE(rig.client->wait_logged_on(5s)) << rig.client_log.str();
    rig.client->send(rig.order("r1", Side::Buy, 100, 400));
    rig.client->send(rig.order("r2", Side::Buy, 100, 390));
    ASSERT_TRUE(eventually([&] { return rig.events.reports().size() >= 2; }, 5s)) << rig.client_log.str();
    const auto first_exec_id = rig.events.reports().front().exec_id;

    // F1: how far has the exchange got?
    ASSERT_TRUE(rig.client->request_last_exec_id());
    ASSERT_TRUE(eventually([&] { return !rig.events.last_exec_ids().empty(); }, 5s));
    const auto newest = rig.events.last_exec_ids().front().exec_id;
    EXPECT_GE(std::stoull(newest), std::stoull(first_exec_id));

    // F3 from our own bookmark: the events come back marked as replays.
    const auto before = rig.events.reports().size();
    ASSERT_TRUE(rig.client->request_event_resend(first_exec_id));
    ASSERT_TRUE(eventually([&] { return !rig.events.resend_completes().empty(); }, 5s))
        << rig.client_log.str();
    EXPECT_EQ(rig.events.resend_completes().front().event_count, 2);

    const auto after = rig.events.reports();
    ASSERT_GT(after.size(), before);
    const auto replayed = std::count_if(after.begin(), after.end(),
                                        [](const auto& r) { return r.replayed; });
    EXPECT_EQ(replayed, 2) << "PossResend(97)=Y distinguishes a replay from a live event";
    // A replay keeps the original ExecID and TransactTime.
    const auto first_replay = std::find_if(after.begin(), after.end(),
                                           [](const auto& r) { return r.replayed; });
    ASSERT_NE(first_replay, after.end());
    EXPECT_EQ(first_replay->exec_id, first_exec_id);
    EXPECT_TRUE(rig.events.errors().empty()) << rig.events.error_text();
}

TEST(OeSimSession, AResendRangeThatOverlapsNothingIsRejected) {
    Rig rig;
    ASSERT_TRUE(rig.client->wait_logged_on(5s)) << rig.client_log.str();
    ASSERT_TRUE(rig.client->request_event_resend("1", "2"));
    ASSERT_TRUE(eventually([&] { return !rig.events.resend_rejects().empty(); }, 5s))
        << rig.client_log.str();
    EXPECT_EQ(rig.events.resend_rejects().front().reason, 1);
    EXPECT_EQ(rig.events.resend_rejects().front().text, "BEGIN_EXEC_ID_TOO_SMALL");
}

// An instrument with a speed bump accepts the order as PendingNew first.
TEST(OeSimSession, ASpeedBumpedOrderIsPendingNewThenNew) {
    Rig rig;
    ASSERT_TRUE(rig.client->wait_logged_on(5s)) << rig.client_log.str();
    rig.client->send(rig.order("sb", Side::Buy, 100, 400, "SIMOE-BUMP-26NOV01-YES"));

    ASSERT_TRUE(eventually([&] { return rig.events.for_order("sb").size() >= 2; }, 5s))
        << rig.client_log.str();
    const auto reports = rig.events.for_order("sb");
    EXPECT_EQ(reports[0].exec_type, ExecType::PendingNew);
    EXPECT_EQ(reports[0].status, OrdStatus::PendingNew);
    EXPECT_EQ(reports[0].leaves_quantity, 100);
    EXPECT_EQ(reports[1].exec_type, ExecType::New);
    EXPECT_EQ(reports[1].status, OrdStatus::New);
    EXPECT_TRUE(rig.events.errors().empty()) << rig.events.error_text();
}

// Nothing the simulator sends should ever fail to decode.
TEST(OeSimSession, TheWholePathDecodesWithoutErrors) {
    Rig rig;
    ASSERT_TRUE(rig.client->wait_logged_on(5s)) << rig.client_log.str();
    rig.client->send(rig.order("a1", Side::Buy, 100, 400));
    rig.client->send(rig.order("a2", Side::Buy, 50, 600));  // crosses
    auto fok = rig.order("a3", Side::Buy, 100000, 600);
    fok.time_in_force = TimeInForce::Fok;
    rig.client->send(fok);
    auto post_only = rig.order("a4", Side::Buy, 100, 600);
    post_only.post_only = PostOnly::Reject;
    rig.client->send(post_only);
    rig.client->cancel(CancelOrder{"a5", "a1", "", kSym, Side::Buy});

    ASSERT_TRUE(eventually([&] { return rig.events.reports().size() >= 5; }, 10s)) << rig.client_log.str();
    EXPECT_TRUE(rig.events.errors().empty()) << rig.events.error_text();
    EXPECT_TRUE(rig.events.business_rejects().empty());

    // The FOK could not fill, and post-only would have crossed.
    const auto fok_reports = rig.events.for_order("a3");
    ASSERT_FALSE(fok_reports.empty());
    EXPECT_EQ(fok_reports.front().exec_type, ExecType::Expired);
    EXPECT_EQ(fok_reports.front().text, "FOK_NOT_FILLED");
    const auto po_reports = rig.events.for_order("a4");
    ASSERT_FALSE(po_reports.empty());
    EXPECT_EQ(po_reports.front().exec_type, ExecType::Rejected);
    EXPECT_EQ(po_reports.front().reject_reason, 11);
}

}  // namespace
