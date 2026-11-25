// The daemon against both simulators: one session pair, a strategy created and
// armed through the control API, the fail-stop halt that crypto_mm never had,
// and the clean shutdown that leaves nothing resting.
#include "cpm/trader/trader.hpp"

#include "../src/fill_recovery.hpp"

#include "cpm/md/sim/simulator.hpp"
#include "cpm/oms/sim/simulator.hpp"
#include "cpm/strategy/crypto_mm.hpp"

#include <gtest/gtest.h>

#include <tuple>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <fstream>
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
TEST(FillRecovery, ReadsAllPagesForOneInstrumentAndSubaccount) {
    cpm::md::RestConfig config;
    config.url = "https://api.cpm-sandbox.fairx.net/api/v1";
    config.api_key = "test";
    config.passphrase = "test";
    config.secret_b64 = "dGVzdA==";
    int fill_pages = 0;
    const auto fetch = [&](const cpm::HttpsRequest& request) {
        if (request.path == "/api/v1/instruments/CURRENT") {
            return cpm::HttpsResponse{200, R"({"ticker":"CURRENT","id":"42","status":"OPEN"})", {}};
        }
        EXPECT_TRUE(request.query.starts_with("instrument_id=42&sub_account=desk%20one"));
        if (request.path == "/api/orders") return cpm::HttpsResponse{200, R"({"data":[]})", {}};
        if (request.path == "/api/positions") {
            return cpm::HttpsResponse{200, R"({"data":[{"ticker":"CURRENT","instrument_id":"42",)"
                R"("sub_account":"desk one","quantity":"-2.00"}]})", {}};
        }

        EXPECT_EQ(request.path, "/api/fills");
        ++fill_pages;
        if (fill_pages == 1) {
            return cpm::HttpsResponse{200, R"({"data":[{"ticker":"CURRENT","instrument_id":"42",)"
                R"("sub_account":"desk one","trade_id":"2","order_id":"9","side":"SELL",)"
                R"("role":"TAKER","price":"0.550000","quantity":"1.00","fee":"0.002000",)"
                R"("timestamp":"2026-11-27T17:25:45.678Z"}],"cursor":"next+="})", {}};
        }
        EXPECT_TRUE(request.query.ends_with("&cursor=next%2B%3D"));
        return cpm::HttpsResponse{200, R"({"data":[{"ticker":"CURRENT","instrument_id":"42",)"
            R"("sub_account":"desk one","trade_id":"1","order_id":"8","side":"SELL",)"
            R"("role":"MAKER","price":"0.600000","quantity":"1.00","fee":"-0.001000",)"
            R"("timestamp":"2026-11-27T17:25:44Z"}]})", {}};
    };
    const auto result = cpm::trader::fetch_fill_history(config, fetch, "CURRENT", "desk one");
    ASSERT_TRUE(result.error.empty()) << result.error;
    EXPECT_EQ(fill_pages, 2);
    EXPECT_EQ(result.net, -200);
    ASSERT_EQ(result.fills.size(), 2u);
    EXPECT_EQ(result.fills.front().fill->trade_match_id, "1");
    EXPECT_EQ(result.fills.front().fill->fee, -1000);
    EXPECT_EQ(result.fills.back().fill->price, 550);
}

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
                 std::optional<int> quiet_after = std::nullopt,
                 cpm::md::HttpsFetch catalog = {}) {
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
        if (catalog) {
            // A catalog this test answers for, so a series resolution - and
            // the price grid the venue reports with it - needs no network.
            options.rest.api_key = "rest-key";
            options.rest.passphrase = "rest-pass";
            options.rest.secret_b64 = "cmVzdC1zZWNyZXQ=";
            options.rest.url = "https://rest.invalid";
            options.rest_fetch = std::move(catalog);
        }
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

// A quiet market nobody is trading does not stop the daemon.
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

TEST(Trader, StaleBookStopsItsStrategyAndKeepsBothFixSessionsOpen) {
    Rig rig(true, std::nullopt, {kSym}, 2);
    ASSERT_EQ(rig.trader->start(), "") << rig.logs();
    const auto created = rig.trader->api().create(rig.spec());
    ASSERT_TRUE(created.ok()) << created.error;
    ASSERT_TRUE(rig.trader->api().start(created.id).ok());
    ASSERT_TRUE(rig.trader->api().arm(created.id).ok());
    ASSERT_TRUE(eventually(*rig.trader, [&] {
        const auto book = rig.trader->api().snapshot().books;
        return !book.empty() && book.front().valid;
    })) << rig.logs();

    ASSERT_TRUE(eventually(*rig.trader, [&] {
        return rig.trader->api().snapshot().strategies.front().state == RunState::Stopped;
    })) << rig.logs();
    const auto snapshot = rig.trader->api().snapshot();
    EXPECT_EQ(snapshot.state, DaemonState::Live);
    EXPECT_TRUE(snapshot.market_data.logged_on);
    EXPECT_TRUE(snapshot.order_entry.logged_on);
    EXPECT_FALSE(snapshot.strategies.front().armed);
    EXPECT_NE(snapshot.strategies.front().note.find("stale feed on " + kSym), std::string::npos);
    EXPECT_FALSE(rig.trader->api().start(created.id).ok());
    EXPECT_FALSE(rig.trader->api().arm(created.id).ok());

    rig.md_sim->stop();
    ASSERT_TRUE(eventually(*rig.trader, [&] { return rig.trader->halted(); }, 20s)) << rig.logs();
    EXPECT_NE(rig.trader->api().snapshot().halt_reason.find("disconnect"), std::string::npos);
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

// This build prices on one grid, the venue's own TAPERED_DECI_CENT. The
// catalog is still read, so an instrument the venue says trades on another
// structure is reported rather than quoted onto the wrong rungs.
TEST(Trader, SaysSoWhenTheVenueReportsAGridThisBuildCannotPrice) {
    const auto catalog = [](const cpm::HttpsRequest&) {
        return cpm::HttpsResponse{200,
                                  R"({"data":[{"ticker":"SIMGAME-26NOV01HOMAWY-HOM",)"
                                  R"("series_ticker":"SIMGAME","status":"OPEN",)"
                                  R"("price_level_structure":"LINEAR_CENT"}]})",
                                  ""};
    };
    Rig rig(true, std::nullopt, {"SIMGAME"}, std::nullopt, catalog);
    EXPECT_EQ(rig.trader->start(), "") << rig.logs();
    EXPECT_NE(rig.logs().find("this build prices only on TAPERED_DECI_CENT"), std::string::npos)
        << rig.logs();

    // And the ladder is the one grid, whatever the catalog said.
    const auto axis = rig.trader->api().axis(kSym);
    EXPECT_EQ(axis.structure, "TAPERED_DECI_CENT");
    EXPECT_EQ(axis.prices.size(), 281u);
}

TEST(Trader, SaysNothingWhenTheVenueAgrees) {
    const auto catalog = [](const cpm::HttpsRequest&) {
        return cpm::HttpsResponse{200,
                                  R"({"data":[{"ticker":"SIMGAME-26NOV01HOMAWY-HOM",)"
                                  R"("series_ticker":"SIMGAME","status":"OPEN",)"
                                  R"("price_level_structure":"TAPERED_DECI_CENT"}]})",
                                  ""};
    };
    Rig rig(true, std::nullopt, {"SIMGAME"}, std::nullopt, catalog);
    EXPECT_EQ(rig.trader->start(), "") << rig.logs();
    EXPECT_EQ(rig.logs().find("prices only on"), std::string::npos) << rig.logs();
    EXPECT_EQ(rig.trader->api().axis(kSym).prices.size(), 281u);
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

// An order's whole life in the strategy's own log: the send is the
// reconciler's line, and these are the venue's answers to it. Before this,
// strategy.log said an order had been sent and then never mentioned it again
// - so a quote that was acknowledged 250ms later and taken 56ms after that
// read exactly like one that rested for a quarter of a second by choice
// (2026-11-19 07:01).
TEST(Trader, AnOrdersWholeLifeIsInTheStrategyLog) {
    Rig rig;
    ASSERT_EQ(rig.trader->start(), "") << rig.logs();
    const auto id = rig.trader->api().create(rig.spec()).id;
    ASSERT_FALSE(id.empty());
    ASSERT_TRUE(rig.trader->api().start(id).ok());
    ASSERT_TRUE(rig.trader->api().arm(id).ok());

    // Sent, then acknowledged. "added" is the line that says it is really on
    // the book - and therefore that a cancel for it can be sent at all.
    ASSERT_TRUE(eventually(*rig.trader, [&] {
        return rig.logs().find("] added: buy ") != std::string::npos;
    })) << rig.logs();

    const auto logged = rig.logs();
    const auto at = logged.find("] added: buy ");
    const auto added = logged.substr(logged.rfind('\n', at) + 1,
                                     logged.find('\n', at) - logged.rfind('\n', at) - 1);
    EXPECT_NE(added.find("[" + id + " " + kSym + "] added: buy "), std::string::npos) << added;
    // What the venue took to answer, which is the window in which the quote
    // was live and could not yet be cancelled.
    EXPECT_NE(added.find(", acked in "), std::string::npos) << added;
    // The venue's own handle for it, which appears nowhere else in this file.
    EXPECT_NE(added.find(", oid "), std::string::npos) << added;
    // The ClOrdID last, where every line in the family puts it.
    EXPECT_NE(added.find(" (mm-"), std::string::npos) << added;

    // The send itself is still the reconciler's line, and still there.
    EXPECT_NE(logged.find("] quoting buy "), std::string::npos) << logged;

    // Stopping takes the quotes off, and the cancel is now answered in the
    // log as well as requested in it. Both halves: before this only the
    // request was written down, so a cancel that landed and one still in
    // flight looked the same.
    ASSERT_TRUE(rig.trader->api().stop(id).ok());
    ASSERT_TRUE(eventually(*rig.trader, [&] {
        return rig.logs().find("] deleted: ") != std::string::npos;
    })) << rig.logs();

    const auto after = rig.logs();
    // A stop withdraws both sides at once, so the request is the reconciler's
    // "withdrawing both quotes" and not the per-side line, which only appears
    // when a running strategy drops one side and keeps the other.
    EXPECT_NE(after.find("] withdrawing both quotes: "), std::string::npos) << "the request";
    const auto gone = after.find("] deleted: ");
    const auto deleted = after.substr(after.rfind('\n', gone) + 1,
                                      after.find('\n', gone) - after.rfind('\n', gone) - 1);
    // Our cancel, not the venue's, so it carries the round trip and the id of
    // the cancel that did it rather than an ExecRestatementReason.
    EXPECT_NE(deleted.find(", cancelled in "), std::string::npos) << deleted;
    EXPECT_NE(deleted.find(" by mm-"), std::string::npos) << deleted;
    EXPECT_EQ(deleted.find("by the venue"), std::string::npos) << deleted;
}

// A fill belongs in the strategy's own log, not only in fills.csv. The CSV is
// the shape a dataframe wants and keeps every id; this is the line an operator
// reads in the same file as the reasoning that led to it, so "why did it quote
// there" and "did it work" are one scroll apart rather than two files.
TEST(Trader, AFillIsLoggedToTheStrategyLogAndNotOnlyToFillsCsv) {
    Rig rig;
    ASSERT_EQ(rig.trader->start(), "") << rig.logs();
    const auto id = rig.trader->api().create(rig.spec()).id;
    ASSERT_FALSE(id.empty());
    ASSERT_TRUE(rig.trader->api().start(id).ok());
    ASSERT_TRUE(rig.trader->api().arm(id).ok());

    // Our own bid has to be resting before anything can hit it.
    ASSERT_TRUE(eventually(*rig.trader, [&] {
        const auto bids = rig.oe_sim->book(kSym, cpm::oms::Side::Buy);
        return std::any_of(bids.begin(), bids.end(), [](const auto& order) {
            return order.cl_ord_id.rfind("mm-", 0) == 0;
        });
    })) << rig.logs();

    // Somebody sells into it: price 1 crosses every bid there is, and enough
    // quantity to sweep the simulator's own seeded liquidity (600, at 0.45
    // and below) as well, because the engine fills the best bid first and
    // the strategy's may not be it. A maker fill is the only way a position
    // exists in this simulator at all.
    const auto bids = rig.oe_sim->book(kSym, cpm::oms::Side::Buy);
    const auto ours = std::find_if(bids.begin(), bids.end(), [](const auto& order) {
        return order.cl_ord_id.rfind("mm-", 0) == 0;
    });
    ASSERT_NE(ours, bids.end());
    ASSERT_GE(rig.oe_sim->trade(kSym, cpm::oms::Side::Sell, 1000, 1), ours->quantity)
        << "the taker never reached our bid: " << rig.logs();

    ASSERT_TRUE(eventually(*rig.trader, [&] {
        return rig.logs().find("] fill: buy ") != std::string::npos;
    })) << rig.logs();

    const auto logged = rig.logs();
    const auto at = logged.find("] fill: buy ");
    const auto line = logged.substr(logged.rfind('\n', at) + 1,
                                    logged.find('\n', at) - logged.rfind('\n', at) - 1);
    // Tagged like every other line of a strategy's log, so the file reads as
    // one timeline: the strategy that did it, then the instrument.
    EXPECT_NE(line.find("[" + id + " " + kSym + "] fill: buy "), std::string::npos) << line;
    // Resting, not crossing: it was our quote that was taken.
    EXPECT_NE(line.find(" maker"), std::string::npos) << line;
    // How long the quote waited, and where the fill left the position - the
    // two things the CSV row has that a reader of the narrative would
    // otherwise have to go and join to.
    EXPECT_NE(line.find(", rested "), std::string::npos) << line;
    EXPECT_NE(line.find(", net "), std::string::npos) << line;
    EXPECT_NE(line.find(", cash "), std::string::npos) << line;
    // And the ClOrdID, which is what joins it to orders.csv and to wire.log.
    EXPECT_NE(line.find(" (mm-"), std::string::npos) << line;
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

TEST(Trader, AMarketDataDisconnectHaltsEverythingAndTheDaemonStaysUpToSayWhy) {
    Rig rig;
    ASSERT_EQ(rig.trader->start(), "") << rig.logs();
    const auto id = rig.trader->api().create(rig.spec()).id;
    ASSERT_FALSE(id.empty());
    ASSERT_TRUE(rig.trader->api().start(id).ok());
    ASSERT_TRUE(rig.trader->api().arm(id).ok());

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

// A ClOrdID is unique across a restart because it names the run, not because
// a counter was saved to disk before each send. The bookmark is therefore one
// line, and allocating an id touches no file at all.
TEST(Trader, EveryClOrdIdNamesTheRunAndTheBookmarkHoldsOnlyTheExecId) {
    Rig rig;
    ASSERT_EQ(rig.trader->start(), "") << rig.logs();
    const auto id = rig.trader->api().create(rig.spec()).id;
    ASSERT_FALSE(id.empty());
    ASSERT_TRUE(rig.trader->api().start(id).ok());
    ASSERT_TRUE(rig.trader->api().arm(id).ok());
    ASSERT_TRUE(eventually(*rig.trader, [&] {
        return rig.logs().find("] added: buy ") != std::string::npos;
    })) << rig.logs();

    const auto logged = rig.logs();
    // The run is announced once, and every quote carries its tag.
    const auto tag_at = logged.find("ClOrdID run tag ");
    ASSERT_NE(tag_at, std::string::npos) << logged;
    const auto tag = logged.substr(tag_at + std::string("ClOrdID run tag ").size(), 6);
    EXPECT_EQ(tag.size(), 6u) << tag;
    EXPECT_NE(logged.find(" (mm-" + tag + "-"), std::string::npos)
        << "a quote's id does not name the run: " << logged;

    // The counter still rises within the run, so two quotes are never the
    // same order.
    EXPECT_NE(logged.find("quoting buy "), std::string::npos) << logged;
    EXPECT_NE(logged.find("quoting sell "), std::string::npos) << logged;
    const auto buy_at = logged.find(" as mm-" + tag + "-");
    const auto sell_at = logged.rfind(" as mm-" + tag + "-");
    ASSERT_NE(buy_at, std::string::npos) << logged;
    EXPECT_NE(buy_at, sell_at) << "both sides were quoted under one id";

    // And the file on disk is the ExecID alone: nothing about the counter
    // survives the run, because nothing needs to.
    rig.trader->stop();
    std::ifstream in(Rig::bookmark_path());
    ASSERT_TRUE(in.is_open()) << "no bookmark at " << Rig::bookmark_path();
    const std::string contents((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    EXPECT_EQ(contents.find("cl_ord_counter"), std::string::npos) << contents;
    EXPECT_EQ(contents.rfind("last_exec_id=", 0), 0u) << contents;
    EXPECT_EQ(std::count(contents.begin(), contents.end(), '\n'), 1) << contents;
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
