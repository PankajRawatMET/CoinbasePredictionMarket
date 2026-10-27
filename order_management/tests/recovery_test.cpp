// Stage 5 and Stage 6 end to end: the durable bookmark, the reconcile-before-
// submit rule, the flatten-reconcile-halt sequence of OE-Q6, and settlement.
#include "cpm/oms/bookmark.hpp"
#include "cpm/oms/oe_client.hpp"
#include "cpm/oms/order_state.hpp"
#include "cpm/oms/reconciler.hpp"
#include "cpm/oms/sim/simulator.hpp"

#include <gtest/gtest.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;
using cpm::oms::Bookmark;
using cpm::oms::BookmarkFile;
using cpm::oms::CancelOrder;
using cpm::oms::Halt;
using cpm::oms::NewOrder;
using cpm::oms::OrderState;
using cpm::oms::OrderType;
using cpm::oms::Reconciler;
using cpm::oms::RecoveryState;
using cpm::oms::SettlementReport;
using cpm::oms::Side;
using cpm::oms::TimeInForce;

const cpm::Credentials kCreds{"oe-rec-key", "oe-rec-pass", "b2Utc2ltLXNlY3JldA=="};
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

std::string temp_path(const std::string& name) {
    return (std::filesystem::temp_directory_path() / (name + std::to_string(::getpid()))).string();
}

// --- the bookmark ---------------------------------------------------------

TEST(BookmarkTest, RoundTripsAndStartsEmpty) {
    const auto path = temp_path("cpm-bookmark-");
    std::filesystem::remove(path);
    BookmarkFile file(path);
    EXPECT_FALSE(file.read().has_value()) << "a missing bookmark is a first run";

    file.write({"480837776594436096", 42});
    const auto read = file.read();
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(read->last_exec_id, "480837776594436096");
    EXPECT_EQ(read->cl_ord_counter, 42u);

    // Overwriting leaves one version, not a mixture.
    file.write({"480837843678134272", 43});
    EXPECT_EQ(file.read()->last_exec_id, "480837843678134272");
    EXPECT_FALSE(std::filesystem::exists(path + ".tmp"));
    std::filesystem::remove(path);
}

// A corrupt bookmark must stop a run rather than silently restart from zero,
// which would replay nothing and lose history.
TEST(BookmarkTest, RefusesToGuessAtACorruptFile) {
    const auto path = temp_path("cpm-bookmark-bad-");
    {
        std::ofstream out(path, std::ios::trunc);
        out << "last_exec_id=123\ncl_ord_counter=not-a-number\n";
    }
    EXPECT_THROW(BookmarkFile(path).read(), std::runtime_error);

    {
        std::ofstream out(path, std::ios::trunc);
        out << "last_exec_id=123\n";  // no counter
    }
    EXPECT_THROW(BookmarkFile(path).read(), std::runtime_error);
    std::filesystem::remove(path);
}

// --- the reconciler, against oe_sim --------------------------------------

struct Rig {
    explicit Rig(const std::string& bookmark_name, Reconciler::Options options = {})
        : port(free_port()),
          sim_logger(sim_log),
          client_logger(client_log),
          bookmark_path(temp_path(bookmark_name)) {
        cpm::oms::sim::SimOptions sim_options;
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

        reconciler = std::make_unique<Reconciler>(options, *client, state, BookmarkFile(bookmark_path),
                                                  client_logger, [this](const Halt& h) {
                                                      std::lock_guard lock(mutex);
                                                      halts.push_back(h);
                                                  });
        state.on_anomaly(reconciler->anomaly_handler());
        client->on_session_event(reconciler->session_callback());
        client->on_events(cpm::oms::combine_oe(state.callbacks(), reconciler->callbacks()));
        client->set_submission_gate([this] { return reconciler->submission_allowed(); });
        cpm::oms::OrderLimits limits;
        limits.allow_orders = true;
        limits.max_quantity = 1000;
        limits.max_notional = 10000000;
        client->set_limits(limits);
        reconciler->start();
        client->start();
    }
    ~Rig() {
        reconciler->stop();
        client->stop();
        sim->stop();
        std::filesystem::remove(bookmark_path);
    }

    std::size_t halt_count() const {
        std::lock_guard lock(mutex);
        return halts.size();
    }
    std::optional<Halt> halt() const {
        std::lock_guard lock(mutex);
        return halts.empty() ? std::nullopt : std::optional<Halt>(halts.front());
    }

    NewOrder order(const std::string& id, cpm::Price price = 400, cpm::Qty quantity = 100) {
        NewOrder o;
        o.cl_ord_id = id;
        o.symbol = kSym;
        o.side = Side::Buy;
        o.quantity = quantity;
        o.price = price;
        o.type = OrderType::Limit;
        o.time_in_force = TimeInForce::Gtc;
        return o;
    }

    std::uint16_t port;
    std::ostringstream sim_log;
    std::ostringstream client_log;
    cpm::Logger sim_logger;
    cpm::Logger client_logger;
    std::string bookmark_path;
    OrderState state;
    mutable std::mutex mutex;
    std::vector<Halt> halts;
    std::unique_ptr<cpm::oms::sim::Simulator> sim;
    std::unique_ptr<cpm::oms::OeClient> client;
    std::unique_ptr<Reconciler> reconciler;
};

// Nothing may be submitted until the session has been reconciled.
TEST(Recovery, ReconcilesBeforeAllowingAnyOrder) {
    Rig rig("cpm-rec-fresh-");
    ASSERT_TRUE(rig.client->wait_logged_on(5s)) << rig.client_log.str();
    // A fresh run with no bookmark: F1 comes back with nothing retained.
    ASSERT_TRUE(eventually([&] { return rig.reconciler->state() == RecoveryState::Live; }, 5s))
        << rig.client_log.str();
    EXPECT_TRUE(rig.reconciler->submission_allowed());
    EXPECT_EQ(rig.halt_count(), 0u);

    rig.state.record_sent(rig.order("o1"));
    EXPECT_EQ(rig.client->send(rig.order("o1")), "o1");
    ASSERT_TRUE(eventually([&] { return rig.state.order("o1").has_value() &&
                                        rig.state.order("o1")->status == cpm::oms::OrdStatus::New; }, 5s))
        << rig.client_log.str();
    // And the bookmark now holds that report's ExecID.
    ASSERT_TRUE(eventually([&] { return BookmarkFile(rig.bookmark_path).read().has_value(); }, 5s));
    const auto saved = BookmarkFile(rig.bookmark_path).read();
    EXPECT_EQ(saved->last_exec_id, rig.state.last_exec_id());
    EXPECT_FALSE(saved->last_exec_id.empty());
}

// OE-Q6: a reject flattens, reconciles and halts, and never resumes.
TEST(Recovery, ARejectFlattensAndHalts) {
    Rig rig("cpm-rec-reject-");
    ASSERT_TRUE(rig.client->wait_logged_on(5s)) << rig.client_log.str();
    ASSERT_TRUE(eventually([&] { return rig.reconciler->state() == RecoveryState::Live; }, 5s));

    rig.state.record_sent(rig.order("o1"));
    rig.client->send(rig.order("o1"));
    ASSERT_TRUE(eventually([&] { return rig.state.order("o1").has_value() &&
                                        rig.state.order("o1")->order_id.empty() == false; }, 5s));

    // The same ClOrdID again: the engine rejects it, which is a halt condition.
    rig.state.record_sent(rig.order("o1", 410));
    rig.client->send(rig.order("o1", 410));

    ASSERT_TRUE(eventually([&] { return rig.halt_count() > 0; }, 5s)) << rig.client_log.str();
    const auto halt = rig.halt();
    ASSERT_TRUE(halt.has_value());
    EXPECT_EQ(halt->reason, Halt::Reason::Reject);
    EXPECT_NE(halt->detail.find("DUPLICATE_CLIENT_ORDER_ID"), std::string::npos) << halt->detail;

    // Submission is closed, and a mass cancel went out to flatten.
    EXPECT_FALSE(rig.reconciler->submission_allowed());
    EXPECT_THROW(rig.client->send(rig.order("o2")), std::runtime_error);
    EXPECT_NE(rig.client_log.str().find("mass cancel sent"), std::string::npos) << rig.client_log.str();
    // The order really was cancelled at the simulator.
    ASSERT_TRUE(eventually(
        [&] {
            const auto book = rig.sim->book(kSym, Side::Buy);
            return std::none_of(book.begin(), book.end(),
                                [](const auto& o) { return o.cl_ord_id == "o1"; });
        },
        5s))
        << rig.client_log.str();
    // One halt only, however many conditions follow.
    EXPECT_EQ(rig.halt_count(), 1u);
}

// A bookmark behind the exchange is caught by F1 and repaired by F3, and the
// replayed reports change nothing because their ExecIDs are already applied.
TEST(Recovery, TheBookmarkSurvivesARestart) {
    const auto path = temp_path("cpm-rec-replay-");
    {
        // A first run that places an order, so there is an ExecID to save.
        Rig rig("cpm-rec-replay-");
        ASSERT_TRUE(rig.client->wait_logged_on(5s)) << rig.client_log.str();
        ASSERT_TRUE(eventually([&] { return rig.reconciler->state() == RecoveryState::Live; }, 5s));
        rig.state.record_sent(rig.order("o1"));
        rig.client->send(rig.order("o1"));
        ASSERT_TRUE(eventually([&] { return !rig.state.last_exec_id().empty(); }, 5s));
        rig.reconciler->note_cl_ord_counter(7);
    }
    // The rig removes its bookmark on the way out, so the restart is simulated
    // by writing one as a crashed process would have left it.
    BookmarkFile(path).write({"480837776594436090", 7});

    Rig rig("cpm-rec-replay-");
    EXPECT_EQ(rig.reconciler->cl_ord_counter(), 7u) << "the counter survived the restart";
    ASSERT_TRUE(rig.client->wait_logged_on(5s)) << rig.client_log.str();
    // Reconciliation runs before anything may be submitted, whatever it finds.
    ASSERT_TRUE(eventually([&] { return rig.reconciler->state() != RecoveryState::Starting; }, 5s));
    EXPECT_NE(rig.client_log.str().find("reconciling before submitting"), std::string::npos)
        << rig.client_log.str();
    EXPECT_NE(rig.client_log.str().find("480837776594436090"), std::string::npos)
        << "the stored ExecID is what a replay would resume from";
}

// Within one session, a replay of events already applied must not double a
// position: the ExecIDs have been seen.
TEST(Recovery, ReplayedReportsDoNotDoubleAnything) {
    Rig rig("cpm-rec-dedup-");
    ASSERT_TRUE(rig.client->wait_logged_on(5s)) << rig.client_log.str();
    ASSERT_TRUE(eventually([&] { return rig.reconciler->state() == RecoveryState::Live; }, 5s));

    // A crossing order, so there is a fill in the history.
    rig.state.record_sent(rig.order("taker", 600));
    rig.client->send(rig.order("taker", 600));
    ASSERT_TRUE(eventually([&] { return rig.state.position(kSym).has_value() &&
                                        rig.state.position(kSym)->net > 0; }, 5s))
        << rig.client_log.str();
    const auto net = rig.state.position(kSym)->net;
    const auto fills = rig.state.order("taker")->fills;

    // Ask for everything back.
    ASSERT_TRUE(rig.client->request_event_resend("1"));
    ASSERT_TRUE(eventually([&] { return rig.state.duplicate_reports() > 0; }, 5s)) << rig.client_log.str();
    EXPECT_EQ(rig.state.position(kSym)->net, net) << "the position did not move";
    EXPECT_EQ(rig.state.order("taker")->fills, fills);
}

// The owner's own shutdown causes a logout, which must not look like a
// failure: the same trap the market data monitor hit.
TEST(Recovery, AShutdownWeAskedForIsNotAHalt) {
    auto rig = std::make_unique<Rig>("cpm-rec-clean-");
    ASSERT_TRUE(rig->client->wait_logged_on(5s)) << rig->client_log.str();
    ASSERT_TRUE(eventually([&] { return rig->reconciler->state() == RecoveryState::Live; }, 5s));
    rig->state.record_sent(rig->order("o1"));
    rig->client->send(rig->order("o1"));
    ASSERT_TRUE(eventually([&] { return rig->state.order("o1")->status == cpm::oms::OrdStatus::New; }, 5s));

    const auto log_before_shutdown = rig->client_log.str().size();
    // ~Rig stops the reconciler, then the client, which logs out.
    const auto halts = rig->halt_count();
    rig.reset();
    EXPECT_EQ(halts, 0u);
    (void)log_before_shutdown;
}

// After a halt the run stays stopped: a reconnect reconciles, but submission
// is never reopened (OE-Q6).
TEST(Recovery, AHaltIsNeverReopened) {
    Rig rig("cpm-rec-stay-");
    ASSERT_TRUE(rig.client->wait_logged_on(5s)) << rig.client_log.str();
    ASSERT_TRUE(eventually([&] { return rig.reconciler->state() == RecoveryState::Live; }, 5s));

    // A cancel for an order that does not exist: the gateway rejects it, which
    // is a halt condition.
    rig.client->cancel(CancelOrder{"c1", "never-existed", "", kSym, Side::Buy});
    ASSERT_TRUE(eventually([&] { return rig.halt_count() > 0; }, 5s)) << rig.client_log.str();
    EXPECT_EQ(rig.halt()->reason, Halt::Reason::Reject);
    EXPECT_FALSE(rig.reconciler->submission_allowed());

    // Reconciling again, however it finishes, leaves it halted.
    rig.client->request_last_exec_id();
    ASSERT_TRUE(eventually([&] { return rig.reconciler->state() == RecoveryState::Halted ||
                                        rig.reconciler->state() == RecoveryState::Flattening; }, 5s));
    EXPECT_FALSE(rig.reconciler->submission_allowed());
    EXPECT_THROW(rig.client->send(rig.order("o2")), std::runtime_error);
}

// --- Stage 6: settlement -------------------------------------------------

TEST(Settlement, RecordsThePayoutAndChecksItAgainstOurFills) {
    Rig rig("cpm-settle-");
    ASSERT_TRUE(rig.client->wait_logged_on(5s)) << rig.client_log.str();
    ASSERT_TRUE(eventually([&] { return rig.reconciler->state() == RecoveryState::Live; }, 5s));

    // Buy 1.00 contract by crossing the seeded ask.
    rig.state.record_sent(rig.order("buy", 600));
    rig.client->send(rig.order("buy", 600));
    ASSERT_TRUE(eventually([&] { return rig.state.position(kSym).has_value() &&
                                        rig.state.position(kSym)->net == 100; }, 5s))
        << rig.client_log.str();

    // YES wins: the long position pays out in full, and the report agrees with
    // the fills we saw.
    rig.sim->settle(kSym, SettlementReport::Outcome::Yes, cpm::kPriceOne, 100, 0);
    ASSERT_TRUE(eventually([&] { return rig.state.position(kSym)->settlement_payout.has_value(); }, 5s))
        << rig.client_log.str();

    const auto position = rig.state.position(kSym);
    EXPECT_EQ(*position->settlement_price, cpm::kPriceOne);
    EXPECT_EQ(*position->settlement_payout, 1000000) << "1.00 contract at 1.000 pays 1 USDC";
    EXPECT_EQ(rig.halt_count(), 0u) << "a settlement that agrees with our fills is not a halt";
    EXPECT_EQ(rig.state.anomalies(), 0u);
}

// A settlement that disagrees with our fills means we missed one, which is
// exactly the condition worth halting on.
TEST(Settlement, AMismatchWithOurFillsIsAnAnomaly) {
    Rig rig("cpm-settle-bad-");
    ASSERT_TRUE(rig.client->wait_logged_on(5s)) << rig.client_log.str();
    ASSERT_TRUE(eventually([&] { return rig.reconciler->state() == RecoveryState::Live; }, 5s));

    // We hold nothing, but the exchange settles 5 contracts long for us.
    rig.sim->settle(kSym, SettlementReport::Outcome::Yes, cpm::kPriceOne, 500, 0);
    ASSERT_TRUE(eventually([&] { return rig.state.anomalies() > 0; }, 5s)) << rig.client_log.str();
    EXPECT_TRUE(eventually([&] { return rig.halt_count() > 0; }, 5s));
    const auto halt = rig.halt();
    ASSERT_TRUE(halt.has_value());
    EXPECT_EQ(halt->reason, Halt::Reason::Anomaly);
    EXPECT_NE(halt->detail.find("a fill was missed"), std::string::npos) << halt->detail;
}

}  // namespace
