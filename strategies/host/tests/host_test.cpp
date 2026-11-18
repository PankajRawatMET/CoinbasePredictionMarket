// The strategy host: the registry, the run-state machine, arming, the halt,
// and the report queue that keeps a strategy single-threaded.
#include "cpm/strategy/host.hpp"

#include "cpm/md/ticker.hpp"

#include <gtest/gtest.h>

#include <map>

#include <sstream>
#include <thread>

namespace {

using cpm::md::InstrumentView;
using cpm::md::LifecycleStatus;
using cpm::oms::ExecutionReport;
using cpm::oms::Position;
using cpm::strategy::IStrategy;
using cpm::strategy::LiveLeg;
using cpm::strategy::ParamSpec;
using cpm::strategy::ParamValues;
using cpm::strategy::RunState;
using cpm::strategy::StrategyHooks;
using cpm::strategy::StrategyHost;
using cpm::strategy::StrategyRegistry;
using cpm::strategy::StrategySpec;
using cpm::strategy::StrategyStatus;
using Clock = cpm::strategy::Clock;

const std::string kSym = "SIMOE-26NOV01HOMAWY-HOM";
const std::string kOther = "SIMOE-26NOV01HOMAWY-AWY";

// A strategy that records what the host did to it. It holds one notional
// resting order so the Stopping -> Stopped transition can be driven by hand.
class Fake final : public IStrategy {
public:
    Fake(std::string symbol, std::string width, StrategyHooks hooks)
        : width(std::move(width)), symbol_(std::move(symbol)), hooks_(std::move(hooks)) {}

    void on_market(const InstrumentView&, const Position&, Clock::time_point) override {
        ++passes;
        could_submit = hooks_.can_submit && hooks_.can_submit();
        // What CryptoMM does at its inventory limit: ask, and carry on with
        // the pass. The host acts on it the moment the pass returns.
        if (!stop_me_with.empty() && hooks_.stop_me) hooks_.stop_me(stop_me_with);
    }
    void on_report(const ExecutionReport& report) override { reports.push_back(report.cl_ord_id); }
    void withdraw(const std::string& why) override { withdrawals.push_back(why); }
    void abandon(const std::string& why) override { abandonments.push_back(why); }
    bool quiescent() const override { return !resting; }
    // A comma splits one parameter into two legs, which is how a test asks
    // for a strategy on more than one symbol.
    std::vector<std::string> symbols() const override {
        std::vector<std::string> out;
        std::size_t at = 0;
        while (at <= symbol_.size()) {
            const auto comma = symbol_.find(',', at);
            out.push_back(symbol_.substr(at, comma == std::string::npos ? comma : comma - at));
            if (comma == std::string::npos) break;
            at = comma + 1;
        }
        return out;
    }
    StrategyStatus status() const override {
        StrategyStatus status;
        status.passes = passes;
        status.reason = reason;
        if (resting) status.legs.push_back(LiveLeg{});
        return status;
    }

    // Sends through the hooks the host handed it, which is the only way an
    // order ever leaves a strategy.
    std::string place(const std::string& cl_ord_id) {
        cpm::oms::NewOrder order;
        order.cl_ord_id = cl_ord_id;
        return hooks_.send ? hooks_.send(order) : std::string();
    }
    // Commentary, through the hook the host handed it: the strategy says
    // what it decided and nothing about itself.
    void say(const std::string& line) {
        if (hooks_.log) hooks_.log(line);
    }
    bool pull(const std::string& cl_ord_id) {
        cpm::oms::CancelOrder request;
        request.cl_ord_id = cl_ord_id;
        return hooks_.cancel && hooks_.cancel(request);
    }

    std::string width;
    // Non-empty: ask the host to stop this strategy on the next pass.
    std::string stop_me_with;
    std::size_t passes = 0;
    bool could_submit = false;
    bool resting = false;
    std::string reason;
    std::vector<std::string> reports;
    std::vector<std::string> withdrawals;
    std::vector<std::string> abandonments;

private:
    std::string symbol_;
    StrategyHooks hooks_;
};

InstrumentView book(LifecycleStatus status = LifecycleStatus::Open) {
    InstrumentView view;
    view.symbol = kSym;
    view.book_valid = true;
    view.status = status;
    return view;
}

// A host with one registered kind, fakes for the venue and the feeds, and a
// handle on the Fake instances so a test can see what reached them.
struct Fixture {
    Fixture() {
        registry.add("Fake", "a test strategy", schema(),
                     [this](const ParamValues& values, StrategyHooks hooks) {
                         auto fake = std::make_unique<Fake>(values.at("symbol"),
                                                           values.at("width-ticks"),
                                                           std::move(hooks));
                         fakes.push_back(fake.get());
                         return fake;
                     });

        StrategyHooks venue;
        venue.can_submit = [this] { return allow_orders && gate_open; };
        venue.orders_permitted = [this] { return allow_orders; };
        venue.send = [](const cpm::oms::NewOrder&) { return std::string("x"); };
        venue.cancel = [](const cpm::oms::CancelOrder&) { return true; };
        venue.next_cl_ord_id = [this] { return "c" + std::to_string(++counter); };
        venue.attribute = [this](const std::string& cl_ord_id, const std::string& strategy_id) {
            attributed[cl_ord_id] = strategy_id;
        };
        venue.log = [this](const std::string& line) { logs.push_back(line); };

        StrategyHost::Feeds feeds;
        feeds.book = [this](const std::string& symbol) -> std::optional<InstrumentView> {
            if (!have_book) return std::nullopt;
            auto view = book(status);
            view.symbol = symbol;
            return view;
        };
        feeds.position = [](const std::string& symbol) {
            Position position;
            position.symbol = symbol;
            return position;
        };
        feeds.stale_book_reason = [this](const std::string& symbol, Clock::time_point) {
            return symbol == stale_symbol ? "stale feed on " + symbol : std::string{};
        };

        host = std::make_unique<StrategyHost>(options, registry, std::move(venue), std::move(feeds),
                                             logger);
    }

    static cpm::strategy::ParamSchema schema() {
        ParamSpec symbol;
        symbol.name = "symbol";
        symbol.type = ParamSpec::Type::Text;
        ParamSpec width;
        width.name = "width-ticks";
        width.type = ParamSpec::Type::Int;
        width.default_value = "2";
        width.min = "1";
        width.max = "9";
        return {symbol, width};
    }

    std::string create(const std::string& symbol = kSym) {
        std::string id;
        const auto error = host->create(StrategySpec{"Fake", {{"symbol", symbol}}}, id);
        EXPECT_EQ(error, "") << error;
        return id;
    }

    // Created, started and armed, which is the state most tests want.
    std::string live(const std::string& symbol = kSym) {
        const auto id = create(symbol);
        EXPECT_EQ(host->start(id), "");
        EXPECT_EQ(host->arm(id), "");
        return id;
    }

    StrategyHost::Options options;
    StrategyRegistry registry;
    std::ostringstream sink;
    cpm::Logger logger{sink, cpm::LogLevel::Error};
    std::unique_ptr<StrategyHost> host;
    std::vector<Fake*> fakes;
    bool allow_orders = true;
    // The reconciler's gate: open once it has reconciled, shut again on a
    // halt. Separate from allow_orders, which is the static --allow-orders key.
    bool gate_open = true;
    bool have_book = true;
    std::string stale_symbol;
    LifecycleStatus status = LifecycleStatus::Open;
    std::uint64_t counter = 0;
    // ClOrdID -> the strategy the host said it came from.
    std::map<std::string, std::string> attributed;
    // Every line a strategy logged, as the daemon would have received it.
    std::vector<std::string> logs;
};

// --- attribution ----------------------------------------------------------

TEST(Attribution, SaysWhichStrategyAnOrderAndItsCancelCameFrom) {
    Fixture f;
    const auto first = f.live();
    const auto second = f.live(kOther);
    ASSERT_EQ(f.fakes.size(), 2u);

    // The ClOrdID cannot say by itself: the counter behind it is the
    // daemon's, so the host is the only thing that knows (docs/design-logs.md).
    EXPECT_EQ(f.fakes[0]->place("mm-1"), "x");
    EXPECT_EQ(f.fakes[1]->place("mm-2"), "x");
    EXPECT_TRUE(f.fakes[0]->pull("c-3"));

    EXPECT_EQ(f.attributed["mm-1"], first);
    EXPECT_EQ(f.attributed["mm-2"], second);
    EXPECT_EQ(f.attributed["c-3"], first);
    EXPECT_NE(first, second);
}

TEST(Attribution, StillSendsWhenNobodyIsAttributing) {
    Fixture f;
    // Every test and every run that writes no record files: the hook is
    // unset and the send goes out exactly as before.
    f.host.reset();
    cpm::strategy::StrategyHooks venue;
    venue.send = [](const cpm::oms::NewOrder&) { return std::string("sent"); };
    venue.cancel = [](const cpm::oms::CancelOrder&) { return true; };
    StrategyHost::Feeds feeds;
    feeds.book = [](const std::string&) -> std::optional<InstrumentView> { return book(); };
    feeds.position = [](const std::string&) { return Position{}; };
    f.fakes.clear();
    f.host = std::make_unique<StrategyHost>(f.options, f.registry, std::move(venue),
                                            std::move(feeds), f.logger);
    f.live();
    ASSERT_EQ(f.fakes.size(), 1u);
    EXPECT_EQ(f.fakes[0]->place("mm-1"), "sent");
    EXPECT_TRUE(f.attributed.empty());
}

// --- the tag on a strategy's own commentary --------------------------------

TEST(StrategyLog, SaysWhichStrategyAndInstrumentALineIsAbout) {
    Fixture f;
    const auto id = f.live();
    ASSERT_EQ(f.fakes.size(), 1u);

    // The strategy writes what it decided; the host says whose decision it
    // was. One file holds every series, so without this a line about
    // ...-HOM and a line about ...-AWY are indistinguishable.
    f.fakes[0]->say("market 0.51 / 0.52, mid 0.515 -> quoting 0.51 / 0.53");
    ASSERT_EQ(f.logs.size(), 1u);
    EXPECT_EQ(f.logs[0], "[" + id + " " + kSym +
                             "] market 0.51 / 0.52, mid 0.515 -> quoting 0.51 / 0.53");
}

TEST(StrategyLog, TheTagFollowsARoll) {
    Fixture f;
    std::string id;
    ASSERT_EQ(f.host->create(StrategySpec{"Fake", {{"symbol", kSym}}}, id, "26NOV01HOMAWY"), "");
    ASSERT_EQ(f.fakes.size(), 1u);
    f.fakes[0]->say("quoting 0.51 / 0.53");

    // A new window is a new instance on the same row, so the tag cannot be
    // decided once and kept: it is read when the line is written.
    ASSERT_EQ(f.host->roll(id, kOther), "");
    ASSERT_EQ(f.fakes.size(), 2u);
    f.fakes[1]->say("quoting 0.47 / 0.49");

    ASSERT_EQ(f.logs.size(), 2u);
    EXPECT_EQ(f.logs[0], "[" + id + " " + kSym + "] quoting 0.51 / 0.53");
    EXPECT_EQ(f.logs[1], "[" + id + " " + kOther + "] quoting 0.47 / 0.49");
}

TEST(StrategyLog, NamesOnlyTheStrategyWhenItQuotesMoreThanOneSymbol) {
    Fixture f;
    // Two legs: the hook carries a line and not what it was about, so a
    // symbol here would be a guess. The strategy says which leg itself.
    const auto id = f.live(kSym + "," + kOther);
    ASSERT_EQ(f.fakes.size(), 1u);

    f.fakes[0]->say("withdrawing both quotes: reference is stale");
    ASSERT_EQ(f.logs.size(), 1u);
    EXPECT_EQ(f.logs[0], "[" + id + "] withdrawing both quotes: reference is stale");
}

// --- a strategy that stops itself -----------------------------------------

TEST(StopsItself, IsStoppedAsSoonAsThePassReturns) {
    Fixture f;
    const auto id = f.live();
    ASSERT_EQ(f.fakes.size(), 1u);
    f.host->tick(Clock::now());
    ASSERT_EQ(f.host->info(id)->state, cpm::strategy::RunState::Running);

    f.fakes[0]->stop_me_with = "long inventory limit reached: net 3 of 3";
    f.host->tick(Clock::now());

    // Stopped on the same pass it asked, with the reason on the row and its
    // quotes withdrawn - a strategy at its limit must not get another pass.
    EXPECT_EQ(f.host->info(id)->state, cpm::strategy::RunState::Stopped);
    EXPECT_EQ(f.host->info(id)->note, "long inventory limit reached: net 3 of 3");
    ASSERT_EQ(f.fakes[0]->withdrawals.size(), 1u);
    EXPECT_EQ(f.fakes[0]->withdrawals.front(), "long inventory limit reached: net 3 of 3");
    // Stopping disarms, so a Start afterwards asks the operator again.
    EXPECT_FALSE(f.host->info(id)->armed);

    const auto passes = f.fakes[0]->passes;
    f.host->tick(Clock::now());
    EXPECT_EQ(f.fakes[0]->passes, passes) << "a stopped strategy is never driven again";
}

TEST(StopsItself, WaitsForItsCancelsLikeAnyOtherStop) {
    Fixture f;
    const auto id = f.live();
    ASSERT_EQ(f.fakes.size(), 1u);
    f.fakes[0]->resting = true;  // something of its own is still at the venue
    f.fakes[0]->stop_me_with = "short inventory limit reached: net -4 of 3";
    f.host->tick(Clock::now());

    EXPECT_EQ(f.host->info(id)->state, cpm::strategy::RunState::Stopping);
    f.fakes[0]->resting = false;
    f.host->tick(Clock::now());
    EXPECT_EQ(f.host->info(id)->state, cpm::strategy::RunState::Stopped);
}

TEST(StopsItself, KeepsItsRowAndItsFeed) {
    Fixture f;
    const auto id = f.live();
    f.fakes[0]->stop_me_with = "long inventory limit reached: net 3 of 3";
    f.host->tick(Clock::now());

    // Not the Stop button: the operator did not ask for this, so the book
    // stays up for them to look at and the row offers Start and Stop.
    const auto info = f.host->info(id);
    ASSERT_TRUE(info.has_value());
    EXPECT_TRUE(info->feed);
    EXPECT_EQ(f.host->list().size(), 1u);
}

// --- the feed a row reads -------------------------------------------------

TEST(Feed, AStrategyWithNoFeedIsNotStarted) {
    Fixture f;
    const auto id = f.create();
    ASSERT_EQ(f.host->set_feed(id, false), "");
    // A strategy with no book to read would quote nothing and not say why.
    EXPECT_EQ(f.host->start(id), id + " has no feed: start its feed first");
    EXPECT_EQ(f.host->info(id)->state, cpm::strategy::RunState::Stopped);
    EXPECT_FALSE(f.host->info(id)->feed);

    // And it starts once it has one again.
    ASSERT_EQ(f.host->set_feed(id, true), "");
    EXPECT_EQ(f.host->start(id), "");
    EXPECT_EQ(f.host->info(id)->state, cpm::strategy::RunState::Running);
}

TEST(Feed, ARowKeepsItsParametersWhileItsFeedIsStopped) {
    Fixture f;
    const auto id = f.create();
    ASSERT_EQ(f.host->set_feed(id, false), "");
    const auto info = f.host->info(id);
    ASSERT_TRUE(info.has_value());
    // Everything the row shows survives: this is what makes Start Feed a
    // button rather than typing it all again.
    EXPECT_EQ(info->symbols, std::vector<std::string>{kSym});
    EXPECT_EQ(info->params.at("width-ticks"), "2");
    EXPECT_EQ(info->kind, "Fake");
}

// --- the registry ---------------------------------------------------------

TEST(Registry, RefusesAnUnknownKind) {
    Fixture f;
    std::string id;
    EXPECT_EQ(f.host->create(StrategySpec{"NoSuch", {{"symbol", kSym}}}, id),
              "unknown strategy kind: NoSuch");
    EXPECT_TRUE(f.host->list().empty());
}

TEST(Registry, RefusesParametersTheSchemaRejectsAndCreatesNothing) {
    Fixture f;
    std::string id;
    EXPECT_EQ(f.host->create(StrategySpec{"Fake", {}}, id), "symbol is required");
    EXPECT_TRUE(f.host->list().empty());
    // The failed attempt did not consume an id, so the next one is still -1.
    EXPECT_EQ(f.create(), "fake-1");
}

TEST(Registry, RefusesADuplicateKind) {
    Fixture f;
    EXPECT_THROW(f.registry.add("Fake", "again", Fixture::schema(),
                                [](const ParamValues&, StrategyHooks) {
                                    return std::unique_ptr<IStrategy>();
                                }),
                 std::invalid_argument);
}

TEST(Registry, RecordsTheValuesItUsedIncludingDefaults) {
    Fixture f;
    const auto id = f.create();
    const auto info = f.host->info(id);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->kind, "Fake");
    EXPECT_EQ(info->params.at("symbol"), kSym);
    EXPECT_EQ(info->symbols, std::vector<std::string>{kSym});
}

// --- one strategy per symbol (GUI-Q6) -------------------------------------

TEST(Host, RefusesASecondStrategyOnTheSameSymbol) {
    Fixture f;
    const auto first = f.live();
    std::string id;
    const auto error = f.host->create(StrategySpec{"Fake", {{"symbol", kSym}}}, id);
    EXPECT_EQ(error, first + " already has " + kSym +
                         "; two strategies on one symbol would share its position");
    EXPECT_EQ(f.host->list().size(), 1u);
}

TEST(Host, AllowsADifferentSymbol) {
    Fixture f;
    f.live();
    std::string id;
    EXPECT_EQ(f.host->create(StrategySpec{"Fake", {{"symbol", kOther}}}, id), "");
    EXPECT_EQ(f.host->list().size(), 2u);
}

TEST(Host, ASymbolIsClaimedByAStrategyThatMerelyExists) {
    // A strategy is Stopped the moment it is created, so a check that only
    // looked at the running ones would not be a check at all.
    Fixture f;
    const auto first = f.create();
    std::string id;
    EXPECT_EQ(f.host->create(StrategySpec{"Fake", {{"symbol", kSym}}}, id),
              first + " already has " + kSym +
                  "; two strategies on one symbol would share its position");
}

TEST(Host, ASymbolIsFreeAgainOnlyOnceTheStrategyUsingItIsRemoved) {
    Fixture f;
    const auto first = f.live();
    ASSERT_EQ(f.host->stop(first, "done"), "");
    std::string id;
    // Stopped is not enough: it still holds the symbol, and its subscription.
    EXPECT_NE(f.host->create(StrategySpec{"Fake", {{"symbol", kSym}}}, id), "");
    ASSERT_EQ(f.host->destroy(first), "");
    EXPECT_EQ(f.host->create(StrategySpec{"Fake", {{"symbol", kSym}}}, id), "");
}

// --- the run state machine ------------------------------------------------

TEST(Host, ANewStrategyIsStoppedAndDisarmed) {
    Fixture f;
    const auto info = f.host->info(f.create());
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->state, RunState::Stopped);
    EXPECT_FALSE(info->armed);
}

TEST(Host, OnlyARunningStrategyIsDriven) {
    Fixture f;
    const auto id = f.create();
    f.host->tick(Clock::now());
    EXPECT_EQ(f.fakes.at(0)->passes, 0u);

    ASSERT_EQ(f.host->start(id), "");
    f.host->tick(Clock::now());
    EXPECT_EQ(f.fakes.at(0)->passes, 1u);
}

TEST(Host, AStoppedStrategyIsNotDrivenAgainSoItCannotReQuote) {
    // QuoteManager::withdraw() cancels but does not latch: the next
    // on_market() would quote again. The host not driving it is what makes
    // "stopped" mean stopped.
    Fixture f;
    const auto id = f.live();
    f.host->tick(Clock::now());
    ASSERT_EQ(f.fakes.at(0)->passes, 1u);

    ASSERT_EQ(f.host->stop(id, "done"), "");
    f.host->tick(Clock::now());
    f.host->tick(Clock::now());
    EXPECT_EQ(f.fakes.at(0)->passes, 1u);
}

TEST(Host, StoppingLastsUntilNothingIsRestingAtTheVenue) {
    Fixture f;
    const auto id = f.live();
    f.fakes.at(0)->resting = true;

    ASSERT_EQ(f.host->stop(id, "done"), "");
    EXPECT_EQ(f.host->info(id)->state, RunState::Stopping);
    EXPECT_EQ(f.fakes.at(0)->withdrawals, std::vector<std::string>{"done"});
    EXPECT_FALSE(f.host->quiescent());

    f.host->tick(Clock::now());
    EXPECT_EQ(f.host->info(id)->state, RunState::Stopping);

    // The cancel is confirmed.
    f.fakes.at(0)->resting = false;
    f.host->tick(Clock::now());
    EXPECT_EQ(f.host->info(id)->state, RunState::Stopped);
    EXPECT_TRUE(f.host->quiescent());
}

TEST(Host, StoppingSomethingWithNothingRestingIsImmediate) {
    Fixture f;
    const auto id = f.live();
    ASSERT_EQ(f.host->stop(id, "done"), "");
    EXPECT_EQ(f.host->info(id)->state, RunState::Stopped);
}

TEST(Host, OnlyStrategiesUsingAStaleBookStopAndRestartRequiresFreshData) {
    Fixture f;
    const auto affected = f.live(kSym);
    const auto healthy = f.live(kOther);
    f.fakes.at(0)->resting = true;
    f.stale_symbol = kSym;
    f.host->tick();

    EXPECT_EQ(f.host->info(affected)->state, RunState::Stopping);
    EXPECT_FALSE(f.host->info(affected)->armed);
    EXPECT_EQ(f.fakes.at(0)->withdrawals, std::vector<std::string>{"stale feed on " + kSym});
    EXPECT_EQ(f.fakes.at(0)->passes, 0u);
    EXPECT_EQ(f.host->info(healthy)->state, RunState::Running);
    EXPECT_TRUE(f.host->info(healthy)->armed);
    EXPECT_EQ(f.fakes.at(1)->passes, 1u);

    f.stale_symbol.clear();
    EXPECT_FALSE(f.host->start(affected).empty());
    f.stale_symbol = kSym;
    f.fakes.at(0)->resting = false;
    f.host->tick();
    EXPECT_EQ(f.host->info(affected)->state, RunState::Stopped);
    EXPECT_EQ(f.host->start(affected), "stale feed on " + kSym);
    EXPECT_EQ(f.host->arm(affected), "stale feed on " + kSym);

    f.stale_symbol.clear();
    f.host->tick();
    EXPECT_EQ(f.host->info(affected)->state, RunState::Stopped);
    f.have_book = false;
    EXPECT_EQ(f.host->start(affected), "waiting for a fresh book on " + kSym);
    f.have_book = true;
    ASSERT_EQ(f.host->start(affected), "");
    EXPECT_FALSE(f.host->info(affected)->armed);
    EXPECT_FALSE(f.host->halted());
}

TEST(Host, CannotRemoveAStrategyThatStillHasOrders) {
    Fixture f;
    const auto id = f.live();
    f.fakes.at(0)->resting = true;
    ASSERT_EQ(f.host->stop(id, "done"), "");
    EXPECT_EQ(f.host->destroy(id), id + " is stopping; stop it first");

    f.fakes.at(0)->resting = false;
    f.host->tick(Clock::now());
    EXPECT_EQ(f.host->destroy(id), "");
    EXPECT_TRUE(f.host->list().empty());
}

TEST(Host, UnknownIdsAreReportedNotThrown) {
    Fixture f;
    EXPECT_EQ(f.host->start("nope"), "no such strategy: nope");
    EXPECT_EQ(f.host->stop("nope", "x"), "no such strategy: nope");
    EXPECT_EQ(f.host->arm("nope"), "no such strategy: nope");
    EXPECT_EQ(f.host->disarm("nope"), "no such strategy: nope");
    EXPECT_EQ(f.host->destroy("nope"), "no such strategy: nope");
    EXPECT_FALSE(f.host->info("nope").has_value());
}

// --- the two keys on the trigger ------------------------------------------

TEST(Host, ADisarmedStrategyRunsButCannotSubmit) {
    Fixture f;
    const auto id = f.create();
    ASSERT_EQ(f.host->start(id), "");
    f.host->tick(Clock::now());
    EXPECT_EQ(f.fakes.at(0)->passes, 1u);
    EXPECT_FALSE(f.fakes.at(0)->could_submit);

    ASSERT_EQ(f.host->arm(id), "");
    f.host->tick(Clock::now());
    EXPECT_TRUE(f.fakes.at(0)->could_submit);
}

TEST(Host, ArmingIsRefusedWhileTheOwnerIsNotAllowingOrders) {
    Fixture f;
    f.allow_orders = false;
    const auto id = f.create();
    ASSERT_EQ(f.host->start(id), "");
    EXPECT_EQ(f.host->arm(id), "the daemon is not allowing orders (start it with --allow-orders)");
    EXPECT_FALSE(f.host->info(id)->armed);
}

TEST(Host, ArmingIsAllowedWhileTheReconcilerIsStillWorking) {
    // The gate is shut until the reconciler finishes, which is a reason to
    // hold the orders back, not a reason to refuse the operator's intent. It
    // refused outright before, so arming right after startup failed with a
    // message about a flag that had in fact been given.
    Fixture f;
    f.gate_open = false;
    const auto id = f.create();
    ASSERT_EQ(f.host->start(id), "");
    EXPECT_EQ(f.host->arm(id), "") << "arming should not wait on reconciliation";
    EXPECT_TRUE(f.host->info(id)->armed);

    // Armed, but nothing goes out until the gate opens.
    f.host->tick(Clock::now());
    EXPECT_FALSE(f.fakes.at(0)->could_submit);
    f.gate_open = true;
    f.host->tick(Clock::now());
    EXPECT_TRUE(f.fakes.at(0)->could_submit);
}

TEST(Host, TheOwnerGateStillAppliesToAnArmedStrategy) {
    // Arming is one key; the reconciler closing the gate mid-run is the other.
    Fixture f;
    const auto id = f.live();
    f.host->tick(Clock::now());
    ASSERT_TRUE(f.fakes.at(0)->could_submit);

    f.gate_open = false;
    f.host->tick(Clock::now());
    EXPECT_FALSE(f.fakes.at(0)->could_submit);
    EXPECT_TRUE(f.host->info(id)->armed);
}

TEST(Host, DisarmingLeavesItRunning) {
    Fixture f;
    const auto id = f.live();
    ASSERT_EQ(f.host->disarm(id), "");
    f.host->tick(Clock::now());
    EXPECT_EQ(f.host->info(id)->state, RunState::Running);
    EXPECT_FALSE(f.fakes.at(0)->could_submit);
}

TEST(Host, StoppingDisarms) {
    // Restarting must not silently resume trading on the strength of an
    // arming from before whatever made the operator stop it.
    Fixture f;
    const auto id = f.live();
    ASSERT_EQ(f.host->stop(id, "done"), "");
    EXPECT_FALSE(f.host->info(id)->armed);

    ASSERT_EQ(f.host->start(id), "");
    f.host->tick(Clock::now());
    EXPECT_FALSE(f.fakes.at(0)->could_submit) << "a restarted strategy must be armed again";
}

TEST(Host, StopAllDisarmsAsWellAsStopping) {
    Fixture f;
    const auto first = f.live();
    const auto second = f.live(kOther);

    f.host->stop_all("kill switch");
    for (const auto& info : f.host->list()) {
        EXPECT_EQ(info.state, RunState::Stopped);
        EXPECT_FALSE(info.armed);
    }
    EXPECT_EQ(f.fakes.at(0)->withdrawals, std::vector<std::string>{"kill switch"});
    EXPECT_EQ(f.fakes.at(1)->withdrawals, std::vector<std::string>{"kill switch"});
    EXPECT_EQ(f.host->start(first), "");  // a stopped strategy can be restarted
    EXPECT_FALSE(f.host->info(second)->armed);
}

// --- the halt is terminal -------------------------------------------------

TEST(Host, AHaltWithdrawsEverythingAndRefusesEverythingAfterwards) {
    Fixture f;
    const auto id = f.live();
    std::string other;

    f.host->halt("sequence gap on the book");
    EXPECT_TRUE(f.host->halted());
    EXPECT_EQ(f.host->halt_reason(), "sequence gap on the book");

    const auto info = f.host->info(id);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->state, RunState::Halted);
    EXPECT_FALSE(info->armed);
    EXPECT_EQ(f.fakes.at(0)->withdrawals,
              std::vector<std::string>{"halted: sequence gap on the book"});

    EXPECT_EQ(f.host->start(id), "halted: sequence gap on the book");
    EXPECT_EQ(f.host->arm(id), "halted: sequence gap on the book");
    EXPECT_EQ(f.host->create(StrategySpec{"Fake", {{"symbol", kOther}}}, other),
              "halted: sequence gap on the book");
}

TEST(Host, AHaltedStrategyIsNeverDrivenAgain) {
    Fixture f;
    f.live();
    f.host->halt("stale feed");
    f.host->tick(Clock::now());
    f.host->tick(Clock::now());
    EXPECT_EQ(f.fakes.at(0)->passes, 0u);
}

TEST(Host, TheFirstHaltReasonIsTheOneThatSticks) {
    Fixture f;
    f.live();
    f.host->halt("sequence gap");
    f.host->halt("disconnect");
    EXPECT_EQ(f.host->halt_reason(), "sequence gap");
}

// --- reports --------------------------------------------------------------

TEST(Host, ReportsAreQueuedAndDeliveredOnTheNextTick) {
    // on_report() may be called from a QuickFIX thread, so it must not touch a
    // strategy: it queues, and tick() fans out.
    Fixture f;
    f.live();
    ExecutionReport report;
    report.cl_ord_id = "mm-1";
    f.host->on_report(report);
    EXPECT_TRUE(f.fakes.at(0)->reports.empty());

    f.host->tick(Clock::now());
    EXPECT_EQ(f.fakes.at(0)->reports, std::vector<std::string>{"mm-1"});
}

TEST(Host, EveryStrategySeesEveryReportAndIgnoresWhatIsNotItsOwn) {
    // A report for a deleted instrument carries "deleted_instrument" as its
    // symbol, so the host cannot route by symbol and does not try.
    Fixture f;
    f.live();
    f.live(kOther);
    ExecutionReport report;
    report.cl_ord_id = "mm-7";
    report.symbol = "deleted_instrument";
    f.host->on_report(report);
    f.host->tick(Clock::now());
    EXPECT_EQ(f.fakes.at(0)->reports, std::vector<std::string>{"mm-7"});
    EXPECT_EQ(f.fakes.at(1)->reports, std::vector<std::string>{"mm-7"});
}

TEST(Host, ReportsAreDeliveredEvenToAStoppedStrategy) {
    // Its cancels are still coming back, and they are what move it to Stopped.
    Fixture f;
    const auto id = f.live();
    f.fakes.at(0)->resting = true;
    ASSERT_EQ(f.host->stop(id, "done"), "");

    ExecutionReport report;
    report.cl_ord_id = "c1";
    f.host->on_report(report);
    f.host->tick(Clock::now());
    EXPECT_EQ(f.fakes.at(0)->reports, std::vector<std::string>{"c1"});
}

TEST(Host, QueuedReportsSurviveABurstFromAnotherThread) {
    Fixture f;
    f.live();
    std::thread session([&] {
        for (int i = 0; i < 200; ++i) {
            ExecutionReport report;
            report.cl_ord_id = "r" + std::to_string(i);
            f.host->on_report(report);
        }
    });
    session.join();
    f.host->tick(Clock::now());
    EXPECT_EQ(f.fakes.at(0)->reports.size(), 200u);
}

// --- the book and the lifecycle -------------------------------------------

TEST(Host, NoBookMeansNoPassAndAnExplanation) {
    Fixture f;
    f.have_book = false;
    const auto id = f.live();
    const auto start = Clock::now();

    f.host->tick(start);
    EXPECT_EQ(f.fakes.at(0)->passes, 0u);
    // Said on a cadence rather than four times a second.
    EXPECT_TRUE(f.host->info(id)->note.empty());

    f.host->tick(start + f.options.book_warn_after);
    EXPECT_NE(f.host->info(id)->note.find("no book for " + kSym), std::string::npos);
}

// The instrument's own name says when it stops trading, and that is the one
// fact about the close that needs nothing to arrive. On 2026-11-04 a strategy
// quoted for 475 ms past the bell: the lifecycle feed had not delivered
// Closed, the book still read 0.51/0.52, and the third order sent in that gap
// came back INSTRUMENT_NOT_ACTIVE and halted the daemon.
TEST(Host, AStrategyIsNotDrivenPastItsWindowsEndWhateverTheFeedSays) {
    Fixture f;
    // A dated window, so its end can be read out of its name.
    const std::string window = "BTCD15M-BTC26NOV040800-0";
    const auto id = f.live(window);
    // Everything the daemon could otherwise go on still says "trade me".
    f.status = LifecycleStatus::Open;

    const auto ends = *cpm::md::parse_ticker(window);
    f.host->tick(ends.window_end - std::chrono::seconds(1));
    EXPECT_EQ(f.fakes.at(0)->passes, 1u) << "a second before the close it still quotes";

    f.host->tick(ends.window_end);
    f.host->tick(ends.window_end + std::chrono::milliseconds(500));
    EXPECT_EQ(f.fakes.at(0)->passes, 1u) << "it was driven past the end of its window";
    // Abandoned, not withdrawn: the venue has already cancelled what rested
    // on the window it closed, and a cancel racing that one halts.
    EXPECT_EQ(f.fakes.at(0)->abandonments.size(), 1u);
    EXPECT_TRUE(f.fakes.at(0)->withdrawals.empty());
    EXPECT_NE(f.host->info(id)->note.find("window has ended"), std::string::npos)
        << f.host->info(id)->note;
}

// A name with no scheduled end - a sports market, a simulator instrument -
// has nothing to read, and is left to the lifecycle status alone.
TEST(Host, AnInstrumentWithNoScheduledEndIsUnaffectedByTheClock) {
    Fixture f;
    f.live();  // kSym: SIMOE-26NOV01HOMAWY-HOM, which parse_ticker cannot take apart
    f.host->tick(Clock::now());
    f.host->tick(Clock::now() + std::chrono::hours(24));
    EXPECT_EQ(f.fakes.at(0)->passes, 2u);
    EXPECT_TRUE(f.fakes.at(0)->abandonments.empty());
}

TEST(Host, AnInstrumentThatHasStoppedTradingIsAbandonedOnceAndNeverCancelled) {
    Fixture f;
    const auto id = f.live();
    f.status = LifecycleStatus::Closed;

    f.host->tick(Clock::now());
    f.host->tick(Clock::now());
    EXPECT_EQ(f.fakes.at(0)->passes, 0u);
    // Abandoned, not withdrawn. The venue cancels what rests on an instrument
    // it closes (ExecRestatementReason=2), and a cancel of ours racing that
    // one comes back UNKNOWN_ORDER - which halts the daemon (OE-Q6).
    EXPECT_EQ(f.fakes.at(0)->abandonments.size(), 1u);
    EXPECT_TRUE(f.fakes.at(0)->withdrawals.empty());
    EXPECT_NE(f.host->info(id)->note.find("instrument is"), std::string::npos);

    // It reopens: quoting resumes, and a later close abandons again.
    f.status = LifecycleStatus::Open;
    f.host->tick(Clock::now());
    EXPECT_EQ(f.fakes.at(0)->passes, 1u);
    f.status = LifecycleStatus::Closed;
    f.host->tick(Clock::now());
    EXPECT_EQ(f.fakes.at(0)->abandonments.size(), 2u);
    EXPECT_TRUE(f.fakes.at(0)->withdrawals.empty());
}

TEST(Host, StoppingAStrategyStillCancels) {
    // The distinction only applies to a closed instrument. An operator who
    // stops a strategy on a live market has orders out there that nobody else
    // is going to cancel.
    Fixture f;
    const auto id = f.live();
    f.fakes.at(0)->resting = true;
    ASSERT_EQ(f.host->stop(id, "operator"), "");
    EXPECT_EQ(f.fakes.at(0)->withdrawals.size(), 1u);
    EXPECT_TRUE(f.fakes.at(0)->abandonments.empty());
}

TEST(Host, AnUnknownLifecycleStatusIsStillQuotable) {
    // Q13: the status is Unknown until the first SecurityStatus arrives, and a
    // strategy that refused to quote until then would never start.
    Fixture f;
    f.live();
    f.status = LifecycleStatus::Unknown;
    f.host->tick(Clock::now());
    EXPECT_EQ(f.fakes.at(0)->passes, 1u);
}

TEST(Host, StatusIsCopiedOutFromTheStrategy) {
    Fixture f;
    const auto id = f.live();
    f.fakes.at(0)->reason = "one-sided book";
    f.host->tick(Clock::now());
    const auto info = f.host->info(id);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->status.passes, 1u);
    EXPECT_EQ(info->status.reason, "one-sided book");
}

// --- editing the parameters (GUI-Q30) -------------------------------------

TEST(Update, ChangesTheParametersOfAFeedThatQuotesNothing) {
    Fixture f;
    const auto id = f.create();
    ASSERT_EQ(f.fakes.size(), 1u);
    EXPECT_EQ(f.fakes.at(0)->width, "2");

    EXPECT_EQ(f.host->update(id, {{"width-ticks", "5"}}), "");

    // A new instance, built with the new value: a parameter is read at
    // construction, so this is the only way one can change at all.
    ASSERT_EQ(f.fakes.size(), 2u);
    EXPECT_EQ(f.fakes.at(1)->width, "5");
    const auto info = f.host->info(id);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->params.at("width-ticks"), "5");
    EXPECT_EQ(info->id, id);
    EXPECT_EQ(info->state, RunState::Stopped);
    EXPECT_EQ(info->symbols, std::vector<std::string>{kSym});
}

TEST(Update, ChangesThemWhileItRunsDisarmed) {
    // The state the operator watches a strategy in before trusting it: the
    // feed is up, it is computing quotes and sending nothing.
    Fixture f;
    const auto id = f.create();
    ASSERT_EQ(f.host->start(id), "");
    f.host->tick(Clock::now());

    EXPECT_EQ(f.host->update(id, {{"width-ticks", "7"}}), "");
    ASSERT_EQ(f.fakes.size(), 2u);
    EXPECT_EQ(f.fakes.at(1)->width, "7");
    const auto info = f.host->info(id);
    ASSERT_TRUE(info.has_value());
    // Still running, and still disarmed: an edit is not a start.
    EXPECT_EQ(info->state, RunState::Running);
    EXPECT_FALSE(info->armed);
    // The new instance is the one being driven from here on.
    f.host->tick(Clock::now());
    EXPECT_EQ(f.fakes.at(1)->passes, 1u);
}

TEST(Update, IsRefusedWhileItIsArmed) {
    Fixture f;
    const auto id = f.live();
    EXPECT_EQ(f.host->update(id, {{"width-ticks", "5"}}),
              id + " is armed and may be quoting: disarm it before changing its parameters");
    // Nothing was rebuilt and nothing was changed.
    EXPECT_EQ(f.fakes.size(), 1u);
    const auto info = f.host->info(id);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->params.at("width-ticks"), "2");
}

TEST(Update, DisarmingOpensThemAgain) {
    Fixture f;
    const auto id = f.live();
    ASSERT_EQ(f.host->disarm(id), "");
    EXPECT_EQ(f.host->update(id, {{"width-ticks", "5"}}), "");
    ASSERT_EQ(f.fakes.size(), 2u);
    EXPECT_EQ(f.fakes.at(1)->width, "5");
}

TEST(Update, CancelsWhatTheOldInstanceStillHadResting) {
    // Disarmed does not mean empty-handed: a disarm leaves whatever was
    // resting to be taken off deliberately, and the instance that placed it
    // is about to be replaced.
    Fixture f;
    const auto id = f.live();
    f.fakes.at(0)->resting = true;
    ASSERT_EQ(f.host->disarm(id), "");

    EXPECT_EQ(f.host->update(id, {{"width-ticks", "5"}}), "");
    ASSERT_EQ(f.fakes.size(), 2u);
    EXPECT_EQ(f.fakes.at(0)->withdrawals, std::vector<std::string>{"parameters changed"});
    // A cancel, never an abandon: the market is open and the venue is not
    // going to take these off for us (OE-Q6 is the other way round).
    EXPECT_TRUE(f.fakes.at(0)->abandonments.empty());
}

TEST(Update, LeavesAQuiescentStrategyAlone) {
    Fixture f;
    const auto id = f.create();
    EXPECT_EQ(f.host->update(id, {{"width-ticks", "5"}}), "");
    EXPECT_TRUE(f.fakes.at(0)->withdrawals.empty());
}

TEST(Update, RefusesToMoveAStrategyToAnotherSymbol) {
    Fixture f;
    const auto id = f.create();
    EXPECT_EQ(f.host->update(id, {{"symbol", kOther}}),
              "a strategy cannot be moved to " + kOther + " by changing its parameters: stop " +
                  id + ", remove it, and start a feed on the new symbol");
    EXPECT_EQ(f.fakes.size(), 1u);
}

TEST(Update, AcceptsTheSymbolItAlreadyHas) {
    // The form sends every box it shows, the symbol among them.
    Fixture f;
    const auto id = f.create();
    EXPECT_EQ(f.host->update(id, {{"symbol", kSym}, {"width-ticks", "5"}}), "");
    ASSERT_EQ(f.fakes.size(), 2u);
    EXPECT_EQ(f.fakes.at(1)->width, "5");
}

TEST(Update, RefusesAValueTheSchemaRefusesAndRebuildsNothing) {
    Fixture f;
    const auto id = f.create();
    EXPECT_EQ(f.host->update(id, {{"width-ticks", "99"}}),
              "width-ticks is too large (1 to 9)");
    EXPECT_EQ(f.fakes.size(), 1u);
    const auto info = f.host->info(id);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->params.at("width-ticks"), "2");
}

TEST(Update, KeepsWhatWasNotSent) {
    Fixture f;
    const auto id = f.create();
    EXPECT_EQ(f.host->update(id, {}), "");
    const auto info = f.host->info(id);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->params.at("symbol"), kSym);
    EXPECT_EQ(info->params.at("width-ticks"), "2");
}

TEST(Update, CarriesTheCountersOverTheRebuild) {
    Fixture f;
    const auto id = f.create();
    ASSERT_EQ(f.host->start(id), "");
    f.host->tick(Clock::now());
    f.host->tick(Clock::now());

    EXPECT_EQ(f.host->update(id, {{"width-ticks", "5"}}), "");
    auto info = f.host->info(id);
    ASSERT_TRUE(info.has_value());
    // What the strategy has done does not fall to zero because it was
    // retuned, for the same reason a roll does not reset it.
    EXPECT_EQ(info->status.passes, 2u);
    f.host->tick(Clock::now());
    info = f.host->info(id);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->status.passes, 3u);
}

TEST(Update, IsRefusedWhileItIsStillStopping) {
    Fixture f;
    const auto id = f.live();
    f.fakes.at(0)->resting = true;
    ASSERT_EQ(f.host->stop(id, "by hand"), "");
    ASSERT_EQ(f.host->info(id)->state, RunState::Stopping);
    EXPECT_EQ(f.host->update(id, {{"width-ticks", "5"}}),
              id + " is still stopping; its cancels have not come back yet");
}

TEST(Update, IsRefusedAfterAHalt) {
    Fixture f;
    const auto id = f.create();
    f.host->halt("a gap on the feed");
    EXPECT_EQ(f.host->update(id, {{"width-ticks", "5"}}), "halted: a gap on the feed");
}

TEST(Update, RefusesAnUnknownStrategy) {
    Fixture f;
    EXPECT_EQ(f.host->update("fake-9", {{"width-ticks", "5"}}), "no strategy called fake-9");
}

}  // namespace
