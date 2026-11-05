#include "cpm/md/market_state.hpp"

#include <algorithm>

namespace cpm::md {

void MarketState::on_anomaly(AnomalyHandler handler) {
    std::lock_guard lock(mutex_);
    on_anomaly_ = std::move(handler);
}

MdCallbacks MarketState::callbacks() {
    MdCallbacks callbacks;
    callbacks.book_snapshot = [this](const BookSnapshot& e) { apply(e); };
    callbacks.book_update = [this](const BookUpdate& e) { apply(e); };
    callbacks.trade = [this](const Trade& e) { apply(e); };
    callbacks.volume = [this](const Volume& e) { apply(e); };
    callbacks.open_interest = [this](const OpenInterest& e) { apply(e); };
    callbacks.lifecycle = [this](const InstrumentLifecycle& e) { apply(e); };
    return callbacks;
}

MarketState::Instrument& MarketState::instrument_for(const std::string& symbol, const std::string& security_id) {
    const auto [it, inserted] = instruments_.try_emplace(symbol);
    Instrument& instrument = it->second;
    if (inserted) {
        instrument.book.on_anomaly([&instrument](const BookAnomaly& anomaly) {
            instrument.pending.push_back(anomaly);
        });
    }
    if (!security_id.empty()) instrument.security_id = security_id;
    return instrument;
}

void MarketState::apply(const BookSnapshot& snapshot) {
    std::vector<BookAnomaly> anomalies;
    {
        std::lock_guard lock(mutex_);
        Instrument& instrument = instrument_for(snapshot.symbol, snapshot.security_id);
        // The first snapshot binds the symbol to its subscription. A second
        // subscription on the same instrument (O7) would otherwise take the
        // book over, and an L1 one would shrink an L2 book to its top level.
        // invalidate_books() unbinds, so a Stage 5 resubscribe takes over.
        if (instrument.book.valid() && instrument.book.md_req_id() != snapshot.md_req_id) {
            // Counted, not flagged: two deliberate subscriptions on one
            // instrument duplicate every message there is, which is normal
            // (O7). An owner that unsubscribed WITHOUT unbinding lands here
            // too and is a defect, but only the owner can tell the two
            // apart - Trader::tick watches these counters for exactly that.
            ++ignored_foreign_snapshots_;
        } else {
            instrument.book.apply(snapshot);
            anomalies.swap(instrument.pending);
        }
    }
    dispatch(std::move(anomalies));
}

void MarketState::apply(const BookUpdate& update) {
    std::vector<BookAnomaly> anomalies;
    {
        std::lock_guard lock(mutex_);
        Instrument& instrument = instrument_for(update.symbol, update.security_id);
        // No snapshot yet, so there is no book to apply this to. Normal while a
        // subscription is being established, and on every Stage 5 recovery:
        // the gateway's in-flight updates arrive after invalidate_books() and
        // before the fresh snapshot. Counted, not flagged.
        if (!instrument.book.valid()) {
            ++updates_awaiting_snapshot_;
            return;
        }
        // O7: the gateway sends every update once per subscription. Applying a
        // second subscription's copy would double-count, and an L1 copy would
        // delete levels an L2 book still holds, so only the subscription that
        // delivered the binding snapshot may update the book.
        if (instrument.book.md_req_id() != update.md_req_id) {
            ++ignored_foreign_updates_;
        } else {
            instrument.book.apply(update);
            anomalies.swap(instrument.pending);
        }
    }
    dispatch(std::move(anomalies));
}

void MarketState::apply(const Trade& trade) {
    std::lock_guard lock(mutex_);
    Instrument& instrument = instrument_for(trade.symbol, trade.security_id);
    if (trade.entry_id.empty()) {
        ++instrument.trades_without_id;
    } else if (!instrument.recent_trade_id_set.insert(trade.entry_id).second) {
        // The same execution under an overlapping subscription (O7).
        ++instrument.duplicate_trades;
        return;
    } else {
        instrument.recent_trade_ids.push_back(trade.entry_id);
        if (instrument.recent_trade_ids.size() > kTradeDedupWindow) {
            instrument.recent_trade_id_set.erase(instrument.recent_trade_ids.front());
            instrument.recent_trade_ids.pop_front();
        }
    }
    ++instrument.trades;
    instrument.last_trade = TradeSummary{trade.yes_price, trade.qty, trade.aggressor, trade.entry_id,
                                         trade.transact_time};
}

void MarketState::apply(const Volume& volume) {
    std::lock_guard lock(mutex_);
    // Cumulative contracts traded, absolute in every message (spec §5.1/§5.2).
    instrument_for(volume.symbol, volume.security_id).volume = volume.contracts;
}

void MarketState::apply(const OpenInterest& open_interest) {
    std::lock_guard lock(mutex_);
    instrument_for(open_interest.symbol, open_interest.security_id).open_interest = open_interest.contracts;
}

void MarketState::apply(const InstrumentLifecycle& lifecycle) {
    std::lock_guard lock(mutex_);
    Instrument& instrument = instrument_for(lifecycle.symbol, lifecycle.security_id);
    // O10: f has no MDEntryID, and overlapping subscriptions deliver the same
    // transition once each (a per-instrument and an all-instrument lifecycle
    // request both fired at the 06:15 roll). The same 326 at the same
    // TransactTime IS the same event, so it is not a second transition.
    // Q14's "record every transition, reject none" still holds for real ones.
    if (!instrument.history.empty() && instrument.history.back().first == lifecycle.status &&
        instrument.history.back().second == lifecycle.transact_time &&
        instrument.last_status_code == lifecycle.status_code) {
        ++instrument.duplicate_status_events;
        return;
    }
    instrument.last_status_code = lifecycle.status_code;
    // Q14: CPM's meanings only, Amended is an event rather than a state, and
    // every transition is recorded without any being rejected.
    if (instrument.history.size() >= kHistoryLimit) instrument.history.erase(instrument.history.begin());
    instrument.history.emplace_back(lifecycle.status, lifecycle.transact_time);

    if (lifecycle.status == LifecycleStatus::Amended) {
        ++instrument.amendments;
        return;
    }
    if (lifecycle.status == LifecycleStatus::Unknown) {
        // The decoder already reported the undefined code; keep the last known
        // trading state rather than losing it.
        ++instrument.unknown_status_events;
        return;
    }
    instrument.status = lifecycle.status;
    instrument.status_code = lifecycle.status_code;
    instrument.status_time = lifecycle.transact_time;
}

void MarketState::invalidate_books() {
    std::lock_guard lock(mutex_);
    for (auto& [symbol, instrument] : instruments_) {
        instrument.book.invalidate();
    }
}

bool MarketState::invalidate_book(const std::string& symbol) {
    std::lock_guard lock(mutex_);
    const auto it = instruments_.find(symbol);
    if (it == instruments_.end()) return false;
    it->second.book.invalidate();
    return true;
}

std::size_t MarketState::evict_finished(std::chrono::seconds older_than, Clock::time_point now) {
    std::lock_guard lock(mutex_);
    std::size_t dropped = 0;
    for (auto it = instruments_.begin(); it != instruments_.end();) {
        const auto& instrument = it->second;
        const bool finished = instrument.status == LifecycleStatus::Settled ||
                              instrument.status == LifecycleStatus::Delisted;
        // status_time is the exchange's clock; a zero one means we never saw a
        // lifecycle event, so there is nothing to age out.
        const bool quiet = instrument.status_time != Clock::time_point{} &&
                           now - instrument.status_time >= older_than;
        if (finished && quiet) {
            it = instruments_.erase(it);
            ++dropped;
        } else {
            ++it;
        }
    }
    return dropped;
}

std::vector<std::string> MarketState::symbols() const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> out;
    out.reserve(instruments_.size());
    for (const auto& [symbol, instrument] : instruments_) out.push_back(symbol);
    return out;
}

std::optional<InstrumentView> MarketState::instrument(const std::string& symbol) const {
    std::lock_guard lock(mutex_);
    const auto it = instruments_.find(symbol);
    if (it == instruments_.end()) return std::nullopt;
    return view_of(it->first, it->second);
}

std::vector<InstrumentView> MarketState::instruments() const {
    std::lock_guard lock(mutex_);
    std::vector<InstrumentView> out;
    out.reserve(instruments_.size());
    for (const auto& [symbol, instrument] : instruments_) out.push_back(view_of(symbol, instrument));
    return out;
}

std::size_t MarketState::ignored_foreign_updates() const {
    std::lock_guard lock(mutex_);
    return ignored_foreign_updates_;
}

std::size_t MarketState::ignored_foreign_snapshots() const {
    std::lock_guard lock(mutex_);
    return ignored_foreign_snapshots_;
}

std::size_t MarketState::updates_awaiting_snapshot() const {
    std::lock_guard lock(mutex_);
    return updates_awaiting_snapshot_;
}

BookCounters MarketState::totals() const {
    std::lock_guard lock(mutex_);
    BookCounters total;
    for (const auto& [symbol, instrument] : instruments_) {
        const auto& counters = instrument.book.counters();
        total.snapshots += counters.snapshots;
        total.updates += counters.updates;
        total.level_changes += counters.level_changes;
        total.price_levels_checked += counters.price_levels_checked;
        total.anomalies += counters.anomalies;
        for (std::size_t i = 0; i < std::size(total.by_kind); ++i) total.by_kind[i] += counters.by_kind[i];
    }
    return total;
}

InstrumentView MarketState::view_of(const std::string& symbol, const Instrument& instrument) const {
    InstrumentView view;
    view.symbol = symbol;
    view.security_id = instrument.security_id;
    view.book_valid = instrument.book.valid();
    view.book_md_req_id = instrument.book.md_req_id();
    view.yes = instrument.book.levels(BookSide::Yes);
    view.no = instrument.book.levels(BookSide::No);
    view.implied_yes_ask = instrument.book.implied_yes_ask();
    view.implied_no_ask = instrument.book.implied_no_ask();
    view.crossed_or_locked = instrument.book.crossed_or_locked();
    view.book_time = instrument.book.last_update();
    view.book_counters = instrument.book.counters();
    view.last_trade = instrument.last_trade;
    view.trades = instrument.trades;
    view.duplicate_trades = instrument.duplicate_trades;
    view.trades_without_id = instrument.trades_without_id;
    view.volume = instrument.volume;
    view.open_interest = instrument.open_interest;
    view.status = instrument.status;
    view.status_code = instrument.status_code;
    view.status_time = instrument.status_time;
    view.amendments = instrument.amendments;
    view.unknown_status_events = instrument.unknown_status_events;
    view.duplicate_status_events = instrument.duplicate_status_events;
    view.history = instrument.history;
    return view;
}

void MarketState::dispatch(std::vector<BookAnomaly> anomalies) const {
    if (anomalies.empty()) return;
    AnomalyHandler handler;
    {
        std::lock_guard lock(mutex_);
        handler = on_anomaly_;
    }
    if (!handler) return;
    for (const auto& anomaly : anomalies) handler(anomaly);
}

}  // namespace cpm::md
