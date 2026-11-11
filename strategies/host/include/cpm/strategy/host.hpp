#pragma once

#include "cpm/common/logging.hpp"
#include "cpm/strategy/registry.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace cpm::strategy {

// Where a strategy is in its life. Stopping is not cosmetic: a strategy that
// has been told to stop still has cancels in flight, and it is not Stopped
// until nothing of its own is left at the venue.
enum class RunState {
    Stopped,
    Running,
    Stopping,
    Halted,  // the host halted; terminal for every strategy at once
};

const char* to_string(RunState state);

// One managed strategy, copied out for reading. Everything the GUI shows about
// a strategy is in here, and none of it is kind-specific.
struct StrategyInfo {
    std::string id;
    std::string kind;
    // The series it follows, empty when it was created on one fixed
    // instrument. `symbols` is the window it is on right now, which changes
    // under it at every roll.
    std::string target;
    ParamValues params;
    std::vector<std::string> symbols;
    RunState state = RunState::Stopped;
    bool armed = false;
    // Whether the owner is subscribed to this row's instrument. Not the
    // host's own fact - the host has never known what a feed is - but the
    // owner sets it here, because a row's buttons and whether it may start
    // at all follow from it, and both of those are read from here.
    //
    // A row with its feed stopped keeps everything else: its kind, its
    // symbol and its parameters are still on the page, ready for Start Feed.
    bool feed = true;
    // Why it last stopped, withdrew, or could not act. Operator-facing.
    std::string note;
    StrategyStatus status;
    bool requires_book_updates = true;
};

// Owns the strategy instances and drives them. It does not know about FIX: the
// books, the positions and the venue all arrive through the owner's callbacks,
// so the whole host is testable with fakes.
//
// Threading: every strategy call happens on the thread that calls tick().
// Reports arriving on a QuickFIX thread are queued and fanned out at the start
// of the next tick, which means a strategy is single-threaded by construction
// and no FIX callback ever blocks on strategy work. (crypto_mm used to call
// on_report from the session thread while its loop called on_market, with
// nothing between them; this is also the fix for that race.)
class StrategyHost {
public:
    struct Options {
        // Refuse a strategy on a symbol another one already claims (GUI-Q6):
        // the position is per symbol and could not be attributed between two.
        bool one_strategy_per_symbol = true;
        // How long a running strategy may see no book before the host says so.
        std::chrono::milliseconds book_warn_after{std::chrono::seconds(5)};
    };

    // What the host needs from its owner. In the daemon these come from
    // MarketState and OrderState; in the tests, from fakes.
    struct Feeds {
        std::function<std::optional<InstrumentView>(const std::string& symbol)> book;
        std::function<Position(const std::string& symbol)> position;
        // True while the instrument may be quoted. Defaults to Open or
        // Unknown, which is what crypto_mm checked inline.
        std::function<bool(const InstrumentView&)> tradable;
        // Where the underlying is, for the symbol a strategy is quoting.
        // Unset leaves every strategy without a reference, which the guard
        // reads as "off" rather than as "unsafe" - a daemon with no Binance
        // feed configured is the daemon as it was before B3.
        std::function<cpm::binance::ReferenceView(const std::string& symbol)> reference;
    };

    // `venue` is the owner's end of the hooks: send, cancel, next id, log. The
    // host wraps can_submit so that arming and the halt are enforced here
    // rather than inside any strategy.
    StrategyHost(Options options, const StrategyRegistry& registry, StrategyHooks venue,
                 Feeds feeds, Logger& logger);
    ~StrategyHost();

    StrategyHost(const StrategyHost&) = delete;
    StrategyHost& operator=(const StrategyHost&) = delete;

    // Each of these returns an empty string on success, or the reason it was
    // refused, worded for the operator. None of them throw for bad input.
    // `target` is the series this strategy follows, if any. The spec's symbol
    // is the window it starts on - already resolved, because the host has no
    // way to ask what is open and should not grow one.
    std::string create(const StrategySpec& spec, std::string& id_out, const std::string& target = {});
    // Move a strategy to the next window of its series: a fresh instance on
    // the new symbol, the same id, the same arming, the counters carried
    // over. The instance is replaced rather than re-pointed so that
    // IStrategy::symbols() stays fixed for an instance's life and no
    // per-instrument state can survive into a window it does not belong to
    // (docs/design-roll.md section 2).
    std::string roll(const std::string& id, const std::string& symbol);
    // Change a strategy's tunables in place, keeping its id, its symbols and
    // its counters. Only while it is not armed: an armed strategy is quoting
    // on the strength of what the operator last confirmed, and a width that
    // changed under it would put a different order in the market than the one
    // that was approved (GUI-Q30). The instance is rebuilt rather than
    // re-pointed, for the same reason roll() rebuilds it - a parameter is
    // read once, at construction, by every strategy here.
    //
    // `symbol` may be present among the values, because the form that sends
    // them shows it, but it may not differ from the one the strategy already
    // has: moving a strategy to another instrument is a different operation
    // with a subscription in it, and the API has roll() for the one case
    // where that happens by itself.
    std::string update(const std::string& id, const ParamValues& params);
    std::string start(const std::string& id);
    std::string stop(const std::string& id, const std::string& why);
    std::string arm(const std::string& id);
    std::string disarm(const std::string& id);
    // The owner saying whether it is subscribed to this row's instrument. It
    // subscribes and unsubscribes; this is only where the answer is kept, so
    // that start() can refuse a strategy with nothing to read and the page
    // can draw the row's buttons.
    std::string set_feed(const std::string& id, bool up);
    // Only a Stopped strategy can be removed, so nothing is dropped while it
    // still has orders in the market.
    std::string destroy(const std::string& id);

    // Stops every strategy. The kill switch, and the shutdown path.
    void stop_all(const std::string& why);
    // Terminal and daemon-wide: withdraw everything, refuse every start and
    // arm from here on. There is no way back (instructions amendment 4).
    void halt(const std::string& why);
    bool halted() const;
    std::string halt_reason() const;

    // Safe from a QuickFIX callback thread: it only queues.
    void on_report(const ExecutionReport& report);
    // One pass: drain the queued reports, then drive every running strategy.
    void tick(Clock::time_point now = Clock::now());
    // True when no strategy has anything left at the venue.
    bool quiescent() const;

    std::vector<StrategyInfo> list() const;
    std::optional<StrategyInfo> info(const std::string& id) const;

private:
    // Held by unique_ptr so an entry's address is stable: a strategy's
    // can_submit hook reads `armed` directly, without taking the host lock,
    // because it is called from inside tick() which already holds it.
    struct Entry {
        std::string id;
        std::string kind;
        std::string target;
        ParamValues params;
        std::vector<std::string> symbols;
        std::unique_ptr<IStrategy> strategy;
        RunState state = RunState::Stopped;
        std::atomic<bool> armed{false};
        std::string note;
        // The owner's answer about the subscription, set through set_feed().
        bool feed = true;
        // A stop the strategy asked for during its pass, acted on the moment
        // the pass returns. Empty when it has not asked.
        std::string stop_requested;
        // Latched so a standing condition is reported on a cadence rather
        // than four times a second: the same reason the CryptoMM driver only
        // logs a line when it changes.
        bool withdrawn_for_lifecycle = false;
        Clock::time_point last_book_warning{};
        Clock::time_point running_since{};
        // What the windows before this one did. A roll gives the strategy a
        // new instance with new counters, and an operator watching "placed"
        // fall to zero every quarter hour would learn nothing from it.
        StrategyStatus carried;
    };

    Entry* find(const std::string& id);
    const Entry* find(const std::string& id) const;
    // The hooks one strategy gets: the owner's venue, plus a can_submit that
    // folds in this strategy's arming and the host-wide halt.
    StrategyHooks hooks_for(Entry& entry) const;
    void drive(Entry& entry, Clock::time_point now);
    // Fills a StrategyInfo from an entry. Caller holds the lock.
    StrategyInfo describe(const Entry& entry) const;
    // Withdraws and moves to Stopping. Caller holds the lock.
    void begin_stop(Entry& entry, const std::string& why);
    // Folds a finished instance's counters into the entry's carried totals,
    // before the instance is replaced. Caller holds the lock.
    void carry_counters(Entry& entry);
    void drain_reports();

    const Options options_;
    const StrategyRegistry& registry_;
    const StrategyHooks venue_;
    const Feeds feeds_;
    Logger& logger_;

    // Guards entries_, next_id_ and halt_reason_. Held across strategy calls,
    // so the report queue gets its own lock below.
    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<Entry>> entries_;
    std::uint64_t next_id_ = 0;
    std::string halt_reason_;
    std::atomic<bool> halted_{false};

    // A FIX callback thread only ever touches these two, and never waits on
    // mutex_, so strategy work can never stall the session.
    mutable std::mutex reports_mutex_;
    std::deque<ExecutionReport> reports_;
};

}  // namespace cpm::strategy
