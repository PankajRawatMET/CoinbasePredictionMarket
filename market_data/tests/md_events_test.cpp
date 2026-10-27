// md_client against md_sim over loopback, asserting on the DECODED events
// rather than the raw log: every channel of design §7 turned into the plain
// structs of cpm/md/events.hpp, with no decode errors anywhere.
#include "cpm/md/market_state.hpp"
#include "cpm/md/md_client.hpp"
#include "cpm/md/sim/simulator.hpp"

#include "test_util.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using cpm::md::Aggressor;
using cpm::md::BookAction;
using cpm::md::Channel;
using cpm::md::LifecycleStatus;
using cpm::md::test::eventually;
using cpm::md::test::free_port;
using cpm::md::test::SharedSink;

const cpm::Credentials kCreds{"sim-test-key", "sim-pass", "c2ltLXRlc3Qtc2VjcmV0LW5vdC1yZWFs"};
const std::string kHom = "SIMGAME-26NOV01HOMAWY-HOM";
const std::string kHomId = "700000000000000001";

// Everything the client decoded, collected off the QuickFIX thread.
class Events {
public:
    cpm::md::MdCallbacks callbacks() {
        cpm::md::MdCallbacks c;
        c.book_snapshot = [this](const cpm::md::BookSnapshot& e) { push(snapshots_, e); };
        c.book_update = [this](const cpm::md::BookUpdate& e) { push(updates_, e); };
        c.trade = [this](const cpm::md::Trade& e) { push(trades_, e); };
        c.volume = [this](const cpm::md::Volume& e) { push(volumes_, e); };
        c.open_interest = [this](const cpm::md::OpenInterest& e) { push(open_interest_, e); };
        c.lifecycle = [this](const cpm::md::InstrumentLifecycle& e) { push(lifecycle_, e); };
        c.reject = [this](const cpm::md::MdReject& e) { push(rejects_, e); };
        c.decode_error = [this](const cpm::md::DecodeError& e) { push(errors_, e); };
        return c;
    }

    std::vector<cpm::md::BookSnapshot> snapshots() const { return copy(snapshots_); }
    std::vector<cpm::md::BookUpdate> updates() const { return copy(updates_); }
    std::vector<cpm::md::Trade> trades() const { return copy(trades_); }
    std::vector<cpm::md::Volume> volumes() const { return copy(volumes_); }
    std::vector<cpm::md::OpenInterest> open_interest() const { return copy(open_interest_); }
    std::vector<cpm::md::InstrumentLifecycle> lifecycle() const { return copy(lifecycle_); }
    std::vector<cpm::md::MdReject> rejects() const { return copy(rejects_); }
    std::vector<cpm::md::DecodeError> errors() const { return copy(errors_); }

    // Fails with the first decode error's text, which names the offending field.
    std::string error_text() const {
        const auto all = errors();
        return all.empty() ? std::string() : all.front().reason + " in " + all.front().raw;
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
    std::vector<cpm::md::BookSnapshot> snapshots_;
    std::vector<cpm::md::BookUpdate> updates_;
    std::vector<cpm::md::Trade> trades_;
    std::vector<cpm::md::Volume> volumes_;
    std::vector<cpm::md::OpenInterest> open_interest_;
    std::vector<cpm::md::InstrumentLifecycle> lifecycle_;
    std::vector<cpm::md::MdReject> rejects_;
    std::vector<cpm::md::DecodeError> errors_;
};

struct Rig {
    explicit Rig(cpm::md::sim::SimOptions sim_options = {})
        : port(free_port()), sim_logger(sim_log), client_logger(client_log) {
        sim_options.credentials = kCreds;
        sim_options.port = port;
        sim_options.dict_dir = CPM_DICT_DIR;
        if (sim_options.events_per_sec == 5.0) sim_options.events_per_sec = 50;
        sim = std::make_unique<cpm::md::sim::Simulator>(std::move(sim_options), sim_logger);
        sim->start();

        cpm::md::MdClientOptions o;
        o.config.credentials = kCreds;
        o.config.endpoint = {"127.0.0.1", port, false, ""};
        o.config.heartbeat_secs = 30;
        o.dict_dir = CPM_DICT_DIR;
        o.reconnect_interval_secs = 1;
        client = std::make_unique<cpm::md::MdClient>(std::move(o), client_logger);
        client->on_market_data(events.callbacks());
        client->start();
    }
    ~Rig() {
        client->stop();
        sim->stop();
    }

    std::uint16_t port;
    SharedSink sim_log;
    SharedSink client_log;
    cpm::Logger sim_logger;
    cpm::Logger client_logger;
    Events events;
    std::unique_ptr<cpm::md::sim::Simulator> sim;
    std::unique_ptr<cpm::md::MdClient> client;
};

TEST(MdEvents, BookSnapshotDecodesBestFirstAndUncrossed) {
    Rig rig;
    ASSERT_TRUE(rig.client->wait_logged_on(5s));
    EXPECT_EQ(rig.client->subscribe({Channel::Book, {kHom}, 10}), "book-1");
    ASSERT_TRUE(eventually([&] { return !rig.events.snapshots().empty(); }, 5s)) << rig.client_log.text();

    const auto snapshot = rig.events.snapshots().front();
    EXPECT_EQ(snapshot.md_req_id, "book-1");
    EXPECT_EQ(snapshot.symbol, kHom);
    EXPECT_EQ(snapshot.security_id, kHomId);
    EXPECT_FALSE(snapshot.yes.empty());
    EXPECT_FALSE(snapshot.no.empty());
    EXPECT_LE(snapshot.yes.size(), 10u);  // spec §4: 10 levels per side
    EXPECT_LE(snapshot.no.size(), 10u);
    for (const auto* side : {&snapshot.yes, &snapshot.no}) {
        EXPECT_TRUE(std::is_sorted(side->begin(), side->end(),
                                   [](const auto& a, const auto& b) { return a.price > b.price; }));
        for (const auto& level : *side) {
            EXPECT_GT(level.price, 0);
            EXPECT_LT(level.price, cpm::kPriceOne);
            EXPECT_GT(level.qty, 0);
        }
    }
    // Q9: a book that is neither crossed nor locked.
    EXPECT_LT(snapshot.yes.front().price + snapshot.no.front().price, cpm::kPriceOne);
    EXPECT_GT(snapshot.transact_time.time_since_epoch().count(), 0);
    EXPECT_TRUE(rig.events.errors().empty()) << rig.events.error_text();
}

TEST(MdEvents, BookUpdatesCarryAbsoluteSizesAndDeletes) {
    Rig rig;
    ASSERT_TRUE(rig.client->wait_logged_on(5s));
    rig.client->subscribe({Channel::Book, {kHom}, 10});
    ASSERT_TRUE(eventually([&] { return rig.events.updates().size() >= 10; }, 10s)) << rig.client_log.text();

    int deletes = 0;
    int sets = 0;
    for (const auto& update : rig.events.updates()) {
        EXPECT_EQ(update.md_req_id, "book-1");
        EXPECT_EQ(update.symbol, kHom);
        EXPECT_EQ(update.security_id, kHomId);
        ASSERT_FALSE(update.changes.empty());
        for (const auto& change : update.changes) {
            EXPECT_GT(change.price, 0);
            EXPECT_LT(change.price, cpm::kPriceOne);
            if (change.action == BookAction::Delete) {
                ++deletes;
                EXPECT_EQ(change.qty, 0);
                EXPECT_EQ(change.level, 0);  // a delete carries no 1023 (O3)
            } else {
                ++sets;
                EXPECT_GT(change.qty, 0);
                EXPECT_GE(change.level, 1);
            }
        }
    }
    EXPECT_GT(sets, 0);
    EXPECT_GT(deletes, 0);
    EXPECT_TRUE(rig.events.errors().empty()) << rig.events.error_text();
}

TEST(MdEvents, L1BookStaysOneLevelPerSide) {
    Rig rig;
    ASSERT_TRUE(rig.client->wait_logged_on(5s));
    rig.client->subscribe({Channel::Book, {kHom}, 1});
    ASSERT_TRUE(eventually([&] { return !rig.events.snapshots().empty() && rig.events.updates().size() >= 5; }, 10s))
        << rig.client_log.text();

    const auto snapshot = rig.events.snapshots().front();
    EXPECT_EQ(snapshot.yes.size(), 1u);
    EXPECT_EQ(snapshot.no.size(), 1u);
    for (const auto& update : rig.events.updates()) {
        for (const auto& change : update.changes) {
            if (change.action != BookAction::Delete) {
                EXPECT_EQ(change.level, 1);
            }
        }
    }
    EXPECT_TRUE(rig.events.errors().empty()) << rig.events.error_text();
}

TEST(MdEvents, TradesDecodeWithAggressorAndEntryId) {
    Rig rig;
    ASSERT_TRUE(rig.client->wait_logged_on(5s));
    rig.client->subscribe({Channel::Trades, {}, 0});  // all instruments
    ASSERT_TRUE(eventually([&] { return rig.events.trades().size() >= 3; }, 10s)) << rig.client_log.text();

    std::vector<std::string> ids;
    for (const auto& trade : rig.events.trades()) {
        EXPECT_EQ(trade.md_req_id, "trades-1");
        EXPECT_FALSE(trade.symbol.empty());
        EXPECT_FALSE(trade.security_id.empty());
        EXPECT_GT(trade.yes_price, 0);
        EXPECT_LT(trade.yes_price, cpm::kPriceOne);
        EXPECT_GT(trade.qty, 0);
        EXPECT_NE(trade.aggressor, Aggressor::None);  // the simulator always has a taker
        EXPECT_FALSE(trade.entry_id.empty());
        ids.push_back(trade.entry_id);
    }
    // MDEntryID(278) is unique per execution, and is what Stage 4 dedupes on.
    std::sort(ids.begin(), ids.end());
    EXPECT_EQ(std::adjacent_find(ids.begin(), ids.end()), ids.end());
    EXPECT_TRUE(rig.events.errors().empty()) << rig.events.error_text();
}

TEST(MdEvents, VolumeAndOpenInterestSnapshotThenUpdates) {
    Rig rig;
    ASSERT_TRUE(rig.client->wait_logged_on(5s));
    rig.client->subscribe({Channel::VolumeOi, {}, 0});
    ASSERT_TRUE(eventually(
        [&] {
            const auto volumes = rig.events.volumes();
            return std::count_if(volumes.begin(), volumes.end(), [](const auto& v) { return !v.snapshot; }) >= 2;
        },
        10s))
        << rig.client_log.text();

    const auto volumes = rig.events.volumes();
    const auto open_interest = rig.events.open_interest();
    // Spec §4.2: a W snapshot per instrument first, then X updates.
    EXPECT_EQ(std::count_if(volumes.begin(), volumes.end(), [](const auto& v) { return v.snapshot; }), 3);
    EXPECT_EQ(std::count_if(open_interest.begin(), open_interest.end(), [](const auto& o) { return o.snapshot; }), 3);
    EXPECT_TRUE(volumes.front().snapshot);
    EXPECT_EQ(volumes.front().md_req_id, "volume-oi-1");
    for (const auto& volume : volumes) {
        EXPECT_FALSE(volume.symbol.empty());
        EXPECT_GE(volume.contracts, 0);  // Q15: zero is not an error
    }
    // Volume only ever grows, per instrument.
    for (const auto& symbol : {kHom, std::string("SIMGAME-26NOV01HOMAWY-AWY")}) {
        cpm::Qty last = -1;
        for (const auto& volume : volumes) {
            if (volume.symbol != symbol) continue;
            EXPECT_GE(volume.contracts, last) << symbol;
            last = volume.contracts;
        }
    }
    EXPECT_TRUE(rig.events.errors().empty()) << rig.events.error_text();
}

TEST(MdEvents, LifecycleRollDecodesInOrder) {
    cpm::md::sim::SimOptions options;
    options.roll_secs = 1;
    options.settle_delay = 300ms;
    Rig rig(options);
    ASSERT_TRUE(rig.client->wait_logged_on(5s));
    rig.client->subscribe({Channel::Lifecycle, {}, 0});
    ASSERT_TRUE(eventually(
        [&] {
            const auto events = rig.events.lifecycle();
            return std::any_of(events.begin(), events.end(),
                               [](const auto& e) { return e.status == LifecycleStatus::Settled; });
        },
        10s))
        << rig.client_log.text();

    const auto events = rig.events.lifecycle();
    ASSERT_GE(events.size(), 5u);
    const auto first = events[0].symbol;
    EXPECT_EQ(events[0].status, LifecycleStatus::Closed);
    EXPECT_EQ(events[0].status_code, 18);
    EXPECT_EQ(events[1].symbol, first);
    EXPECT_EQ(events[1].status, LifecycleStatus::Determined);
    EXPECT_NE(events[2].symbol, first);  // the next window's instrument
    EXPECT_EQ(events[2].status, LifecycleStatus::Listed);
    EXPECT_EQ(events[3].status, LifecycleStatus::Open);
    const auto settled = std::find_if(events.begin(), events.end(), [&](const auto& e) {
        return e.symbol == first && e.status == LifecycleStatus::Settled;
    });
    ASSERT_NE(settled, events.end());
    EXPECT_FALSE(settled->security_id.empty());
    EXPECT_TRUE(settled->md_req_id.empty());  // no 324, as on the sandbox
    EXPECT_TRUE(rig.events.errors().empty()) << rig.events.error_text();
}

TEST(MdEvents, RejectsReachTheCallback) {
    Rig rig;
    ASSERT_TRUE(rig.client->wait_logged_on(5s));
    rig.client->subscribe({Channel::Book, {"NOPE-0"}, 10});
    ASSERT_TRUE(eventually([&] { return !rig.events.rejects().empty(); }, 5s)) << rig.client_log.text();

    const auto reject = rig.events.rejects().front();
    EXPECT_EQ(reject.kind, cpm::md::MdReject::Kind::MarketDataRequest);
    EXPECT_EQ(reject.md_req_id, "book-1");
    EXPECT_EQ(reject.reason_code, "0");  // 281=0 unknown symbol
    EXPECT_EQ(reject.text, "unknown symbol=NOPE-0");
    EXPECT_TRUE(rig.events.snapshots().empty());
    EXPECT_TRUE(rig.events.errors().empty()) << rig.events.error_text();
}

TEST(MdEvents, EveryChannelAtOnceDecodesWithoutErrors) {
    cpm::md::sim::SimOptions options;
    options.roll_secs = 1;
    options.settle_delay = 300ms;
    Rig rig(options);
    ASSERT_TRUE(rig.client->wait_logged_on(5s));
    rig.client->subscribe({Channel::Book, {kHom}, 10});
    rig.client->subscribe({Channel::Trades, {}, 0});
    rig.client->subscribe({Channel::VolumeOi, {}, 0});
    rig.client->subscribe({Channel::Lifecycle, {}, 0});

    ASSERT_TRUE(eventually(
        [&] {
            return !rig.events.snapshots().empty() && !rig.events.updates().empty() && !rig.events.trades().empty() &&
                   !rig.events.volumes().empty() && !rig.events.open_interest().empty() &&
                   !rig.events.lifecycle().empty();
        },
        15s))
        << rig.client_log.text();
    EXPECT_TRUE(rig.events.rejects().empty());
    EXPECT_TRUE(rig.events.errors().empty()) << rig.events.error_text();
}


// --- Stage 4: the book and instrument state built from the feed -------------

// Holds a MarketState fed by the client, with anomalies collected.
struct StateRig : Rig {
    explicit StateRig(cpm::md::sim::SimOptions options = {}) : Rig(std::move(options)) {
        state.on_anomaly([this](const cpm::md::BookAnomaly& anomaly) {
            std::lock_guard lock(mutex);
            anomalies.push_back(std::string(to_string(anomaly.kind)) + ": " + anomaly.detail);
        });
        client->on_market_data(state.callbacks());
    }

    // The callbacks capture `state`, which is destroyed before ~Rig() would
    // stop the feed, so the feed has to stop here first.
    ~StateRig() {
        client->stop();
        sim->stop();
    }

    std::string anomaly_text() const {
        std::lock_guard lock(mutex);
        return anomalies.empty() ? std::string() : anomalies.front();
    }
    std::size_t anomaly_count() const {
        std::lock_guard lock(mutex);
        return anomalies.size();
    }

    cpm::md::MarketState state;
    mutable std::mutex mutex;
    std::vector<std::string> anomalies;
};

TEST(MdBook, RebuildsTheSimulatorsBookWithoutAnomalies) {
    StateRig rig;
    ASSERT_TRUE(rig.client->wait_logged_on(5s));
    rig.client->subscribe({Channel::Book, {kHom}, 10});
    ASSERT_TRUE(eventually(
        [&] {
            const auto view = rig.state.instrument(kHom);
            return view && view->book_counters.updates >= 20;
        },
        10s))
        << rig.client_log.text();

    const auto view = rig.state.instrument(kHom);
    ASSERT_TRUE(view.has_value());
    EXPECT_TRUE(view->book_valid);
    EXPECT_EQ(view->book_md_req_id, "book-1");
    EXPECT_EQ(view->security_id, kHomId);
    EXPECT_EQ(rig.anomaly_count(), 0u) << rig.anomaly_text();
    EXPECT_EQ(view->book_counters.anomalies, 0u);
    // The simulator sends 1023 on every set, so every one was cross-checked.
    EXPECT_GT(view->book_counters.price_levels_checked, 0u);
    EXPECT_EQ(view->book_counters.count(cpm::md::BookAnomaly::Kind::PriceLevelMismatch), 0u);

    EXPECT_FALSE(view->crossed_or_locked);
    EXPECT_FALSE(view->yes.empty());
    EXPECT_FALSE(view->no.empty());
    EXPECT_LE(view->yes.size(), cpm::md::OrderBook::kMaxLevels);
    for (const auto* side : {&view->yes, &view->no}) {
        EXPECT_TRUE(std::is_sorted(side->begin(), side->end(),
                                   [](const auto& a, const auto& b) { return a.price > b.price; }));
    }
    ASSERT_TRUE(view->implied_yes_ask.has_value());
    EXPECT_EQ(*view->implied_yes_ask, cpm::kPriceOne - view->no.front().price);
}

// O7: two book subscriptions on one instrument deliver every change twice.
TEST(MdBook, OnlyTheFirstBookSubscriptionOwnsTheSymbol) {
    StateRig rig;
    ASSERT_TRUE(rig.client->wait_logged_on(5s));
    rig.client->subscribe({Channel::Book, {kHom}, 10});
    rig.client->subscribe({Channel::Book, {kHom}, 1});
    ASSERT_TRUE(eventually([&] { return rig.state.ignored_foreign_updates() >= 5; }, 10s)) << rig.client_log.text();

    const auto view = rig.state.instrument(kHom);
    EXPECT_EQ(view->book_md_req_id, "book-1") << "the L2 subscription snapshotted first";
    EXPECT_EQ(rig.state.ignored_foreign_snapshots(), 1u);
    EXPECT_GT(view->yes.size(), 1u) << "an L1 copy would have shrunk the book to its top level";
    EXPECT_EQ(rig.anomaly_count(), 0u) << rig.anomaly_text();
}

TEST(MdBook, TheSameTradeUnderTwoSubscriptionsIsCountedOnce) {
    StateRig rig;
    ASSERT_TRUE(rig.client->wait_logged_on(5s));
    rig.client->subscribe({Channel::Trades, {kHom}, 0});
    rig.client->subscribe({Channel::Trades, {}, 0});  // all instruments
    ASSERT_TRUE(eventually(
        [&] {
            const auto view = rig.state.instrument(kHom);
            return view && view->duplicate_trades >= 3;
        },
        10s))
        << rig.client_log.text();

    const auto view = rig.state.instrument(kHom);
    EXPECT_GT(view->trades, 0u);
    EXPECT_GT(view->duplicate_trades, 0u);
    EXPECT_EQ(view->trades_without_id, 0u);
    ASSERT_TRUE(view->last_trade.has_value());
    EXPECT_NE(view->last_trade->aggressor, Aggressor::None);
}

TEST(MdBook, LifecycleAndStatsReachTheInstrumentState) {
    cpm::md::sim::SimOptions options;
    options.roll_secs = 1;
    options.settle_delay = 300ms;
    StateRig rig(options);
    ASSERT_TRUE(rig.client->wait_logged_on(5s));
    rig.client->subscribe({Channel::Lifecycle, {}, 0});
    rig.client->subscribe({Channel::VolumeOi, {}, 0});
    ASSERT_TRUE(eventually(
        [&] {
            for (const auto& view : rig.state.instruments()) {
                if (view.status == LifecycleStatus::Settled && view.volume.has_value()) return true;
            }
            return false;
        },
        10s))
        << rig.client_log.text();

    const auto hom = rig.state.instrument(kHom);
    ASSERT_TRUE(hom.has_value());
    ASSERT_TRUE(hom->volume.has_value());
    ASSERT_TRUE(hom->open_interest.has_value());
    EXPECT_GE(*hom->volume, 0);

    // A rolled instrument ends Settled, having gone through Closed/Determined.
    const auto all = rig.state.instruments();
    const auto settled = std::find_if(all.begin(), all.end(),
                                      [](const auto& v) { return v.status == LifecycleStatus::Settled; });
    ASSERT_NE(settled, all.end());
    EXPECT_GE(settled->history.size(), 3u);
    EXPECT_EQ(settled->amendments, 0u);
    EXPECT_EQ(settled->unknown_status_events, 0u);
}

// Stage 5 will call this after a gap; the book must go and the rest must stay.
TEST(MdBook, InvalidateThenResubscribeRebuildsFromAFreshSnapshot) {
    StateRig rig;
    ASSERT_TRUE(rig.client->wait_logged_on(5s));
    const auto first = rig.client->subscribe({Channel::Book, {kHom}, 10});
    rig.client->subscribe({Channel::Trades, {kHom}, 0});
    ASSERT_TRUE(eventually(
        [&] {
            const auto view = rig.state.instrument(kHom);
            return view && view->book_valid && view->last_trade.has_value();
        },
        10s))
        << rig.client_log.text();

    rig.client->unsubscribe(first);
    rig.state.invalidate_books();
    auto view = rig.state.instrument(kHom);
    EXPECT_FALSE(view->book_valid);
    EXPECT_TRUE(view->yes.empty());
    EXPECT_TRUE(view->last_trade.has_value()) << "trades have no snapshot to rebuild from";

    const auto second = rig.client->subscribe({Channel::Book, {kHom}, 10});
    EXPECT_NE(second, first);
    ASSERT_TRUE(eventually(
        [&] {
            const auto again = rig.state.instrument(kHom);
            return again && again->book_valid;
        },
        10s))
        << rig.client_log.text();

    view = rig.state.instrument(kHom);
    EXPECT_EQ(view->book_md_req_id, second);
    EXPECT_FALSE(view->yes.empty());
    EXPECT_EQ(rig.anomaly_count(), 0u) << rig.anomaly_text();
}

}  // namespace
