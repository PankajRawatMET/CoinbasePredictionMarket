#pragma once

#include "cpm/md/book.hpp"
#include "cpm/md/md_client.hpp"

#include <cstddef>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cpm::md {

// The last public trade on an instrument (spec §5: 270 is the YES price).
struct TradeSummary {
    Price yes_price = 0;
    Qty qty = 0;
    Aggressor aggressor = Aggressor::None;
    std::string entry_id;
    Clock::time_point time{};
};

// One instrument's state, copied out for reading so callers never hold a lock.
struct InstrumentView {
    std::string symbol;
    std::string security_id;

    bool book_valid = false;
    std::string book_md_req_id;  // the subscription this book is built from (O7)
    std::vector<BookLevel> yes;  // best (highest) price first
    std::vector<BookLevel> no;
    std::optional<Price> implied_yes_ask;
    std::optional<Price> implied_no_ask;
    bool crossed_or_locked = false;
    Clock::time_point book_time{};
    BookCounters book_counters;

    std::optional<TradeSummary> last_trade;
    std::size_t trades = 0;
    std::size_t duplicate_trades = 0;   // the same 278 twice (O7: one copy per subscription)
    std::size_t trades_without_id = 0;  // no 278, so they cannot be deduplicated

    std::optional<Qty> volume;
    std::optional<Qty> open_interest;

    // Unknown until the first SecurityStatus(f) arrives (Q13).
    LifecycleStatus status = LifecycleStatus::Unknown;
    int status_code = 0;
    Clock::time_point status_time{};
    std::size_t amendments = 0;              // Amended(26) is an event, not a state (Q14)
    std::size_t unknown_status_events = 0;   // a 326 the spec does not define
    std::size_t duplicate_status_events = 0; // the same transition twice (O10)
    std::vector<std::pair<LifecycleStatus, Clock::time_point>> history;
};

// Per-instrument state built from decoded events: book, last trade, volume,
// open interest and lifecycle. Plain C++ with no QuickFIX and no FIX session;
// feed it with callbacks() or by calling apply() directly.
//
// Thread safety: apply() runs on the QuickFIX callback thread while readers
// run elsewhere, so everything is guarded and reads return copies. Anomaly
// handlers are called after the lock is released.
class MarketState {
public:
    // How many MDEntryIDs are remembered per instrument for trade dedup.
    static constexpr std::size_t kTradeDedupWindow = 1024;
    // How many lifecycle transitions are kept per instrument.
    static constexpr std::size_t kHistoryLimit = 64;

    using AnomalyHandler = std::function<void(const BookAnomaly&)>;

    // Register before the feed starts. Called without the state locked, so the
    // handler may read the state back.
    void on_anomaly(AnomalyHandler handler);

    // Callbacks to hand to MdClient::on_market_data.
    MdCallbacks callbacks();

    void apply(const BookSnapshot& snapshot);
    void apply(const BookUpdate& update);
    void apply(const Trade& trade);
    void apply(const Volume& volume);
    void apply(const OpenInterest& open_interest);
    void apply(const InstrumentLifecycle& lifecycle);

    // Stage 5: every book must be rebuilt from a fresh snapshot. Trades,
    // volume, open interest and lifecycle are kept: they have no snapshot.
    void invalidate_books();

    // Drops instruments that have finished trading and have been quiet for
    // `older_than`. Without this a long run accumulates every instrument it
    // ever saw, and the sandbox lists on the order of a hundred new rolling
    // windows a day (O2/O6), each holding a book, a lifecycle history and up
    // to kTradeDedupWindow trade ids. Returns how many were dropped.
    //
    // Only Settled and Delisted instruments are eligible: Closed and
    // Determined still settle later, and anything still trading is kept
    // whatever its age.
    std::size_t evict_finished(std::chrono::seconds older_than, Clock::time_point now = Clock::now());

    std::vector<std::string> symbols() const;
    std::optional<InstrumentView> instrument(const std::string& symbol) const;
    std::vector<InstrumentView> instruments() const;

    // Messages ignored because the symbol's book belongs to another
    // subscription (O7: overlapping subscriptions duplicate every message).
    std::size_t ignored_foreign_updates() const;
    std::size_t ignored_foreign_snapshots() const;
    // Updates dropped because no snapshot has bound the symbol's book yet.
    // Expected while a subscription is being established or re-established:
    // the gateway's in-flight updates outlive an unsubscribe, so this is a
    // counter rather than an anomaly.
    std::size_t updates_awaiting_snapshot() const;
    // Book counters summed over every instrument.
    BookCounters totals() const;

private:
    struct Instrument {
        OrderBook book;
        std::vector<BookAnomaly> pending;  // filled by the book, drained by apply()
        std::string security_id;
        std::optional<TradeSummary> last_trade;
        std::size_t trades = 0;
        std::size_t duplicate_trades = 0;
        std::size_t trades_without_id = 0;
        std::deque<std::string> recent_trade_ids;
        std::unordered_set<std::string> recent_trade_id_set;
        std::optional<Qty> volume;
        std::optional<Qty> open_interest;
        LifecycleStatus status = LifecycleStatus::Unknown;
        int status_code = 0;
        Clock::time_point status_time{};
        std::size_t amendments = 0;
        std::size_t unknown_status_events = 0;
        std::size_t duplicate_status_events = 0;
        int last_status_code = -1;
        std::vector<std::pair<LifecycleStatus, Clock::time_point>> history;
    };

    // Finds or creates the instrument, with its book wired to report into
    // `pending`. std::map nodes are stable, so the capture stays valid.
    Instrument& instrument_for(const std::string& symbol, const std::string& security_id);
    InstrumentView view_of(const std::string& symbol, const Instrument& instrument) const;
    void dispatch(std::vector<BookAnomaly> anomalies) const;

    mutable std::mutex mutex_;
    std::map<std::string, Instrument> instruments_;
    std::size_t ignored_foreign_updates_ = 0;
    std::size_t ignored_foreign_snapshots_ = 0;
    std::size_t updates_awaiting_snapshot_ = 0;
    AnomalyHandler on_anomaly_;
};

}  // namespace cpm::md
