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
using cpm::Qty;
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
    // `orders` is the daemon-wide --allow-orders key, which is fixed for the
    // life of a run and so for the life of a ControlApi.
    explicit Fixture(bool orders = true) : allow_orders(orders) {
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
            view.valid = book_valid;
            view.best_yes_bid = 500;
            view.implied_yes_ask = 600;
            view.best_yes_bid_qty = 500;
            view.best_no_bid_qty = 400;
            view.mid = 550;
            view.status = book_status;  // as cpm::md::to_string spells it
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
        // The operator's own order path (G13): the same gate the strategies
        // get, the daemon's one counter, and a send that answers with the
        // reason rather than throwing.
        env.can_submit = [this] { return gate_open; };
        env.resting_orders = [this](const std::string&) { return resting; };
        env.watched = [this] { return watched; };
        env.axis = [](const std::string& symbol) {
            cpm::trader::AxisView axis;
            axis.symbol = symbol;
            axis.structure = "tapered";
            axis.prices = {100, 500, 900};
            return axis;
        };
        env.next_cl_ord_id = [this] { return "ex-" + std::to_string(++counter); };
        env.send_order = [this](const cpm::oms::NewOrder& order) {
            exits.push_back(order);
            return send_error;
        };

        api = std::make_unique<ControlApi>(*host, registry, std::move(env), allow_orders);
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
    // The instrument's lifecycle status, as the daemon spells it. Only Open
    // may be quoted or exited ([MD] O6).
    std::string book_status{"Open"};
    // False is a book no live subscription is building any more.
    bool book_valid = true;
    std::vector<std::string> subscribed, unsubscribed, sent, cancelled, resolved;
    std::string resolve_error;
    std::vector<PositionView> positions;
    std::vector<cpm::trader::PnlView> pnl;
    std::uint64_t counter = 0;
    // The operator's exits (G13): what reached the order path, and the
    // refusal it answers with.
    std::vector<cpm::oms::NewOrder> exits;
    std::string send_error;
    // What of ours is still resting on the instrument, which an exit must
    // not cross.
    std::vector<cpm::strategy::LiveLeg> resting;
    // What the daemon watches for its own sake, with or without a strategy.
    std::vector<std::string> watched;

    // A position to exit, in contracts at kQtyScale.
    void hold(Qty net, const std::string& symbol = kSym) {
        PositionView position;
        position.symbol = symbol;
        position.net = net;
        position.bought = net > 0 ? net : 0;
        position.sold = net < 0 ? -net : 0;
        position.fills = 1;
        positions.push_back(position);
    }
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

// --- the window's own clock (G16) -----------------------------------------

// Arming into the last seconds of a window is a bell, not a trade: it quotes
// for a moment, has to come straight back off, and anything still in flight
// at the close is INSTRUMENT_NOT_ACTIVE, which used to halt the daemon.
TEST(ControlApi, RefusesToArmIntoAWindowThatIsAboutToEnd) {
    Fixture f;
    // A real dated window, dated so that it has long since ended.
    const std::string gone = "BTCD15M-BTC26NOV040800-0";
    const auto created = f.api->create(StrategySpec{"CryptoMM", {{"symbol", gone}, {"qty", "1"}}});
    ASSERT_TRUE(created.ok()) << created.error;
    ASSERT_TRUE(f.api->start(created.id).ok());

    const auto armed = f.api->arm(created.id);
    EXPECT_FALSE(armed.ok());
    EXPECT_NE(armed.error.find("window has already ended"), std::string::npos) << armed.error;
    // It can still be created and started - looking at a closed window is
    // harmless, and only arming puts an order near the bell.
    EXPECT_EQ(f.api->snapshot().strategies.size(), 1u);
}

// A name with no scheduled end has no bell to be near, so nothing changes
// for a sports market or a simulator instrument.
TEST(ControlApi, ArmingAnInstrumentWithNoScheduledEndIsUnaffected) {
    Fixture f;
    const auto id = f.api->create(f.spec()).id;
    ASSERT_TRUE(f.api->start(id).ok());
    EXPECT_TRUE(f.api->arm(id).ok());
}

TEST(ControlApi, RefusesAnExitOnAWindowThatHasEnded) {
    Fixture f;
    const std::string gone = "BTCD15M-BTC26NOV040800-0";
    f.hold(500, gone);

    const auto result = f.api->exit_position(gone, 0);
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("window has ended"), std::string::npos) << result.error;
    EXPECT_TRUE(f.exits.empty());
    // The page is told the same thing, so the button is never offered.
    const auto snapshot = f.api->snapshot();
    EXPECT_NE(snapshot.positions.front().exit_refusal.find("window has ended"),
              std::string::npos);
}

// --- the ladder's price grid ----------------------------------------------

// A watched instrument has a book on the page from startup, so it must have
// a ladder too: the grid is what the ladder is drawn on, and without it the
// page asks once, gets nothing, and shows an empty panel (GUI-Q35).
TEST(ControlApi, AWatchedInstrumentHasALadderWithNoStrategyOnIt) {
    Fixture f;
    EXPECT_TRUE(f.api->axis(kSym).prices.empty()) << "nothing watches it yet";

    f.watched.push_back(kSym);
    EXPECT_FALSE(f.api->axis(kSym).prices.empty());
    EXPECT_TRUE(f.api->axis(kOther).prices.empty()) << "only what is watched";

    // And a strategy's own instrument still has one, watched or not.
    f.watched.clear();
    ASSERT_TRUE(f.api->create(f.spec()).ok());
    EXPECT_FALSE(f.api->axis(kSym).prices.empty());
}

// --- exiting a position (G13) ---------------------------------------------
// The position is per symbol and outlives the row that opened it, so exiting
// one is an action on the position and not on any strategy.

TEST(ControlApi, ExitsALongBySellingIntoTheBidForIoc) {
    Fixture f;
    f.hold(500);  // long 5

    const auto result = f.api->exit_position(kSym, 0);  // 0 is all of it
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_EQ(f.exits.size(), 1u);
    const auto& order = f.exits.front();
    EXPECT_EQ(order.symbol, kSym);
    EXPECT_EQ(order.side, cpm::oms::Side::Sell);
    EXPECT_EQ(order.quantity, 500);
    // The bid, because that is what a long crosses into; never a market
    // order, which this venue forces to the worst admissible price.
    EXPECT_EQ(order.type, cpm::oms::OrderType::Limit);
    EXPECT_EQ(order.price, 500);
    // IOC and not post-only: it is meant to cross, and what it cannot fill
    // must not be left resting for a cancel to halt on later (OE-Q6).
    EXPECT_EQ(order.time_in_force, cpm::oms::TimeInForce::Ioc);
    EXPECT_EQ(order.post_only, cpm::oms::PostOnly::None);
    // The ClOrdID says it was not a quote, and it comes from the daemon's
    // one counter, which is what the bookmark persists.
    EXPECT_EQ(order.cl_ord_id, result.id);
    EXPECT_EQ(order.cl_ord_id.rfind("ex-", 0), 0u);
}

TEST(ControlApi, ExitsAShortByBuyingTheImpliedAsk) {
    Fixture f;
    f.hold(-300);  // short 3

    ASSERT_TRUE(f.api->exit_position(kSym, 0).ok());
    ASSERT_EQ(f.exits.size(), 1u);
    EXPECT_EQ(f.exits.front().side, cpm::oms::Side::Buy);
    EXPECT_EQ(f.exits.front().quantity, 300);
    // The YES ask this venue implies from the best NO bid (Q9).
    EXPECT_EQ(f.exits.front().price, 600);
}

TEST(ControlApi, ExitsOnlyTheQuantityAsked) {
    Fixture f;
    f.hold(500);

    ASSERT_TRUE(f.api->exit_position(kSym, 200).ok());
    ASSERT_EQ(f.exits.size(), 1u);
    EXPECT_EQ(f.exits.front().quantity, 200);
}

TEST(ControlApi, RefusesMoreThanIsHeldRatherThanClampingIt) {
    Fixture f;
    f.hold(300);

    const auto result = f.api->exit_position(kSym, 500);
    EXPECT_FALSE(result.ok());
    // The number the operator typed, and the number they hold, both named.
    EXPECT_NE(result.error.find("more than the 3 held"), std::string::npos) << result.error;
    EXPECT_TRUE(f.exits.empty()) << "nothing may reach the venue after a refusal";
}

TEST(ControlApi, RefusesAnExitWithNoPositionAndOneThatIsFlat) {
    Fixture f;
    EXPECT_FALSE(f.api->exit_position(kSym, 0).ok());

    f.hold(0);
    const auto flat = f.api->exit_position(kSym, 0);
    EXPECT_FALSE(flat.ok());
    EXPECT_NE(flat.error.find("flat"), std::string::npos) << flat.error;
    EXPECT_TRUE(f.exits.empty());
}

// The one that matters most: an order on a window that has stopped trading
// is answered INSTRUMENT_NOT_ACTIVE, and that reject halts the whole run
// (OE-Q6). So a closed position is refused here rather than at the gateway.
TEST(ControlApi, RefusesAnExitOnAnythingButAnOpenInstrument) {
    Fixture f;
    f.hold(500);
    f.book_status = "Closed";

    const auto result = f.api->exit_position(kSym, 0);
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("Closed"), std::string::npos) << result.error;
    EXPECT_NE(result.error.find("halts"), std::string::npos) << result.error;
    EXPECT_TRUE(f.exits.empty());
}

// A book nobody is building is not a price. The daemon unbinds a book when it
// unsubscribes, so this is what a position whose feed was stopped looks like,
// and crossing to the last touch it happened to remember is how an exit ends
// up quoting into a market that has moved on.
TEST(ControlApi, RefusesAnExitWhenTheBookIsNoLongerLive) {
    Fixture f;
    f.hold(500);
    f.book_valid = false;

    const auto result = f.api->exit_position(kSym, 0);
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("not live"), std::string::npos) << result.error;
    EXPECT_TRUE(f.exits.empty());
    // And the page is told, so the button is not offered in the first place.
    const auto snapshot = f.api->snapshot();
    EXPECT_NE(snapshot.positions.front().exit_refusal.find("not live"), std::string::npos);
}

TEST(ControlApi, RefusesAnExitWhileAStrategyIsArmedOnTheSymbol) {
    Fixture f;
    f.hold(500);
    const auto id = f.api->create(f.spec()).id;
    ASSERT_TRUE(f.api->start(id).ok());
    ASSERT_TRUE(f.api->arm(id).ok());

    const auto result = f.api->exit_position(kSym, 0);
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("disarm it first"), std::string::npos) << result.error;
    EXPECT_TRUE(f.exits.empty());

    // Disarmed, the same press goes through: nothing is quoting over it now.
    ASSERT_TRUE(f.api->disarm(id).ok());
    EXPECT_TRUE(f.api->exit_position(kSym, 0).ok());
    EXPECT_EQ(f.exits.size(), 1u);
}

// Disarming stops new quotes; the last ones are still in the market until
// their cancels come back, and that is exactly the moment an operator reaches
// for Exit. Crossing one of our own orders is a self-match, which the venue
// answers by cancelling a side - so the exit could vanish instead of exiting.
TEST(ControlApi, RefusesAnExitThatWouldTradeWithOurOwnRestingOrder) {
    Fixture f;
    f.hold(500);  // long 5, so the exit sells into the bid at 500
    f.resting.push_back({cpm::oms::Side::Buy, "mm-7", 500, 100, false});

    const auto result = f.api->exit_position(kSym, 0);
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("mm-7"), std::string::npos) << result.error;
    EXPECT_NE(result.error.find("trade with itself"), std::string::npos) << result.error;
    EXPECT_TRUE(f.exits.empty());

    // Our order on the other side of the book is not in the way: an exit
    // that sells into the bid cannot cross an offer above it.
    f.resting.clear();
    f.resting.push_back({cpm::oms::Side::Sell, "mm-8", 600, 100, false});
    EXPECT_TRUE(f.api->exit_position(kSym, 0).ok());
}

TEST(ControlApi, RefusesAnExitWhileNothingMayBeSubmitted) {
    Fixture f;
    f.hold(500);
    f.gate_open = false;  // reconciling

    EXPECT_FALSE(f.api->exit_position(kSym, 0).ok());
    f.gate_open = true;
    EXPECT_TRUE(f.api->exit_position(kSym, 0).ok());
}

TEST(ControlApi, RefusesAnExitAfterAHaltAndInAnObserverRun) {
    Fixture observer(false);  // started without --allow-orders
    observer.hold(500);
    const auto refused = observer.api->exit_position(kSym, 0);
    EXPECT_FALSE(refused.ok());
    EXPECT_NE(refused.error.find("--allow-orders"), std::string::npos) << refused.error;

    Fixture f;
    f.hold(500);
    f.host->halt("market data: Gap on " + kSym);
    EXPECT_FALSE(f.api->exit_position(kSym, 0).ok());
    EXPECT_TRUE(f.exits.empty());
}

TEST(ControlApi, AnExitRefusedByTheOrderPathIsAValueNotAnException) {
    Fixture f;
    f.hold(500);
    f.send_error = "OrderQty 5 exceeds the limit of 1";

    const auto result = f.api->exit_position(kSym, 0);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.error, "OrderQty 5 exceeds the limit of 1");
}

TEST(ControlApi, TheSnapshotSaysWhetherEachPositionCanBeExitedAndAtWhat) {
    Fixture f;
    f.hold(500);

    auto snapshot = f.api->snapshot();
    ASSERT_EQ(snapshot.positions.size(), 1u);
    const auto& open = snapshot.positions.front();
    EXPECT_EQ(open.exit_refusal, "") << "an exitable position carries no refusal";
    ASSERT_TRUE(open.exit_price.has_value());
    EXPECT_EQ(*open.exit_price, 500);
    EXPECT_EQ(open.exit_available, 500);
    EXPECT_EQ(open.status, "Open");

    // And when it cannot: the page reads the reason from here rather than
    // deciding for itself, so the button it offers and the order the daemon
    // would send cannot disagree.
    f.book_status = "Settled";
    snapshot = f.api->snapshot();
    const auto& closed = snapshot.positions.front();
    EXPECT_FALSE(closed.exit_refusal.empty());
    EXPECT_FALSE(closed.exit_price.has_value());
    EXPECT_EQ(closed.status, "Settled");
}

TEST(ControlApi, AnExitAdvancesTheRevisionSoThePageSeesTheOrder) {
    Fixture f;
    f.hold(500);
    const auto before = f.api->revision();
    ASSERT_TRUE(f.api->exit_position(kSym, 0).ok());
    EXPECT_GT(f.api->revision(), before);

    // A refusal changes nothing, so it moves nothing.
    const auto after = f.api->revision();
    EXPECT_FALSE(f.api->exit_position(kOther, 0).ok());
    EXPECT_EQ(f.api->revision(), after);
}

}  // namespace
