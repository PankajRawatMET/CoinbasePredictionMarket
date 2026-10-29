#pragma once

#include "cpm/md/events.hpp"
#include "cpm/md/ticker.hpp"

#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// Which instrument of a series is trading right now.
//
// A rolling series - BTCD15M, ETHD15M - is a chain of 15-minute windows, and
// quoting one means following the chain. The all-instrument lifecycle
// subscription (269=L, 146=0) announces every window as it is Listed and
// Opened, and every one as it Closes, is Determined and Settles ([MD] O6).
// This turns that stream of events into one answer per series.
//
// It is a pure state machine: no FIX, no sockets, no clock of its own. apply()
// is safe from the session's callback thread; current() is asked from the
// daemon's tick thread, which is where anything is acted on.
//
// What it deliberately does not do: subscribe, unsubscribe, or decide that a
// roll has happened. The owner compares current() with what it is on and acts
// - on its own thread, where it is allowed to (docs/design-roll.md section 2).
namespace cpm::md {

struct SeriesWindow {
    std::string symbol;       // "BTCD15M-BTC26OCT260630-0"
    std::string security_id;  // SecurityID(48), as the feed gave it
    Clock::time_point window_end{};
    LifecycleStatus status = LifecycleStatus::Unknown;
    Clock::time_point status_time{};

    // Tradable: Open, and only Open. Listed is "publicly visible but not
    // tradeable" (REST InstrumentStatus), and everything after Open is over.
    bool tradable() const { return status == LifecycleStatus::Open; }
};

class SeriesWatcher {
public:
    struct Options {
        // A window whose end has passed by more than this is not offered as
        // current, whatever the feed last said about it. The feed is
        // changes-only, so a Closed that never arrived would otherwise leave
        // a dead window looking live - and quoting one is a rejected order,
        // which halts (OE-Q6).
        std::chrono::seconds past_end_grace{30};
        // How long a window that has stopped trading is remembered, so the
        // owner can still say what it was and what it settled at.
        std::chrono::seconds remember_finished{std::chrono::minutes(5)};
    };

    // Two constructors rather than a default argument: Options{} in a default
    // argument is evaluated before the enclosing class is complete.
    SeriesWatcher();
    explicit SeriesWatcher(Options options);

    // One lifecycle event. Names that are not dated windows - a sports
    // market, a simulator symbol - are ignored: they have no series to roll.
    void apply(const InstrumentLifecycle& event);

    // The window of `series` that is open at `now`, if there is one. `now` is
    // passed rather than read so that a test owns the clock and so that the
    // answer is the caller's instant, not an instant between its calls.
    std::optional<SeriesWindow> current(const std::string& series, Clock::time_point now) const;
    // The most recent window of `series` that is no longer tradable, while it
    // is still remembered. What the roll just left behind.
    std::optional<SeriesWindow> previous(const std::string& series) const;
    // Everything known about a series, newest window first. For the GUI and
    // for diagnosing a series that has gone quiet.
    std::vector<SeriesWindow> windows(const std::string& series) const;
    std::vector<std::string> series() const;

    // Drops what is past remember_finished. A daemon that stays up for days
    // would otherwise accumulate every window it ever saw - 96 a day per
    // D15M series ([MD] O2/O6), which is the same reason
    // MarketState::evict_finished exists.
    std::size_t evict(Clock::time_point now);

private:
    Options options_;
    mutable std::mutex mutex_;
    // series -> symbol -> what is known about it.
    std::map<std::string, std::map<std::string, SeriesWindow>> series_;
};

}  // namespace cpm::md
