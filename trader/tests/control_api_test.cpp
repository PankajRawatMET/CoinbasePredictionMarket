// The operator's surface, driven directly: no sockets, no FIX, no daemon.
// Everything the GUI will be able to do in G3 is exercised here first.
#include "cpm/trader/control_api.hpp"

#include "cpm/strategy/crypto_mm.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <sstream>

namespace {

using cpm::md::InstrumentView;
using cpm::md::LifecycleStatus;
using cpm::strategy::RunState;
using cpm::strategy::StrategyHooks;
using cpm::strategy::StrategyHost;
using cpm::strategy::StrategyRegistry;
using cpm::strategy::StrategySpec;
using cpm::trader::BookView;
using cpm::trader::ControlApi;
using cpm::trader::DaemonState;
using cpm::trader::LimitsView;
using cpm::trader::PositionView;
using cpm::trader::SessionView;
using Clock = cpm::trader::Clock;

const std::string kSym = "SIMOE-26NOV01HOMAWY-HOM";
const std::string kOther = "SIMOE-26NOV01HOMAWY-AWY";
// What the fake resolver answers for BTCD15M: a real window of that series.
const std::string kWindow = "BTCD15M-BTC26OCT260630-0";

// A host with CryptoMM registered and fakes for everything a session would
// otherwise provide, so the API can be driven with no venue at all.
struct Fixture {
    Fixture() {
        cpm::strategy::register_crypto_mm(registry);

        StrategyHooks venue;
        venue.can_submit = [this] { return allow_orders && gate_open; };
        venue.orders_permitted = [this] { return allow_orders; };
        venue.send = [this](const cpm::oms::NewOrder& order) {
            sent.push_back(order.cl_ord_id);
            return order.cl_ord_id;
        };
        venue.cancel = [this](const cpm::oms::CancelOrder& request) {
            cancelled.push_back(request.orig_cl_ord_id);
            return true;
        };
        venue.next_cl_ord_id = [this] { return "mm-" + std::to_string(++counter); };
        venue.log = [](const std::string&) {};

        StrategyHost::Feeds feeds;
        feeds.book = [this](const std::string& symbol) -> std::optional<InstrumentView> {
            if (!have_book) return std::nullopt;
            InstrumentView view;
            view.symbol = symbol;
            view.book_valid = true;
            view.status = LifecycleStatus::Open;
            view.yes.push_back(cpm::md::BookLevel{500, 500});
            view.no.push_back(cpm::md::BookLevel{400, 500});
            return view;
        };
        feeds.position = [](const std::string& symbol) {
            cpm::oms::Position position;
            position.symbol = symbol;
            return position;
        };
        host = std::make_unique<StrategyHost>(StrategyHost::Options{}, registry, std::move(venue),
                                              std::move(feeds), logger);

        ControlApi::Env env;
        env.market_data = [this] { return SessionView{md_up, md_up ? "" : "not connected"}; };
        env.order_entry = [this] { return SessionView{oe_up, oe_up ? "" : "not connected"}; };
        env.recovery = [] { return std::string("live"); };
        env.positions = [this] { return positions; };
        env.pnl = [this] { return pnl; };
        env.limits = [] {
            LimitsView limits;
            limits.allow_orders = true;
            limits.max_quantity = 1000;
            limits.max_orders_per_sec = 8;
            return limits;
        };
        env.book = [this](const std::string& symbol) -> std::optional<BookView> {
            BookView view;
            view.symbol = symbol;
            view.valid = true;
            view.best_yes_bid = 500;
            view.implied_yes_ask = 600;
            view.mid = 550;
            view.status = "open";
            return view;
        };
        env.resolve = [this](const std::string& series) {
            ControlApi::Env::Resolved out;
            resolved.push_back(series);
            if (!resolve_error.empty()) {
                out.error = resolve_error;
                return out;
            }
            out.symbol = kWindow;
            return out;
        };
        env.subscribe = [this](const std::string& symbol) {
            if (!subscribe_ok) return "the market data session is not logged on";
            subscribed.push_back(symbol);
            return "";
        };
        env.unsubscribe = [this](const std::string& symbol) { unsubscribed.push_back(symbol); };

        api = std::make_unique<ControlApi>(*host, registry, std::move(env), true);
    }

    StrategySpec spec(const std::string& symbol = kSym) {
        return StrategySpec{"CryptoMM", {{"symbol", symbol}, {"qty", "1"}}};
    }

    StrategyRegistry registry;
    std::ostringstream sink;
    cpm::Logger logger{sink, cpm::LogLevel::Error};
    std::unique_ptr<StrategyHost> host;
    std::unique_ptr<ControlApi> api;

    bool allow_orders = true;
    bool gate_open = true;
    bool md_up = true;
    bool oe_up = true;
    bool have_book = true;
    bool subscribe_ok = true;
    std::vector<std::string> subscribed, unsubscribed, sent, cancelled, resolved;
    std::string resolve_error;
    std::vector<PositionView> positions;
    std::vector<cpm::trader::PnlView> pnl;
    std::uint64_t counter = 0;
};

// --- the kinds and their schemas ------------------------------------------

TEST(ControlApi, PublishesTheKindsAndTheirSchemas) {
    Fixture f;
    const auto kinds = f.api->kinds();
    ASSERT_EQ(kinds.size(), 1u);
    EXPECT_EQ(kinds.front().name, "CryptoMM");
    EXPECT_FALSE(kinds.front().help.empty());
    // The form the GUI renders comes from here, not from the page's source.
    // Named rather than counted: adding a parameter to a strategy should not
    // fail a test about the API that publishes them.
    const auto& schema = kinds.front().schema;
    EXPECT_FALSE(schema.empty());
    EXPECT_EQ(schema.front().name, "symbol");
    EXPECT_TRUE(schema.front().required());
    for (const auto& wanted : {"symbol", "qty", "width-ticks", "withdraw-before", "ticks"}) {
        const bool present = std::any_of(schema.begin(), schema.end(),
                                         [&](const cpm::strategy::ParamSpec& spec) {
                                             return spec.name == wanted;
                                         });
        EXPECT_TRUE(present) << wanted << " is not in the published schema";
    }
}

// --- create, and the subscription that must come with it ------------------

TEST(ControlApi, ASeriesIsResolvedToTheWindowTradingNow) {
    Fixture f;
    const auto created =
        f.api->create(StrategySpec{"CryptoMM", {{"symbol", "BTCD15M"}, {"qty", "1"}}});
    ASSERT_TRUE(created.ok()) << created.error;
    EXPECT_EQ(f.resolved, std::vector<std::string>{"BTCD15M"}) << "the series was looked up";

    // The strategy runs on the instrument, and remembers the series it
    // follows so it can be moved to the next window.
    const auto info = f.api->snapshot().strategies.at(0);
    EXPECT_EQ(info.target, "BTCD15M");
    EXPECT_EQ(info.symbols, std::vector<std::string>{kWindow});
    EXPECT_EQ(info.params.at("symbol"), kWindow);
    EXPECT_EQ(f.subscribed, std::vector<std::string>{kWindow});
}

TEST(ControlApi, ASeriesIsRefusedClearlyWhenItCannotBeResolved) {
    // Typing BTCD15M is the obvious thing to do, and the gateway's answer to
    // it is "unknown symbol" - a reject, which halts the daemon terminally
    // (Q27). So it is never sent: either it resolves, or the operator is told
    // why it could not.
    Fixture f;
    f.resolve_error = "no window of BTCD15M is open right now";
    const auto refused =
        f.api->create(StrategySpec{"CryptoMM", {{"symbol", "BTCD15M"}, {"qty", "1"}}});
    EXPECT_FALSE(refused.ok());
    EXPECT_EQ(refused.error, "no window of BTCD15M is open right now");
    EXPECT_TRUE(f.subscribed.empty()) << "nothing may reach the gateway";
    EXPECT_TRUE(f.api->snapshot().strategies.empty()) << "and nothing half-built is left";
}

TEST(ControlApi, TwoStrategiesOnOneSeriesAreRefused) {
    // R-Q6: a series claims every window it will ever roll onto, so the
    // collision is caught before either has resolved.
    Fixture f;
    ASSERT_TRUE(f.api->create(StrategySpec{"CryptoMM", {{"symbol", "BTCD15M"}, {"qty", "1"}}}).ok());
    const auto second =
        f.api->create(StrategySpec{"CryptoMM", {{"symbol", "BTCD15M"}, {"qty", "1"}}});
    EXPECT_FALSE(second.ok());
    EXPECT_NE(second.error.find("already follows BTCD15M"), std::string::npos) << second.error;
}

TEST(ControlApi, RollsAStrategyOntoTheNextWindow) {
    Fixture f;
    const auto created =
        f.api->create(StrategySpec{"CryptoMM", {{"symbol", "BTCD15M"}, {"qty", "1"}}});
    ASSERT_TRUE(created.ok()) << created.error;
    const auto before = f.api->revision();

    const std::string next = "BTCD15M-BTC26OCT260645-0";
    const auto rolled = f.api->roll(created.id, next);
    ASSERT_TRUE(rolled.ok()) << rolled.error;

    const auto info = f.api->snapshot().strategies.at(0);
    EXPECT_EQ(info.id, created.id) << "the same strategy, not a new one";
    EXPECT_EQ(info.target, "BTCD15M");
    EXPECT_EQ(info.symbols, std::vector<std::string>{next});

    // The new window's book is opened before the strategy moves, and the old
    // one is dropped only after - an instrument that closed a couple of
    // minutes ago is already "unknown symbol", and that reject halts (R-N2).
    EXPECT_EQ(f.subscribed, (std::vector<std::string>{kWindow, next}));
    EXPECT_EQ(f.unsubscribed, std::vector<std::string>{kWindow});
    EXPECT_GT(f.api->revision(), before) << "the page has to hear about it";
}

TEST(ControlApi, AnInstrumentIsStillPinnedAndCannotRoll) {
    Fixture f;
    const auto created = f.api->create(StrategySpec{"CryptoMM", {{"symbol", kSym}, {"qty", "1"}}});
    ASSERT_TRUE(created.ok()) << created.error;
    EXPECT_TRUE(f.resolved.empty()) << "a full ticker is not looked up";
    EXPECT_TRUE(f.api->snapshot().strategies.at(0).target.empty());

    const auto rolled = f.api->roll(created.id, "BTCD15M-BTC26OCT260645-0");
    EXPECT_FALSE(rolled.ok());
    EXPECT_NE(rolled.error.find("follows one instrument"), std::string::npos) << rolled.error;
}

TEST(ControlApi, CreateSubscribesTheSymbol) {
    Fixture f;
    const auto result = f.api->create(f.spec());
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.id, "cryptomm-1");
    EXPECT_EQ(f.subscribed, std::vector<std::string>{kSym});
}

TEST(ControlApi, AFailedSubscriptionLeavesNothingHalfBuilt) {
    // A strategy on a symbol whose book never arrives is indistinguishable
    // from the wrong symbol, so it is refused rather than left to time out.
    Fixture f;
    f.subscribe_ok = false;
    const auto result = f.api->create(f.spec());
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.error, "the market data session is not logged on");
    EXPECT_TRUE(f.api->snapshot().strategies.empty());
}

TEST(ControlApi, BadParametersAreRefusedBeforeAnySubscription) {
    Fixture f;
    const auto result = f.api->create(StrategySpec{"CryptoMM", {{"symbol", kSym}, {"qty", "0"}}});
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.error, "qty is too small (0.01 to 10000)");
    EXPECT_TRUE(f.subscribed.empty());
}

TEST(ControlApi, AnUnknownKindIsRefused) {
    Fixture f;
    const auto result = f.api->create(StrategySpec{"Martingale", {{"symbol", kSym}}});
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.error, "unknown strategy kind: Martingale");
}

TEST(ControlApi, RemovingAStrategyUnsubscribesIt) {
    Fixture f;
    const auto id = f.api->create(f.spec()).id;
    ASSERT_TRUE(f.api->destroy(id).ok());
    EXPECT_EQ(f.unsubscribed, std::vector<std::string>{kSym});
    EXPECT_TRUE(f.api->snapshot().strategies.empty());
}

TEST(ControlApi, ARunningStrategyCannotBeRemoved) {
    Fixture f;
    const auto id = f.api->create(f.spec()).id;
    ASSERT_TRUE(f.api->start(id).ok());
    const auto result = f.api->destroy(id);
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("stop it first"), std::string::npos);
    EXPECT_TRUE(f.unsubscribed.empty());
}

// --- Stop keeps the row, and takes the feed ------------------------------

TEST(ControlApi, StoppingTheFeedKeepsTheRowAndEverythingOnIt) {
    Fixture f;
    const auto id = f.api->create(f.spec()).id;
    ASSERT_TRUE(f.api->stop_feed(id).ok());

    EXPECT_EQ(f.unsubscribed, std::vector<std::string>{kSym});
    // The row is still there, with its kind, its symbol and its parameters:
    // Start Feed is one call away, and nothing had to be typed again.
    const auto rows = f.api->snapshot().strategies;
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows.front().id, id);
    EXPECT_FALSE(rows.front().feed);
    EXPECT_EQ(rows.front().symbols, std::vector<std::string>{kSym});
    EXPECT_FALSE(rows.front().params.empty());
}

TEST(ControlApi, AStrategyWithNoFeedCannotStart) {
    Fixture f;
    const auto id = f.api->create(f.spec()).id;
    ASSERT_TRUE(f.api->stop_feed(id).ok());

    // The page disables the button; the rule itself lives in the daemon,
    // because the stdin console calls the same verb.
    const auto result = f.api->start(id);
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("start its feed first"), std::string::npos);
    EXPECT_EQ(f.api->snapshot().strategies.front().state, cpm::strategy::RunState::Stopped);
}

TEST(ControlApi, StartingTheFeedAgainSubscribesAndLetsItStart) {
    Fixture f;
    const auto id = f.api->create(f.spec()).id;
    ASSERT_TRUE(f.api->stop_feed(id).ok());
    ASSERT_TRUE(f.api->start_feed(id).ok());

    EXPECT_EQ(f.subscribed, (std::vector<std::string>{kSym, kSym}));
    EXPECT_TRUE(f.api->snapshot().strategies.front().feed);
    EXPECT_TRUE(f.api->start(id).ok()) << "with a book to read it may quote again";
}

TEST(ControlApi, AFeedIsNotStoppedWhileTheStrategyIsStillRunning) {
    Fixture f;
    const auto id = f.api->create(f.spec()).id;
    ASSERT_TRUE(f.api->start(id).ok());

    // The cancels have to be confirmed before the book they were quoting is
    // dropped, which is the same rule destroy follows.
    const auto result = f.api->stop_feed(id);
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("stop it first"), std::string::npos);
    EXPECT_TRUE(f.unsubscribed.empty());
    EXPECT_TRUE(f.api->snapshot().strategies.front().feed);
}

TEST(ControlApi, TheFeedIsNeitherStoppedTwiceNorStartedWhenItIsUp) {
    Fixture f;
    const auto id = f.api->create(f.spec()).id;
    EXPECT_FALSE(f.api->start_feed(id).ok()) << "it already has one";
    ASSERT_TRUE(f.api->stop_feed(id).ok());
    const auto again = f.api->stop_feed(id);
    EXPECT_FALSE(again.ok());
    EXPECT_NE(again.error.find("no feed to stop"), std::string::npos);
    EXPECT_EQ(f.unsubscribed.size(), 1u) << "and the second attempt dropped nothing";
}

TEST(ControlApi, RemovingARowWhoseFeedIsAlreadyStoppedDropsNothingMore) {
    Fixture f;
    const auto id = f.api->create(f.spec()).id;
    ASSERT_TRUE(f.api->stop_feed(id).ok());
    ASSERT_TRUE(f.api->destroy(id).ok());
    // Unsubscribing is not ref-counted, and by now the symbol may belong to
    // another row: a second unsubscribe would take its book away.
    EXPECT_EQ(f.unsubscribed, std::vector<std::string>{kSym});
    EXPECT_TRUE(f.api->snapshot().strategies.empty());
}

TEST(ControlApi, ARowWithNoFeedStillHoldsItsInstrument) {
    Fixture f;
    const auto first = f.api->create(f.spec()).id;
    ASSERT_TRUE(f.api->stop_feed(first).ok());

    // Unsubscribing is not ref-counted, so two rows on one instrument would
    // make a stopped feed able to take a live row's book. They cannot
    // happen: a stopped row goes on holding its instrument, and the second
    // create is refused the way it was before it was stopped. The operator
    // throws the row away (x) or starts its feed again.
    const auto second = f.api->create(f.spec());
    EXPECT_FALSE(second.ok());
    EXPECT_NE(second.error.find("already"), std::string::npos) << second.error;
    EXPECT_EQ(f.api->snapshot().strategies.size(), 1u);
    EXPECT_EQ(f.unsubscribed, std::vector<std::string>{kSym}) << "and nothing was dropped twice";
}

// --- the lifecycle an operator drives -------------------------------------

TEST(ControlApi, TheWholeLifecycleFromTheOutside) {
    Fixture f;
    const auto id = f.api->create(f.spec()).id;
    ASSERT_FALSE(id.empty());

    auto snapshot = f.api->snapshot();
    ASSERT_EQ(snapshot.strategies.size(), 1u);
    EXPECT_EQ(snapshot.strategies.front().state, RunState::Stopped);
    EXPECT_FALSE(snapshot.strategies.front().armed);

    ASSERT_TRUE(f.api->start(id).ok());
    EXPECT_EQ(f.api->snapshot().strategies.front().state, RunState::Running);

    // Running but disarmed: it computes and sends nothing.
    f.host->tick(Clock::now());
    EXPECT_TRUE(f.sent.empty());

    ASSERT_TRUE(f.api->arm(id).ok());
    f.host->tick(Clock::now());
    EXPECT_FALSE(f.sent.empty()) << "an armed strategy should quote";

    ASSERT_TRUE(f.api->disarm(id).ok());
    EXPECT_FALSE(f.api->snapshot().strategies.front().armed);

    ASSERT_TRUE(f.api->stop(id).ok());
    f.host->tick(Clock::now());
    EXPECT_FALSE(f.cancelled.empty()) << "stopping should withdraw the quotes";
}

TEST(ControlApi, ArmingIsRefusedWhenTheDaemonIsAnObserver) {
    Fixture f;
    f.allow_orders = false;
    const auto id = f.api->create(f.spec()).id;
    ASSERT_TRUE(f.api->start(id).ok());
    const auto result = f.api->arm(id);
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("--allow-orders"), std::string::npos);
}

TEST(ControlApi, ArmingWorksBeforeTheReconcilerHasFinished) {
    // The GUI's Arm button a second after startup must not be refused with a
    // message about a flag that was given.
    Fixture f;
    f.gate_open = false;
    const auto id = f.api->create(f.spec()).id;
    ASSERT_TRUE(f.api->start(id).ok());
    const auto armed = f.api->arm(id);
    EXPECT_TRUE(armed.ok()) << armed.error;
    EXPECT_TRUE(f.api->snapshot().strategies.front().armed);
    // Armed but held: nothing has gone out.
    f.host->tick(Clock::now());
    EXPECT_TRUE(f.sent.empty());
}

TEST(ControlApi, StopAllIsTheKillSwitch) {
    Fixture f;
    const auto first = f.api->create(f.spec()).id;
    const auto second = f.api->create(f.spec(kOther)).id;
    ASSERT_TRUE(f.api->start(first).ok());
    ASSERT_TRUE(f.api->start(second).ok());
    ASSERT_TRUE(f.api->arm(first).ok());
    ASSERT_TRUE(f.api->arm(second).ok());

    ASSERT_TRUE(f.api->stop_all().ok());
    for (const auto& strategy : f.api->snapshot().strategies) {
        EXPECT_NE(strategy.state, RunState::Running);
        EXPECT_FALSE(strategy.armed);
    }
}

TEST(ControlApi, RefusalsAreValuesNotExceptions) {
    // Every one of these crosses an HTTP boundary in G3, so none may throw.
    Fixture f;
    EXPECT_FALSE(f.api->start("nope").ok());
    EXPECT_FALSE(f.api->stop("nope").ok());
    EXPECT_FALSE(f.api->arm("nope").ok());
    EXPECT_FALSE(f.api->disarm("nope").ok());
    EXPECT_FALSE(f.api->destroy("nope").ok());
    EXPECT_EQ(f.api->start("nope").error, "no such strategy: nope");
}

// --- the snapshot ---------------------------------------------------------

TEST(ControlApi, TheSnapshotIsStartingUntilBothSessionsAreUp) {
    Fixture f;
    f.oe_up = false;
    EXPECT_EQ(f.api->snapshot().state, DaemonState::Starting);
    f.oe_up = true;
    EXPECT_EQ(f.api->snapshot().state, DaemonState::Live);
}

TEST(ControlApi, TheSnapshotCarriesTheSessionsAndTheirReasons) {
    Fixture f;
    f.md_up = false;
    const auto snapshot = f.api->snapshot();
    EXPECT_FALSE(snapshot.market_data.logged_on);
    EXPECT_EQ(snapshot.market_data.last_reason, "not connected");
    EXPECT_TRUE(snapshot.order_entry.logged_on);
    EXPECT_EQ(snapshot.recovery, "live");
}

TEST(ControlApi, OneBookPerClaimedSymbolWithoutDuplicates) {
    Fixture f;
    f.api->create(f.spec());
    f.api->create(f.spec(kOther));
    const auto snapshot = f.api->snapshot();
    ASSERT_EQ(snapshot.books.size(), 2u);
    EXPECT_EQ(snapshot.books[0].symbol, kSym);
    EXPECT_EQ(snapshot.books[1].symbol, kOther);
    // Scaled integers all the way out: nothing here is a double.
    EXPECT_EQ(snapshot.books[0].best_yes_bid, 500);
    EXPECT_EQ(snapshot.books[0].mid, 550);
}

TEST(ControlApi, NoStrategiesMeansNoBooks) {
    Fixture f;
    EXPECT_TRUE(f.api->snapshot().books.empty());
}

TEST(ControlApi, TheSnapshotCarriesThePositionsAndTheLimits) {
    Fixture f;
    PositionView position;
    position.symbol = kSym;
    position.net = 250;
    position.cash = -137500;
    position.fills = 2;
    f.positions.push_back(position);

    const auto snapshot = f.api->snapshot();
    ASSERT_EQ(snapshot.positions.size(), 1u);
    EXPECT_EQ(snapshot.positions.front().net, 250);
    EXPECT_EQ(snapshot.positions.front().fills, 2u);
    EXPECT_EQ(snapshot.limits.max_quantity, 1000);
    EXPECT_TRUE(snapshot.allow_orders);
}

// --- retuning a strategy (GUI-Q30) ----------------------------------------

TEST(ControlApi, RetunesAStrategyThatIsNotArmedAndMovesNoSubscription) {
    Fixture f;
    const auto created = f.api->create(f.spec());
    ASSERT_TRUE(created.ok()) << created.error;
    const auto subscribed_before = f.subscribed;

    const auto result = f.api->update(created.id, {{"width-ticks", "3"}, {"qty", "4"}});
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.id, created.id);

    const auto info = f.api->snapshot().strategies.at(0);
    EXPECT_EQ(info.params.at("width-ticks"), "3");
    EXPECT_EQ(info.params.at("qty"), "4");
    // The symbol is the one value an edit cannot change, so the book it is
    // watching is untouched: nothing was subscribed and nothing dropped.
    EXPECT_EQ(f.subscribed, subscribed_before);
    EXPECT_TRUE(f.unsubscribed.empty());
}

// The daemon's P&L line (GUI-Q15). Realized always adds up; unrealized stops
// adding up the moment one open position has no mark, because a sum missing a
// term reads as a smaller number rather than an unknown one.
TEST(ControlApi, TotalsThePnlAndRefusesToTotalAnUnmarkedOne) {
    Fixture f;
    f.pnl.push_back(cpm::trader::PnlView{"A", 550, 30000, 20000, 50000});
    f.pnl.push_back(cpm::trader::PnlView{"B", 400, 10000, -5000, 5000});

    auto snapshot = f.api->snapshot();
    EXPECT_EQ(snapshot.pnl.size(), 2u);
    EXPECT_EQ(snapshot.pnl_total.realized, 40000);
    EXPECT_EQ(snapshot.pnl_total.unrealized, 15000);
    EXPECT_EQ(snapshot.pnl_total.total, 55000);

    // One unmarked position, and the two halves part company: realized is
    // still exact, the rest is not known.
    f.pnl.push_back(cpm::trader::PnlView{"C", std::nullopt, 7000, std::nullopt, std::nullopt});
    snapshot = f.api->snapshot();
    EXPECT_EQ(snapshot.pnl_total.realized, 47000);
    EXPECT_FALSE(snapshot.pnl_total.unrealized.has_value());
    EXPECT_FALSE(snapshot.pnl_total.total.has_value());
}

TEST(ControlApi, RetuningAnArmedStrategyIsRefusedAsAValue) {
    Fixture f;
    const auto created = f.api->create(f.spec());
    ASSERT_TRUE(f.api->start(created.id).ok());
    ASSERT_TRUE(f.api->arm(created.id).ok());

    const auto result = f.api->update(created.id, {{"width-ticks", "3"}});
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("disarm it"), std::string::npos) << result.error;
    EXPECT_EQ(f.api->snapshot().strategies.at(0).params.at("width-ticks"), "4")
        << "the schema's default, unchanged";
}

TEST(ControlApi, RetuningASeriesStrategyKeepsTheSeriesAndTheWindow) {
    // The page shows the series in the symbol box and sends back what it
    // shows, so an edit of a rolling strategy arrives carrying "BTCD15M"
    // while the strategy itself is on a window of it.
    Fixture f;
    const auto created =
        f.api->create(StrategySpec{"CryptoMM", {{"symbol", "BTCD15M"}, {"qty", "1"}}});
    ASSERT_TRUE(created.ok()) << created.error;

    const auto result = f.api->update(created.id, {{"symbol", "BTCD15M"}, {"qty", "6"}});
    ASSERT_TRUE(result.ok()) << result.error;
    const auto info = f.api->snapshot().strategies.at(0);
    EXPECT_EQ(info.target, "BTCD15M");
    EXPECT_EQ(info.symbols, std::vector<std::string>{kWindow});
    EXPECT_EQ(info.params.at("symbol"), kWindow);
    EXPECT_EQ(info.params.at("qty"), "6");
    // No resolve and no subscribe: an edit is not a create and not a roll.
    EXPECT_EQ(f.resolved, std::vector<std::string>{"BTCD15M"});
    EXPECT_EQ(f.subscribed, std::vector<std::string>{kWindow});
}

TEST(ControlApi, TheRevisionAdvancesOnEveryChangeAndNotOnARead) {
    // The stream layer of G3 uses this to tell a stale snapshot from a
    // current one.
    Fixture f;
    EXPECT_EQ(f.api->revision(), 0u);
    const auto id = f.api->create(f.spec()).id;
    const auto after_create = f.api->revision();
    EXPECT_GT(after_create, 0u);

    f.api->snapshot();
    f.api->snapshot();
    EXPECT_EQ(f.api->revision(), after_create) << "reading must not count as a change";

    f.api->start(id);
    EXPECT_GT(f.api->revision(), after_create);
    const auto after_start = f.api->revision();
    f.api->start("nope");
    EXPECT_EQ(f.api->revision(), after_start) << "a refusal changed nothing";

    f.api->update(id, {{"width-ticks", "3"}});
    EXPECT_GT(f.api->revision(), after_start) << "a retune is a change the page must see";
}

// --- the halt is terminal and visible -------------------------------------

TEST(ControlApi, AHaltShowsItsReasonAndRefusesEverythingAfterwards) {
    Fixture f;
    const auto id = f.api->create(f.spec()).id;
    ASSERT_TRUE(f.api->start(id).ok());

    f.host->halt("market data: Gap on " + kSym);

    const auto snapshot = f.api->snapshot();
    EXPECT_EQ(snapshot.state, DaemonState::Halted);
    EXPECT_EQ(snapshot.halt_reason, "market data: Gap on " + kSym);
    // Still readable, which is the whole point of staying up (GUI-Q4).
    ASSERT_EQ(snapshot.strategies.size(), 1u);
    EXPECT_EQ(snapshot.strategies.front().state, RunState::Halted);
    EXPECT_FALSE(snapshot.books.empty());

    EXPECT_FALSE(f.api->start(id).ok());
    EXPECT_FALSE(f.api->arm(id).ok());
    EXPECT_FALSE(f.api->create(f.spec(kOther)).ok());
}

}  // namespace
