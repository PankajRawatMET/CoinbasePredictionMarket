#include "cpm/md/book.hpp"

#include <algorithm>

namespace cpm::md {
namespace {

// Order prices exclude 0 and 1 (O5); only settlement values may equal either.
bool in_range(Price price) { return price > 0 && price < kPriceOne; }

}  // namespace

const char* to_string(BookAnomaly::Kind kind) {
    switch (kind) {
        case BookAnomaly::Kind::NewAtExistingPrice: return "new at an existing price";
        case BookAnomaly::Kind::ChangeAtMissingPrice: return "change at a missing price";
        case BookAnomaly::Kind::DeleteOfMissingPrice: return "delete of a missing price";
        case BookAnomaly::Kind::PriceLevelMismatch: return "MDPriceLevel(1023) mismatch";
        case BookAnomaly::Kind::DepthExceeded: return "depth over 10 levels";
        case BookAnomaly::Kind::PriceOutOfRange: return "price out of range";
        case BookAnomaly::Kind::CrossedOrLocked: return "crossed or locked book";
        case BookAnomaly::Kind::UpdateBeforeSnapshot: return "update before any snapshot";
    }
    return "unknown";
}

void OrderBook::on_anomaly(AnomalyHandler handler) { on_anomaly_ = std::move(handler); }

void OrderBook::apply(const BookSnapshot& snapshot) {
    symbol_ = snapshot.symbol;
    security_id_ = snapshot.security_id;
    md_req_id_ = snapshot.md_req_id;
    yes_.clear();
    no_.clear();
    ++counters_.snapshots;
    // The snapshot's own order is the gateway's; rebuilding it level by level
    // keeps one place that enforces best-first and the price range.
    for (const auto side : {BookSide::Yes, BookSide::No}) {
        for (const auto& level : (side == BookSide::Yes ? snapshot.yes : snapshot.no)) {
            if (!check_price(side, level.price)) continue;
            set_level(side, level.price, level.qty);
        }
        check_invariants(side);
    }
    valid_ = true;
    last_update_ = snapshot.transact_time;
}

void OrderBook::apply(const BookUpdate& update) {
    if (!valid_) {
        report(BookAnomaly::Kind::UpdateBeforeSnapshot, BookSide::Yes, 0,
               "symbol " + update.symbol + " has no snapshot yet");
        return;
    }
    if (symbol_.empty()) symbol_ = update.symbol;
    ++counters_.updates;
    for (const auto& change : update.changes) {
        if (!check_price(change.side, change.price)) continue;
        auto& side = side_of(change.side);
        const auto existing = std::find_if(side.begin(), side.end(),
                                           [&](const BookLevel& l) { return l.price == change.price; });
        const bool present = existing != side.end();

        // A delete, or a set to zero, removes the level (design §8).
        if (change.action == BookAction::Delete || change.qty == 0) {
            if (!present) {
                report(BookAnomaly::Kind::DeleteOfMissingPrice, change.side, change.price);
                continue;
            }
            remove_level(change.side, change.price);
            ++counters_.level_changes;
            continue;
        }

        // 279=0 should name a new price and 279=1 an existing one (O9 saw no
        // exception). Either way the absolute size is applied: the feed is the
        // authority, not us.
        if (change.was_new && present) {
            report(BookAnomaly::Kind::NewAtExistingPrice, change.side, change.price,
                   "279=0 but the book already holds " + format_price(change.price));
        } else if (!change.was_new && !present) {
            report(BookAnomaly::Kind::ChangeAtMissingPrice, change.side, change.price,
                   "279=1 but the book does not hold " + format_price(change.price));
        }
        set_level(change.side, change.price, change.qty);
        ++counters_.level_changes;

        // 1023 is the price's position after the change (Q7, confirmed by O9),
        // which makes it a free consistency check on every update.
        if (change.level > 0) {
            ++counters_.price_levels_checked;
            const auto& after = side_of(change.side);
            const auto position = std::find_if(after.begin(), after.end(), [&](const BookLevel& l) {
                                      return l.price == change.price;
                                  }) - after.begin() + 1;
            if (position != change.level) {
                report(BookAnomaly::Kind::PriceLevelMismatch, change.side, change.price,
                       "1023=" + std::to_string(change.level) + " but the price sits at " +
                           std::to_string(position) + " of " + std::to_string(after.size()));
            }
        }
        check_invariants(change.side);
    }
    last_update_ = update.transact_time;
}

void OrderBook::invalidate() {
    valid_ = false;
    // Unbind too: the next snapshot, whichever subscription it comes from,
    // owns the book again (Stage 5 resubscribes with a fresh MDReqID).
    md_req_id_.clear();
    yes_.clear();
    no_.clear();
}

const std::vector<BookLevel>& OrderBook::levels(BookSide side) const {
    return side == BookSide::Yes ? yes_ : no_;
}

std::optional<BookLevel> OrderBook::best(BookSide side) const {
    const auto& levels_ = levels(side);
    if (levels_.empty()) return std::nullopt;
    return levels_.front();
}

std::optional<Price> OrderBook::implied_yes_ask() const {
    const auto no_bid = best(BookSide::No);
    if (!no_bid) return std::nullopt;
    return kPriceOne - no_bid->price;
}

std::optional<Price> OrderBook::implied_no_ask() const {
    const auto yes_bid = best(BookSide::Yes);
    if (!yes_bid) return std::nullopt;
    return kPriceOne - yes_bid->price;
}

bool OrderBook::crossed_or_locked() const {
    const auto yes_bid = best(BookSide::Yes);
    const auto no_bid = best(BookSide::No);
    return yes_bid && no_bid && yes_bid->price + no_bid->price >= kPriceOne;
}

void OrderBook::report(BookAnomaly::Kind kind, BookSide side, Price price, std::string detail) {
    ++counters_.anomalies;
    ++counters_.by_kind[static_cast<std::size_t>(kind)];
    if (on_anomaly_) on_anomaly_({kind, symbol_, side, price, std::move(detail)});
}

std::vector<BookLevel>& OrderBook::side_of(BookSide side) { return side == BookSide::Yes ? yes_ : no_; }

void OrderBook::set_level(BookSide side, Price price, Qty qty) {
    auto& levels_ = side_of(side);
    const auto at = std::lower_bound(levels_.begin(), levels_.end(), price,
                                     [](const BookLevel& l, Price p) { return l.price > p; });
    if (at != levels_.end() && at->price == price) {
        at->qty = qty;
        return;
    }
    levels_.insert(at, BookLevel{price, qty});
}

void OrderBook::remove_level(BookSide side, Price price) {
    auto& levels_ = side_of(side);
    const auto at = std::find_if(levels_.begin(), levels_.end(), [&](const BookLevel& l) { return l.price == price; });
    if (at != levels_.end()) levels_.erase(at);
}

bool OrderBook::check_price(BookSide side, Price price) {
    if (in_range(price)) return true;
    report(BookAnomaly::Kind::PriceOutOfRange, side, price, "level dropped");
    return false;
}

void OrderBook::check_invariants(BookSide side) {
    if (levels(side).size() > kMaxLevels) {
        report(BookAnomaly::Kind::DepthExceeded, side, 0,
               std::to_string(levels(side).size()) + " levels on the " + to_string(side) + " side");
    }
    if (crossed_or_locked()) {
        const auto yes_bid = best(BookSide::Yes);
        const auto no_bid = best(BookSide::No);
        report(BookAnomaly::Kind::CrossedOrLocked, side, yes_bid->price,
               "best YES " + format_price(yes_bid->price) + " + best NO " + format_price(no_bid->price) + " >= 1.000");
    }
}

}  // namespace cpm::md
