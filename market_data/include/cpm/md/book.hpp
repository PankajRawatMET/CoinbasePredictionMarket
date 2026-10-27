#pragma once

#include "cpm/md/events.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace cpm::md {

// Something in the feed that contradicts the spec or the book's own state.
// Reported, never thrown: the feed is the gateway's, not ours, and Stage 5
// decides what to do about it.
struct BookAnomaly {
    enum class Kind {
        NewAtExistingPrice,    // 279=0 where the book already has that price
        ChangeAtMissingPrice,  // 279=1 where it does not
        DeleteOfMissingPrice,  // 279=2 for a price the book does not hold
        PriceLevelMismatch,    // 1023 is not the position after the change (Q7, O9)
        DepthExceeded,         // more than 10 levels on one side (spec §4)
        PriceOutOfRange,       // outside 0.001..0.999 (O5); the level is dropped
        CrossedOrLocked,       // best YES bid + best NO bid >= 1.000 (Q9)
        UpdateBeforeSnapshot,  // an X arrived before any W, so there is no book
    };

    Kind kind;
    std::string symbol;
    BookSide side = BookSide::Yes;
    Price price = 0;
    std::string detail;
};

const char* to_string(BookAnomaly::Kind kind);

// How often each kind has been seen, for assertions and for a feed-health line.
struct BookCounters {
    std::size_t snapshots = 0;
    std::size_t updates = 0;
    std::size_t level_changes = 0;
    std::size_t price_levels_checked = 0;  // entries that carried 1023
    std::size_t anomalies = 0;
    std::size_t by_kind[8] = {};

    std::size_t count(BookAnomaly::Kind kind) const { return by_kind[static_cast<std::size_t>(kind)]; }
};

// One instrument's book for ONE subscription. Both sides are bids (spec §4.1):
// 269=0 the YES bids, 269=1 the NO bids, each in its own contract's price.
//
// A book belongs to a single MDReqID because the gateway sends every update
// once per subscription (O7), and an L1 subscription's "delete the old best"
// would wrongly remove a level that an L2 book still holds. MarketState binds
// each symbol to the subscription whose snapshot it last applied.
class OrderBook {
public:
    static constexpr std::size_t kMaxLevels = 10;  // spec §4: 10 levels per side

    using AnomalyHandler = std::function<void(const BookAnomaly&)>;

    // Called for every anomaly, on the thread that applies the event.
    void on_anomaly(AnomalyHandler handler);

    // A snapshot is authoritative, so it replaces both sides (design §8).
    void apply(const BookSnapshot& snapshot);
    // Each change sets the absolute size at a price, or removes the level.
    void apply(const BookUpdate& update);

    // Stage 5: after a gap, a disconnect or a stale feed the book is no longer
    // trustworthy and must be rebuilt from a fresh snapshot. This also unbinds
    // it from its subscription, so the next snapshot takes ownership.
    void invalidate();
    bool valid() const { return valid_; }

    const std::vector<BookLevel>& levels(BookSide side) const;
    std::optional<BookLevel> best(BookSide side) const;
    std::size_t depth(BookSide side) const { return levels(side).size(); }

    // The YES ask implied by the best NO bid, and the NO ask implied by the
    // best YES bid (spec §5: "best NO bid 0.380 -> yes_ask = 0.620").
    std::optional<Price> implied_yes_ask() const;
    std::optional<Price> implied_no_ask() const;

    // Q9: both sides are bids, so they must sum to less than 1.000.
    bool crossed_or_locked() const;

    const std::string& symbol() const { return symbol_; }
    const std::string& security_id() const { return security_id_; }
    const std::string& md_req_id() const { return md_req_id_; }
    Clock::time_point last_update() const { return last_update_; }
    const BookCounters& counters() const { return counters_; }

private:
    void report(BookAnomaly::Kind kind, BookSide side, Price price, std::string detail = {});
    std::vector<BookLevel>& side_of(BookSide side);
    // Sets or removes one price level, keeping the side best-first.
    void set_level(BookSide side, Price price, Qty qty);
    void remove_level(BookSide side, Price price);
    bool check_price(BookSide side, Price price);
    void check_invariants(BookSide side);

    std::string symbol_;
    std::string security_id_;
    std::string md_req_id_;
    std::vector<BookLevel> yes_;  // best (highest) price first
    std::vector<BookLevel> no_;
    bool valid_ = false;
    Clock::time_point last_update_{};
    BookCounters counters_;
    AnomalyHandler on_anomaly_;
};

}  // namespace cpm::md
