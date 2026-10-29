#include "cpm/md/series_watcher.hpp"

#include <algorithm>

namespace cpm::md {
namespace {

// Everything after Open. Listed is before it, and Unknown is "nothing has
// been said yet", which is not the same as finished.
bool finished(LifecycleStatus status) {
    switch (status) {
        case LifecycleStatus::Closed:
        case LifecycleStatus::Determined:
        case LifecycleStatus::Settled:
        case LifecycleStatus::Delisted:
            return true;
        default:
            return false;
    }
}

}  // namespace

SeriesWatcher::SeriesWatcher() : SeriesWatcher(Options{}) {}

SeriesWatcher::SeriesWatcher(Options options) : options_(options) {}

void SeriesWatcher::apply(const InstrumentLifecycle& event) {
    const auto parsed = parse_ticker(event.symbol);
    if (!parsed) return;  // not a dated window, so not a series that rolls

    std::lock_guard lock(mutex_);
    auto& windows = series_[parsed->series];
    auto& window = windows[event.symbol];
    window.symbol = event.symbol;
    if (!event.security_id.empty()) window.security_id = event.security_id;
    window.window_end = parsed->window_end;

    // An Amended is an event, not a state ([MD] Q14): it says something about
    // the instrument changed, not that it stopped trading. Anything else the
    // feed names is the state now.
    if (event.status != LifecycleStatus::Amended && event.status != LifecycleStatus::Unknown) {
        window.status = event.status;
        window.status_time = event.transact_time;
    }
}

std::optional<SeriesWindow> SeriesWatcher::current(const std::string& series,
                                                   Clock::time_point now) const {
    std::lock_guard lock(mutex_);
    const auto it = series_.find(series);
    if (it == series_.end()) return std::nullopt;

    std::optional<SeriesWindow> best;
    for (const auto& [symbol, window] : it->second) {
        if (!window.tradable()) continue;
        // A window whose end has gone by is not the one trading now, whatever
        // the last message about it said.
        if (now > window.window_end + options_.past_end_grace) continue;
        // The latest end, not the earliest. If two are somehow open at once,
        // the one that ends later is the one with trading left in it - and
        // the one that ends sooner may be a Closed we never heard.
        if (!best || window.window_end > best->window_end) best = window;
    }
    return best;
}

std::optional<SeriesWindow> SeriesWatcher::previous(const std::string& series) const {
    std::lock_guard lock(mutex_);
    const auto it = series_.find(series);
    if (it == series_.end()) return std::nullopt;

    std::optional<SeriesWindow> best;
    for (const auto& [symbol, window] : it->second) {
        if (!finished(window.status)) continue;
        if (!best || window.window_end > best->window_end) best = window;
    }
    return best;
}

std::vector<SeriesWindow> SeriesWatcher::windows(const std::string& series) const {
    std::lock_guard lock(mutex_);
    std::vector<SeriesWindow> out;
    const auto it = series_.find(series);
    if (it == series_.end()) return out;
    for (const auto& [symbol, window] : it->second) out.push_back(window);
    std::sort(out.begin(), out.end(), [](const SeriesWindow& a, const SeriesWindow& b) {
        return a.window_end > b.window_end;
    });
    return out;
}

std::vector<std::string> SeriesWatcher::series() const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> out;
    out.reserve(series_.size());
    for (const auto& [name, windows] : series_) out.push_back(name);
    return out;
}

std::size_t SeriesWatcher::evict(Clock::time_point now) {
    std::lock_guard lock(mutex_);
    std::size_t dropped = 0;
    for (auto series = series_.begin(); series != series_.end();) {
        auto& windows = series->second;
        for (auto it = windows.begin(); it != windows.end();) {
            // Measured from the window's end rather than its last message: a
            // window that settled and then said nothing more is exactly the
            // one worth forgetting, and its end is the time anyone would
            // reason about.
            const bool over = finished(it->second.status) &&
                              now > it->second.window_end + options_.remember_finished;
            if (over) {
                it = windows.erase(it);
                ++dropped;
            } else {
                ++it;
            }
        }
        // A series with nothing left is not a series we know anything about.
        if (windows.empty()) {
            series = series_.erase(series);
        } else {
            ++series;
        }
    }
    return dropped;
}

}  // namespace cpm::md
