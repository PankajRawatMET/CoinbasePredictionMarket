// The daemon against both simulators: one session pair, a strategy created and
// armed through the control API, the fail-stop halt that crypto_mm never had,
// and the clean shutdown that leaves nothing resting.
#include "cpm/trader/trader.hpp"

#include "cpm/md/sim/simulator.hpp"
#include "cpm/oms/sim/simulator.hpp"
#include "cpm/strategy/crypto_mm.hpp"

#include <gtest/gtest.h>

#include <tuple>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <sstream>
#include <thread>

namespace {

using namespace std::chrono_literals;
using cpm::strategy::RunState;
using cpm::strategy::StrategyRegistry;
using cpm::strategy::StrategySpec;
using cpm::trader::DaemonState;
using cpm::trader::Trader;
using cpm::trader::TraderOptions;

// The market data simulator's first default instrument, which the order entry
// simulator is pointed at so both services speak about the same symbol.
const std::string kSym = "SIMGAME-26NOV01HOMAWY-HOM";

std::uint16_t free_port() {
    const int socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    ::bind(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    socklen_t length = sizeof(address);
    ::getsockname(socket_fd, reinterpret_cast<sockaddr*>(&address), &length);
    const auto port = ntohs(address.sin_port);
    ::close(socket_fd);
    return port;
}

// Ticks the daemon while it waits, because the daemon only does anything on a
// tick: that is what makes a strategy single-threaded.
bool eventually(Trader& trader, const std::function<bool()>& ready,
                std::chrono::milliseconds timeout = 10s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        trader.tick(cpm::oms::Clock::now());
        if (ready()) return true;
        std::this_thread::sleep_for(25ms);
    }
    trader.tick(cpm::oms::Clock::now());
    return ready();
}

// Both simulators and a daemon wired to them.
struct Rig {
    // `quiet_after` is the simulator's own fault flag: it keeps the session
    // up and stops sending, which is staleness rather than a disconnect.
    explicit Rig(bool allow_orders = true, std::optional<int> drop_seq = std::nullopt,
                 std::vector<std::string> watch = {},
                 std::optional<int> quiet_after = std::nullopt) {
        cpm::strategy::register_crypto_mm(registry);

        const cpm::Credentials md_creds{"md-key", "md-pass", "bWQtc2ltLXNlY3JldA=="};
        const cpm::Credentials oe_creds{"oe-key", "oe-pass", "b2Utc2ltLXNlY3JldA=="};
        const auto md_port = free_port();
        const auto oe_port = free_port();

        cpm::md::sim::SimOptions md_options;
        md_options.credentials = md_creds;
        md_options.port = md_port;
        md_options.dict_dir = CPM_DICT_DIR;
        md_options.drop_seq = drop_seq;
        md_options.stale_after_secs = quiet_after;
        md_sim = std::make_unique<cpm::md::sim::Simulator>(std::move(md_options), sim_logger);
        md_sim->start();

        cpm::oms::sim::SimOptions oe_options;
        oe_options.credentials = oe_creds;
        oe_options.port = oe_port;
        oe_options.dict_dir = CPM_DICT_DIR;
        // Serve the market data symbol, so the strategy's book and its orders
        // are at least nominally about the same instrument.
        oe_options.instruments = {{kSym, true, std::chrono::milliseconds{0}}};
        oe_sim = std::make_unique<cpm::oms::sim::Simulator>(std::move(oe_options), sim_logger);
        oe_sim->start();

        TraderOptions options;
        options.md.credentials = md_creds;
        options.md.endpoint = {"127.0.0.1", md_port, false, ""};
        options.oe.credentials = oe_creds;
        options.oe.endpoint = {"127.0.0.1", oe_port, false, ""};
        options.oe.subaccount = "dev-1";
        options.dict_dir = CPM_DICT_DIR;
        options.bookmark = bookmark_path();
        options.allow_orders = allow_orders;
        options.limits.max_quantity = 1000;
        options.limits.max_notional = 10'000'000;
        options.limits.max_orders_per_sec = 20;
        options.stale_after = 2s;
        options.watch = std::move(watch);
        trader = std::make_unique<Trader>(std::move(options), registry, logger);
    }

    ~Rig() {
        if (trader) trader->stop();
        if (oe_sim) oe_sim->stop();
        if (md_sim) md_sim->stop();
        ::unlink(bookmark_path().c_str());
    }

    static std::string bookmark_path() {
        return "cpm-trader-test-bookmark-" + std::to_string(::getpid()) + ".txt";
    }

    std::string logs() const { return log.str() + sim_log.str(); }

    StrategySpec spec() const {
        return StrategySpec{"CryptoMM", {{"symbol", kSym}, {"qty", "1"}, {"width-ticks", "2"}}};
    }

    StrategyRegistry registry;
    std::ostringstream log, sim_log;
    cpm::Logger logger{log, cpm::LogLevel::Debug};
    cpm::Logger sim_logger{sim_log};
    std::unique_ptr<cpm::md::sim::Simulator> md_sim;
    std::unique_ptr<cpm::oms::sim::Simulator> oe_sim;
    std::unique_ptr<Trader> trader;
};

TEST(Trader, LogsOnBothSessionsAndReportsItself) {
    Rig rig;
    ASSERT_EQ(rig.trader->start(), "") << rig.logs();
    const auto snapshot = rig.trader->api().snapshot();
    EXPECT_EQ(snapshot.state, DaemonState::Live);
    EXPECT_TRUE(snapshot.market_data.logged_on);
    EXPECT_TRUE(snapshot.order_entry.logged_on);
    EXPECT_TRUE(snapshot.allow_orders);
    EXPECT_TRUE(snapshot.strategies.empty());
}

TEST(Trader, CreatingAStrategySubscribesItsBook) {
    Rig rig;
    ASSERT_EQ(rig.trader->start(), "") << rig.logs();
    const auto created = rig.trader->api().create(rig.spec());
    ASSERT_TRUE(created.ok()) << created.error;

    // The book arrives because create() opened the subscription; nothing else
    // in the daemon would have.
    ASSERT_TRUE(eventually(*rig.trader, [&] {
        const auto books = rig.trader->api().snapshot().books;
        return !books.empty() && books.front().valid && books.front().best_yes_bid.has_value();
    })) << rig.logs();

    const auto book = rig.trader->api().snapshot().books.front();
    EXPECT_EQ(book.symbol, kSym);
    EXPECT_TRUE(book.implied_yes_ask.has_value());
    EXPECT_TRUE(book.mid.has_value());
}

// The book belongs to the subscription that built it. Stopping a feed and
// starting it again resubscribes with a fresh MDReqID, and the daemon used to
// leave the book bound to the old one: the new snapshot was then ignored as a
// second subscriber's (O7), and so was every update after it, so the book
// froze at the instant the feed was dropped while still reading as live. A
// strategy quoting around that mid is the best price in a market it cannot
// see, and an exit crossing to it never fills (2026-11-04, from the log).
TEST(Trader, AFeedStoppedAndStartedRebuildsTheBookInsteadOfFreezingIt) {
    Rig rig;
    ASSERT_EQ(rig.trader->start(), "") << rig.logs();
    const auto created = rig.trader->api().create(rig.spec());
    ASSERT_TRUE(created.ok()) << created.error;
    ASSERT_TRUE(eventually(*rig.trader, [&] {
        const auto books = rig.trader->api().snapshot().books;
        return !books.empty() && books.front().valid && books.front().mid.has_value();
    })) << rig.logs();

    ASSERT_TRUE(rig.trader->api().stop_feed(created.id).ok());
    // Unbound with the subscription, so nothing of the old book is left to
    // read as a price.
    {
        const auto books = rig.trader->api().snapshot().books;
        ASSERT_FALSE(books.empty()) << "the row keeps its symbol when the feed stops";
        EXPECT_FALSE(books.front().valid);
        EXPECT_FALSE(books.front().mid.has_value());
    }

    ASSERT_TRUE(rig.trader->api().start_feed(created.id).ok());
    // Live again, from the new subscription's own snapshot.
    ASSERT_TRUE(eventually(*rig.trader, [&] {
        const auto books = rig.trader->api().snapshot().books;
        return !books.empty() && books.front().valid && books.front().mid.has_value();
    })) << rig.logs();

    // And still moving: the simulator's book changes several times a second,
    // so a book that never changes again is the frozen one.
    const auto top = [&] {
        const auto book = rig.trader->api().snapshot().books.front();
        return std::make_tuple(book.best_yes_bid, book.best_yes_bid_qty, book.implied_yes_ask,
                               book.best_no_bid_qty, book.bids.size(), book.asks.size());
    };
    const auto first = top();
    EXPECT_TRUE(eventually(*rig.trader, [&] { return top() != first; }))
        << "the book stopped moving after the resubscribe: " << rig.logs();
}

// --- the watch list (G15) -------------------------------------------------
// What the daemon holds for its own sake: the book is up from startup, with
// no strategy and no Start Feed, so the page shows a market before anything
// is quoted and a position can be priced whatever happens to the row that
// opened it.
TEST(Trader, WatchedInstrumentsAreSubscribedBeforeAnyStrategyExists) {
    Rig rig(true, std::nullopt, {kSym});
    ASSERT_EQ(rig.trader->start(), "") << rig.logs();

    ASSERT_TRUE(eventually(*rig.trader, [&] {
        const auto book = rig.trader->api().snapshot().books;
        return !book.empty() && book.front().valid && book.front().mid.has_value();
    })) << rig.logs();
    EXPECT_TRUE(rig.trader->api().snapshot().strategies.empty())
        << "the book is the daemon's, not a strategy's";
}

// Stopping a strategy's feed must not take a watched book with it: the
// daemon holds that subscription for its own sake, and an exit priced from
// it is the reason it is there.
TEST(Trader, StoppingAStrategysFeedKeepsAWatchedBook) {
    Rig rig(true, std::nullopt, {kSym});
    ASSERT_EQ(rig.trader->start(), "") << rig.logs();
    const auto created = rig.trader->api().create(rig.spec());
    ASSERT_TRUE(created.ok()) << created.error;
    ASSERT_TRUE(eventually(*rig.trader, [&] {
        const auto book = rig.trader->api().snapshot().books;
        return !book.empty() && book.front().valid && book.front().mid.has_value();
    })) << rig.logs();

    ASSERT_TRUE(rig.trader->api().stop_feed(created.id).ok());
    const auto book = rig.trader->api().snapshot().books;
    ASSERT_FALSE(book.empty());
    EXPECT_TRUE(book.front().valid) << "a watched book outlives the row that was reading it";
    EXPECT_TRUE(book.front().mid.has_value());

    // And it is still being rebuilt, not frozen at the moment the row let go.
    const auto top = [&] {
        const auto view = rig.trader->api().snapshot().books.front();
        return std::make_tuple(view.best_yes_bid, view.best_yes_bid_qty, view.implied_yes_ask);
    };
    const auto first = top();
    EXPECT_TRUE(eventually(*rig.trader, [&] { return top() != first; })) << rig.logs();
}

// Staleness is fail-stop for the whole daemon, so a quiet market nobody is
// trading must not be able to stop one that is being traded. A watched book
// is handed to FeedMonitor only when a strategy starts on it.
TEST(Trader, AQuietWatchedBookDoesNotHaltTheDaemon) {
    // The simulator goes quiet two seconds in, with the session still up:
    // staleness, which is what FeedMonitor judges, and not a disconnect,
    // which halts whatever is subscribed.
    Rig rig(true, std::nullopt, {kSym}, 2);
    ASSERT_EQ(rig.trader->start(), "") << rig.logs();
    ASSERT_TRUE(eventually(*rig.trader, [&] {
        const auto book = rig.trader->api().snapshot().books;
        return !book.empty() && book.front().valid;
    })) << rig.logs();

    // Well past stale_after (2 s) with nothing quoting it.
    const auto deadline = std::chrono::steady_clock::now() + 6s;
    while (std::chrono::steady_clock::now() < deadline) {
        rig.trader->tick(cpm::oms::Clock::now());
        std::this_thread::sleep_for(25ms);
    }
    EXPECT_NE(rig.trader->api().snapshot().state, cpm::trader::DaemonState::Halted)
        << "a book nobody is quoting stopped a daemon that was not quoting it: " << rig.logs();
}

// The same book, once a strategy is running on it, is monitored again: now
// something is trading it, and a dead feed under it must stop the run.
TEST(Trader, AWatchedBookIsMonitoredOnceAStrategyIsOnIt) {
    Rig rig(true, std::nullopt, {kSym}, 2);
    ASSERT_EQ(rig.trader->start(), "") << rig.logs();
    const auto created = rig.trader->api().create(rig.spec());
    ASSERT_TRUE(created.ok()) << created.error;
    ASSERT_TRUE(rig.trader->api().start(created.id).ok());
    ASSERT_TRUE(eventually(*rig.trader, [&] {
        const auto book = rig.trader->api().snapshot().books;
        return !book.empty() && book.front().valid;
    })) << rig.logs();

    // The same silence, now under a running strategy.
    EXPECT_TRUE(eventually(*rig.trader,
                           [&] {
                               return rig.trader->api().snapshot().state ==
                                      cpm::trader::DaemonState::Halted;
                           },
                           10s))
        << "a strategy's own feed went quiet and the daemon carried on: " << rig.logs();
}

// A series nobody can resolve is a warning, not a failure to start: the
// lifecycle feed may simply not have seen it roll yet, and the next roll
// picks it up.
TEST(Trader, AnUnresolvableWatchedSeriesDoesNotStopTheDaemon) {
    Rig rig(true, std::nullopt, {"BTCD15M"});
    EXPECT_EQ(rig.trader->start(), "") << rig.logs();
    EXPECT_NE(rig.logs().find("not watching BTCD15M"), std::string::npos) << rig.logs();
    EXPECT_TRUE(rig.trader->api().snapshot().books.empty());
}

TEST(Trader, RefusesAStrategyWhoseFeedCouldNotBeOpened) {
    Rig rig;
    // Not started, so market data is not logged on and no book could arrive.
    const auto created = rig.trader->api().create(rig.spec());
    EXPECT_FALSE(created.ok());
    EXPECT_NE(created.error.find("not logged on"), std::string::npos);
    EXPECT_TRUE(rig.trader->api().snapshot().strategies.empty());
}

TEST(Trader, AnArmedStrategyQuotesAndStoppingItLeavesNothingResting) {
    Rig rig;
    ASSERT_EQ(rig.trader->start(), "") << rig.logs();
    const auto id = rig.trader->api().create(rig.spec()).id;
    ASSERT_FALSE(id.empty());
    ASSERT_TRUE(rig.trader->api().start(id).ok());
    ASSERT_TRUE(rig.trader->api().arm(id).ok());

    ASSERT_TRUE(eventually(*rig.trader, [&] {
        const auto info = rig.trader->api().snapshot().strategies;
        return !info.empty() && info.front().status.placed >= 2;
    })) << rig.logs();

    // They really are at the venue, on both sides.
    ASSERT_TRUE(eventually(*rig.trader, [&] {
        const auto bids = rig.oe_sim->book(kSym, cpm::oms::Side::Buy);
        const auto asks = rig.oe_sim->book(kSym, cpm::oms::Side::Sell);
        const auto ours = [](const auto& orders) {
            return std::any_of(orders.begin(), orders.end(), [](const auto& order) {
                return order.cl_ord_id.rfind("mm-", 0) == 0;
            });
        };
        return ours(bids) && ours(asks);
    })) << rig.logs();

    ASSERT_TRUE(rig.trader->api().stop(id).ok());
    ASSERT_TRUE(eventually(*rig.trader, [&] {
        return rig.trader->api().snapshot().strategies.front().state == RunState::Stopped;
    })) << rig.logs();

    const auto bids = rig.oe_sim->book(kSym, cpm::oms::Side::Buy);
    const auto asks = rig.oe_sim->book(kSym, cpm::oms::Side::Sell);
    const auto none_of_ours = [](const auto& orders) {
        return std::none_of(orders.begin(), orders.end(), [](const auto& order) {
            return order.cl_ord_id.rfind("mm-", 0) == 0;
        });
    };
    EXPECT_TRUE(none_of_ours(bids)) << "no bid of ours is left resting";
    EXPECT_TRUE(none_of_ours(asks)) << "and no ask";
}

TEST(Trader, AnObserverCannotArmAnythingAndSendsNothing) {
    Rig rig(/*allow_orders=*/false);
    ASSERT_EQ(rig.trader->start(), "") << rig.logs();
    const auto id = rig.trader->api().create(rig.spec()).id;
    ASSERT_FALSE(id.empty());
    ASSERT_TRUE(rig.trader->api().start(id).ok());

    const auto armed = rig.trader->api().arm(id);
    EXPECT_FALSE(armed.ok());
    EXPECT_NE(armed.error.find("--allow-orders"), std::string::npos);

    // It still reasons about the book: a dry run is useful precisely because
    // it says what it would have done.
    ASSERT_TRUE(eventually(*rig.trader, [&] {
        return rig.trader->api().snapshot().strategies.front().status.passes > 0;
    })) << rig.logs();
    const auto status = rig.trader->api().snapshot().strategies.front().status;
    EXPECT_EQ(status.placed, 0u);
    // The simulator seeds a book of its own, so the test is that none of the
    // resting orders are ours.
    const auto bids = rig.oe_sim->book(kSym, cpm::oms::Side::Buy);
    EXPECT_TRUE(std::none_of(bids.begin(), bids.end(), [](const auto& order) {
        return order.cl_ord_id.rfind("mm-", 0) == 0;
    }));
}

TEST(Trader, AStaleFeedHaltsEverythingAndTheDaemonStaysUpToSayWhy) {
    // Amendment 4, and the gap crypto_mm had: it never wired up FeedMonitor,
    // so a feed that went silent stopped nothing.
    Rig rig;
    ASSERT_EQ(rig.trader->start(), "") << rig.logs();
    const auto id = rig.trader->api().create(rig.spec()).id;
    ASSERT_FALSE(id.empty());
    ASSERT_TRUE(rig.trader->api().start(id).ok());
    ASSERT_TRUE(rig.trader->api().arm(id).ok());

    // The simulator is told to go quiet 2 s after logon by stale_after.
    rig.md_sim->stop();

    ASSERT_TRUE(eventually(*rig.trader, [&] { return rig.trader->halted(); }, 20s)) << rig.logs();

    const auto snapshot = rig.trader->api().snapshot();
    EXPECT_EQ(snapshot.state, DaemonState::Halted);
    EXPECT_FALSE(snapshot.halt_reason.empty());
    EXPECT_NE(snapshot.halt_reason.find("market data"), std::string::npos);
    // The strategy is halted, disarmed, and still readable: the whole point of
    // not exiting (GUI-Q4).
    ASSERT_EQ(snapshot.strategies.size(), 1u);
    EXPECT_EQ(snapshot.strategies.front().state, RunState::Halted);
    EXPECT_FALSE(snapshot.strategies.front().armed);

    // And nothing restarts.
    EXPECT_FALSE(rig.trader->api().start(id).ok());
    EXPECT_FALSE(rig.trader->api().arm(id).ok());
    EXPECT_FALSE(rig.trader->api().create(rig.spec()).ok());
}

TEST(Trader, OrdersAreRefusedOnceHalted) {
    Rig rig;
    ASSERT_EQ(rig.trader->start(), "") << rig.logs();
    const auto id = rig.trader->api().create(rig.spec()).id;
    ASSERT_TRUE(rig.trader->api().start(id).ok());
    ASSERT_TRUE(rig.trader->api().arm(id).ok());
    ASSERT_TRUE(eventually(*rig.trader, [&] {
        return rig.trader->api().snapshot().strategies.front().status.placed >= 2;
    })) << rig.logs();

    const auto placed_before = rig.trader->api().snapshot().strategies.front().status.placed;
    rig.md_sim->stop();
    ASSERT_TRUE(eventually(*rig.trader, [&] { return rig.trader->halted(); }, 20s)) << rig.logs();

    for (int i = 0; i < 8; ++i) rig.trader->tick(cpm::oms::Clock::now());
    EXPECT_EQ(rig.trader->api().snapshot().strategies.front().status.placed, placed_before)
        << "a halted strategy placed another quote";
}

TEST(Trader, StopIsIdempotentAndSavesTheBookmark) {
    Rig rig;
    ASSERT_EQ(rig.trader->start(), "") << rig.logs();
    rig.trader->stop();
    rig.trader->stop();
    SUCCEED();
}

TEST(Trader, TwoStrategiesOnOneSymbolAreRefusedByTheDaemonToo) {
    Rig rig;
    ASSERT_EQ(rig.trader->start(), "") << rig.logs();
    ASSERT_TRUE(rig.trader->api().create(rig.spec()).ok());
    const auto second = rig.trader->api().create(rig.spec());
    EXPECT_FALSE(second.ok());
    EXPECT_NE(second.error.find("already has"), std::string::npos);
}

}  // namespace
