// Stage 5 and Stage 6 end to end: the durable bookmark, the reconcile-before-
// submit rule, the flatten-reconcile-halt sequence of OE-Q6, and settlement.
#include "cpm/oms/bookmark.hpp"
#include "cpm/oms/oe_client.hpp"
#include "cpm/oms/order_rules.hpp"
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
#include <vector>

namespace {

using namespace std::chrono_literals;
using cpm::oms::Bookmark;
using cpm::oms::BookmarkFile;
using cpm::oms::CancelOrder;
using cpm::oms::CancelReject;
using cpm::oms::ExecType;
using cpm::oms::ExecutionReport;
using cpm::oms::Fill;
using cpm::oms::Halt;
using cpm::oms::OrdStatus;
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

// Two threads saving at once is the normal case once a strategy is armed: the
// session thread bookmarks each ExecutionReport while the strategy thread
// bookmarks each ClOrdID it allocates. Neither may fail, and neither may leave
// a half-written file or a stray temporary behind.
TEST(BookmarkTest, SurvivesConcurrentWriters) {
    const auto path = temp_path("cpm-bookmark-race-");
    std::filesystem::remove(path);
    BookmarkFile file(path);

    constexpr int kWriters = 4;
    constexpr int kWrites = 200;
    std::atomic<int> failures{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> writers;
    for (int writer = 0; writer < kWriters; ++writer) {
        writers.emplace_back([&, writer] {
            while (!go.load()) std::this_thread::yield();
            for (int i = 0; i < kWrites; ++i) {
                try {
                    file.write({"4808377765944360" + std::to_string(writer) + std::to_string(i),
                                static_cast<std::uint64_t>(i)});
                } catch (const std::exception&) {
                    ++failures;
                }
            }
        });
    }
    go.store(true);
    for (auto& thread : writers) thread.join();

    EXPECT_EQ(failures.load(), 0) << "a concurrent save must not lose the bookmark";
    // Whichever save landed last, the file is one whole version and parses.
    EXPECT_NO_THROW(BookmarkFile(path).read());
    for (const auto& entry :
         std::filesystem::directory_iterator(std::filesystem::temp_directory_path())) {
        EXPECT_EQ(entry.path().string().find(path + ".tmp"), std::string::npos)
            << "left a temporary behind: " << entry.path();
    }
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

// ExecIDs are decimal integers carried as strings, so the newer of two is the
// longer one, or the greater when they are the same length. Comparing them
// lexically alone reads 9 as newer than 10, which is how a bookmark ahead of
// the retention window came to look like one behind it.
TEST(ExecIdOrder, OrdersByLengthThenValue) {
    using cpm::oms::exec_id_before;
    EXPECT_TRUE(exec_id_before("9", "10")) << "length first, or 9 reads as newer than 10";
    EXPECT_FALSE(exec_id_before("10", "9"));
    EXPECT_TRUE(exec_id_before("483656036197072901", "483656078886699010"));
    EXPECT_FALSE(exec_id_before("483656078886699010", "483656036197072901"));
    EXPECT_FALSE(exec_id_before("150", "150")) << "neither is before the other";
    EXPECT_TRUE(exec_id_before("", "1")) << "nothing processed is older than anything";
    EXPECT_FALSE(exec_id_before("1", ""));
    EXPECT_FALSE(exec_id_before("", ""));
}

// A bookmark NEWER than anything the exchange still holds is the ordinary
// state of affairs after a quiet spell: the retention window is up to 24
// hours and not guaranteed, so our own events age out of it while the
// bookmark keeps them. There is nothing to replay and nothing missing, and
// the run must go live rather than stop. This halted a live run on
// 2026-10-30T07:41:55Z.
TEST(Recovery, ABookmarkNewerThanTheRetentionWindowIsNotAGap) {
    // A bookmark left by an earlier run, as a restart would find it.
    const auto path = temp_path("cpm-rec-ahead-");
    std::filesystem::remove(path);
    BookmarkFile(path).write({"483656078886699010", 3});

    Rig rig("cpm-rec-ahead-");
    ASSERT_TRUE(rig.client->wait_logged_on(5s)) << rig.client_log.str();
    ASSERT_TRUE(eventually([&] { return rig.reconciler->state() != RecoveryState::Starting; }, 5s));

    // F2 answers with an older ExecID than ours: everything we processed has
    // aged out of the window. Driven directly, so the test does not depend on
    // the simulator's own retention.
    cpm::oms::LastExecId newest;
    newest.exec_id = "483656036197072901";
    rig.reconciler->callbacks().last_exec_id(newest);

    EXPECT_EQ(rig.halt_count(), 0u) << "being ahead of the window stopped the run";
    EXPECT_TRUE(eventually([&] { return rig.reconciler->state() == RecoveryState::Live; }, 5s))
        << "it must go live: there is nothing to replay";
    EXPECT_NE(rig.client_log.str().find("ahead of the exchange's retention window"),
              std::string::npos)
        << rig.client_log.str();
    // And it asked for no replay, which is the request that was rejected.
    EXPECT_EQ(rig.client_log.str().find("- replaying"), std::string::npos) << rig.client_log.str();
}

// And the backstop, for the window ageing past the bookmark between F2 and
// F3: reason 2 is "your BeginExecID is past the newest stored event", which
// is not data loss. Reason 1 - the bookmark predating the window - still is.
TEST(Recovery, AResendRejectForBeingAheadDoesNotHaltButOneForBeingBehindDoes) {
    Rig rig("cpm-rec-reject2-");
    auto callbacks = rig.reconciler->callbacks();

    cpm::oms::ResendReject ahead;
    ahead.reason = 2;
    ahead.text = "begin ExecID is after the last retained fill";
    callbacks.resend_reject(ahead);
    EXPECT_EQ(rig.halt_count(), 0u) << "nothing to replay is not data loss";

    cpm::oms::ResendReject behind;
    behind.reason = 1;
    behind.text = "begin ExecID predates the retention window";
    callbacks.resend_reject(behind);
    ASSERT_TRUE(eventually([&] { return rig.halt_count() > 0; }, 5s));
    EXPECT_EQ(rig.halt().value().reason, Halt::Reason::DataLoss);
}

// A cancel that lost a race with the fill it was chasing is not a fault, and
// must not stop a live run. The order manager learns of a fill on the session
// thread while a strategy's own view of its quotes waits for the next tick,
// so a cancel for an order that has just finished does go out - and the
// venue answers UNKNOWN_ORDER. This halted a live run on 2026-10-30.
//
// One session delivers in sequence, so the report that ended the order is
// always applied before the reject for the cancel that was too late, which is
// what makes the correlation the spec advises reliable.
TEST(Recovery, ALateCancelForAFinishedOrderDoesNotHalt) {
    Rig rig("cpm-rec-late-cancel-");
    auto callbacks = rig.reconciler->callbacks();

    rig.state.record_sent(rig.order("mm-1"));
    ExecutionReport fill;
    fill.cl_ord_id = "mm-1";
    fill.exec_id = "480837776594436096";
    fill.symbol = kSym;
    fill.side = Side::Buy;
    fill.exec_type = ExecType::Trade;
    fill.status = OrdStatus::Filled;
    fill.leaves_quantity = 0;
    fill.cumulative_quantity = 100;
    fill.fill = Fill{100, 400, "t1", false, 0};
    rig.state.apply(fill);
    ASSERT_TRUE(rig.state.order("mm-1")->terminal);

    CancelReject late;
    late.cl_ord_id = "mm-2";
    late.orig_cl_ord_id = "mm-1";
    late.order_id = "0";
    late.reason = 1;  // UNKNOWN_ORDER, which is what this venue sends for a fill
    late.text = "UNKNOWN_ORDER";
    callbacks.cancel_reject(late);
    EXPECT_EQ(rig.halt_count(), 0u) << "a cancel that was merely late stopped the run";
    EXPECT_FALSE(rig.reconciler->halt().has_value());

    // A reject for an order that is still live is a different thing: nothing
    // explains it, so the run still stops (OE-Q6).
    rig.state.record_sent(rig.order("mm-3"));
    ExecutionReport resting;
    resting.cl_ord_id = "mm-3";
    resting.exec_id = "480837776594436097";
    resting.symbol = kSym;
    resting.side = Side::Buy;
    resting.exec_type = ExecType::New;
    resting.status = OrdStatus::New;
    resting.leaves_quantity = 100;
    rig.state.apply(resting);
    ASSERT_FALSE(rig.state.order("mm-3")->terminal);

    CancelReject unexplained = late;
    unexplained.cl_ord_id = "mm-4";
    unexplained.orig_cl_ord_id = "mm-3";
    callbacks.cancel_reject(unexplained);
    ASSERT_TRUE(eventually([&] { return rig.halt_count() > 0; }, 5s));
    EXPECT_EQ(rig.halt().value().reason, Halt::Reason::Reject);
    EXPECT_NE(rig.halt().value().detail.find("mm-3"), std::string::npos)
        << "the halt must name the order, not only the cancel: " << rig.halt().value().detail;
}

// An order we have never heard of is not explained either.
TEST(Recovery, ACancelRejectForAnUnknownOrderStillHalts) {
    Rig rig("cpm-rec-unknown-cancel-");
    CancelReject reject;
    reject.cl_ord_id = "mm-9";
    reject.orig_cl_ord_id = "mm-8";
    reject.reason = 1;
    reject.text = "UNKNOWN_ORDER";
    rig.reconciler->callbacks().cancel_reject(reject);
    ASSERT_TRUE(eventually([&] { return rig.halt_count() > 0; }, 5s));
    EXPECT_EQ(rig.halt().value().reason, Halt::Reason::Reject);
}

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
// A quote the close caught in flight. OrdRejReason 44 on an instrument whose
// window has ended is a race we lost, not a loss of track: the order never
// existed, and the venue has already cancelled whatever rested on the window
// it closed. The daemon already forgives the late CANCEL for the same order
// in the same quarter second (2026-11-04); this makes the two agree.
TEST(Recovery, ARejectAfterTheWindowHasEndedDoesNotHalt) {
    Reconciler::Options options;
    options.window_has_ended = [](const std::string&) { return true; };
    Rig rig("cpm-rec-late-", options);
    ASSERT_TRUE(rig.client->wait_logged_on(5s)) << rig.client_log.str();
    ASSERT_TRUE(eventually([&] { return rig.reconciler->state() == RecoveryState::Live; }, 5s));

    cpm::oms::ExecutionReport report;
    report.cl_ord_id = "o1";
    report.symbol = kSym;
    report.exec_type = cpm::oms::ExecType::Rejected;
    report.status = cpm::oms::OrdStatus::Rejected;
    report.reject_reason = 44;
    report.text = "INSTRUMENT_NOT_ACTIVE - instrument status=CLOSED, must be OPEN";
    rig.reconciler->callbacks().report(report);

    EXPECT_EQ(rig.halt_count(), 0u) << "a late order halted the run";
    EXPECT_TRUE(rig.reconciler->submission_allowed());
    EXPECT_NE(rig.client_log.str().find("late, not wrong"), std::string::npos)
        << rig.client_log.str();
}

// Narrow on purpose: the same reject on a window that is still trading means
// the daemon is wrong about what is live, and that still stops everything.
TEST(Recovery, ARejectOnALiveWindowStillHaltsWhateverTheReason) {
    Reconciler::Options options;
    options.window_has_ended = [](const std::string&) { return false; };
    Rig rig("cpm-rec-live-", options);
    ASSERT_TRUE(rig.client->wait_logged_on(5s)) << rig.client_log.str();
    ASSERT_TRUE(eventually([&] { return rig.reconciler->state() == RecoveryState::Live; }, 5s));

    cpm::oms::ExecutionReport report;
    report.cl_ord_id = "o1";
    report.symbol = kSym;
    report.exec_type = cpm::oms::ExecType::Rejected;
    report.status = cpm::oms::OrdStatus::Rejected;
    report.reject_reason = 44;
    report.text = "INSTRUMENT_NOT_ACTIVE";
    rig.reconciler->callbacks().report(report);

    EXPECT_GT(rig.halt_count(), 0u);
    EXPECT_EQ(rig.halt()->reason, Halt::Reason::Reject);
    EXPECT_FALSE(rig.reconciler->submission_allowed());
}

// And a reject with any other reason halts even after the window has gone:
// only 44 is the close, and everything else is still unexplained.
TEST(Recovery, AnotherReasonAfterTheWindowEndsStillHalts) {
    Reconciler::Options options;
    options.window_has_ended = [](const std::string&) { return true; };
    Rig rig("cpm-rec-other-", options);
    ASSERT_TRUE(rig.client->wait_logged_on(5s)) << rig.client_log.str();
    ASSERT_TRUE(eventually([&] { return rig.reconciler->state() == RecoveryState::Live; }, 5s));

    cpm::oms::ExecutionReport report;
    report.cl_ord_id = "o1";
    report.symbol = kSym;
    report.exec_type = cpm::oms::ExecType::Rejected;
    report.status = cpm::oms::OrdStatus::Rejected;
    report.reject_reason = 25;
    report.text = "INSUFFICIENT_BALANCE";
    rig.reconciler->callbacks().report(report);

    EXPECT_GT(rig.halt_count(), 0u);
    EXPECT_NE(rig.halt()->detail.find("INSUFFICIENT_BALANCE"), std::string::npos);
}

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

// A settlement that disagrees with fills we did see means we missed one,
// which is exactly the condition worth halting on.
TEST(Settlement, AMismatchWithFillsWeSawIsAMissedFill) {
    Rig rig("cpm-settle-bad-");
    ASSERT_TRUE(rig.client->wait_logged_on(5s)) << rig.client_log.str();
    ASSERT_TRUE(eventually([&] { return rig.reconciler->state() == RecoveryState::Live; }, 5s));

    // One contract of ours, seen and recorded.
    rig.state.record_sent(rig.order("buy", 600));
    rig.client->send(rig.order("buy", 600));
    ASSERT_TRUE(eventually([&] { return rig.state.position(kSym).has_value() &&
                                        rig.state.position(kSym)->net == 100; }, 5s))
        << rig.client_log.str();

    // The exchange settles five. We traded this instrument here, so the four
    // we cannot account for are fills that never reached us.
    rig.sim->settle(kSym, SettlementReport::Outcome::Yes, cpm::kPriceOne, 500, 0);
    ASSERT_TRUE(eventually([&] { return rig.state.anomalies() > 0; }, 5s)) << rig.client_log.str();
    EXPECT_TRUE(eventually([&] { return rig.halt_count() > 0; }, 5s));
    const auto halt = rig.halt();
    ASSERT_TRUE(halt.has_value());
    EXPECT_EQ(halt->reason, Halt::Reason::Anomaly);
    EXPECT_NE(halt->detail.find("a fill was missed"), std::string::npos) << halt->detail;
    EXPECT_NE(halt->detail.find("settled net 5 but our fills make 1"), std::string::npos)
        << halt->detail;
}

// The same disagreement, but on an instrument this session never touched.
// That is a restart past the exchange's replay window, not a missed fill,
// and the operator reading the halt at 3am must be told which.
TEST(Settlement, AMismatchOnAnInstrumentWeNeverTradedHereSaysItIsARestart) {
    Rig rig("cpm-settle-restart-");
    ASSERT_TRUE(rig.client->wait_logged_on(5s)) << rig.client_log.str();
    ASSERT_TRUE(eventually([&] { return rig.reconciler->state() == RecoveryState::Live; }, 5s));

    // No order sent, no fill seen: the position behind this settlement was
    // built by a process that is gone.
    rig.sim->settle(kSym, SettlementReport::Outcome::Yes, cpm::kPriceOne, 500, 0);
    ASSERT_TRUE(eventually([&] { return rig.state.anomalies() > 0; }, 5s)) << rig.client_log.str();
    const auto halt = rig.halt();
    ASSERT_TRUE(halt.has_value());
    EXPECT_NE(halt->detail.find("a restart, not a missed fill"), std::string::npos) << halt->detail;
    EXPECT_EQ(halt->detail.find("a fill was missed"), std::string::npos) << halt->detail;
}

TEST(Settlement, AShortPositionIsReportedAsShortAndNotAsFlat) {
    Rig rig("cpm-settle-short-");
    ASSERT_TRUE(rig.client->wait_logged_on(5s)) << rig.client_log.str();
    ASSERT_TRUE(eventually([&] { return rig.reconciler->state() == RecoveryState::Live; }, 5s));

    // The exchange settles us short five. format_qty renders a negative as a
    // flat 0, which would make the message disagree with itself.
    rig.sim->settle(kSym, SettlementReport::Outcome::Yes, cpm::kPriceOne, 0, 500);
    ASSERT_TRUE(eventually([&] { return rig.state.anomalies() > 0; }, 5s)) << rig.client_log.str();
    const auto halt = rig.halt();
    ASSERT_TRUE(halt.has_value());
    EXPECT_NE(halt->detail.find("settled net -5"), std::string::npos) << halt->detail;
}

}  // namespace
