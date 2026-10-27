#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <vector>

// Simulated CPM market for md_sim: plain C++, no QuickFIX. The FIX layer turns
// the changes returned here into W/X/f messages.
namespace cpm::md::sim {

using Clock = std::chrono::system_clock;
using Price = std::int64_t;  // thousandths: 0.440 -> 440 (design §8)
using Qty = std::int64_t;    // hundredths:  2     -> 200

inline constexpr Price kPriceOne = 1000;

// Both book sides are bids (spec §4.1): 269=0 YES bids, 269=1 NO bids.
enum class Side { Yes, No };

// SecurityTradingStatus(326), CPM meanings (spec §6).
enum class Status : int {
    Pause = 2,
    Open = 17,
    Closed = 18,
    Listed = 21,
    Amended = 26,
    Determined = 27,
    Settled = 28,
    Delisted = 29,
};

struct Level {
    Price price = 0;
    Qty qty = 0;
    bool operator==(const Level&) const = default;
};

struct InstrumentSpec {
    std::string symbol;
    std::string security_id;
    Price fair = 500;  // centre of the generated quotes
    int levels = 4;    // initial depth per side
};

struct Instrument {
    std::string symbol;
    std::string security_id;
    std::vector<Level> yes;  // best (highest) first, at most kMaxLevels
    std::vector<Level> no;   // best (highest) first, at most kMaxLevels
    Qty volume = 0;
    Qty open_interest = 0;
    Status status = Status::Open;
    Price fair = 500;
    Clock::time_point last_book_change{};

    const std::vector<Level>& side(Side s) const { return s == Side::Yes ? yes : no; }
};

// One level change, with the absolute quantity left at that price (spec §5.2:
// replace the level, never add). level is the 1-based position after a New or
// Change; 0 for a Delete.
struct BookDelta {
    enum class Action { New = 0, Change = 1, Delete = 2 };
    Side side = Side::Yes;
    Action action = Action::New;
    Price price = 0;
    Qty qty = 0;
    int level = 0;
};

// A public trade. yes_price is the YES price (spec §5: NO = 1.000 - px).
// aggressor: 1 = bought YES (hit a NO bid), 2 = sold YES (hit a YES bid) [OBS O4].
struct Trade {
    Price yes_price = 0;
    Qty qty = 0;
    int aggressor = 1;
    std::string entry_id;  // MDEntryID(278), unique per execution
};

// Everything one market event changed on one instrument. Trade and book share
// the event time, as on the sandbox [OBS O4].
struct Step {
    std::string symbol;
    std::string security_id;
    Clock::time_point time{};
    std::vector<BookDelta> book;
    std::optional<Level> best_before[2];  // [Side::Yes], [Side::No]
    std::optional<Level> best_after[2];
    std::optional<Trade> trade;
    bool stats_changed = false;
    Qty volume = 0;
    Qty open_interest = 0;

    bool empty() const { return book.empty() && !trade && !stats_changed; }
};

struct StatusEvent {
    std::string symbol;
    std::string security_id;
    Status status = Status::Open;
    Clock::time_point time{};
};

// Result of closing a rolling instrument and listing its successor, in the
// order the sandbox sends it [OBS O6]: Closed, book cleared, Determined,
// then Listed and Open for the new instrument.
struct Roll {
    std::vector<StatusEvent> closing;  // Closed
    Step cleared;                      // deletes of every resting level
    std::vector<StatusEvent> after;    // Determined, Listed (new), Open (new)
};

class Market {
public:
    static constexpr std::size_t kMaxLevels = 10;  // spec §4: 10 levels per side

    Market(std::vector<InstrumentSpec> instruments, std::uint64_t seed, Clock::time_point now);

    // Known instruments (any status). A settled instrument is removed and
    // becomes unknown, as on the sandbox [OBS O6].
    const Instrument* find(const std::string& symbol) const;
    std::vector<std::string> symbols() const;

    // One random event (add, cancel or trade) on a random Open instrument.
    // Books never cross or lock and never exceed kMaxLevels per side.
    Step step(Clock::time_point now);

    Roll roll(const std::string& symbol, InstrumentSpec next, Clock::time_point now);
    StatusEvent settle(const std::string& symbol, Clock::time_point now);

private:
    Instrument& pick_open();
    void add_order(Instrument& inst, Step& out);
    void cancel_order(Instrument& inst, Step& out);
    void trade(Instrument& inst, Step& out);
    Instrument make(const InstrumentSpec& spec, Clock::time_point now);
    int uniform(int lo, int hi);

    std::mt19937_64 rng_;
    std::map<std::string, Instrument> instruments_;
    std::uint64_t next_entry_id_ = 900000000000000001ULL;
};

// Book helpers (also used by tests to replay deltas).
// Applies a delta to one side, keeping it sorted best (highest) first.
void apply_delta(std::vector<Level>& side, const BookDelta& delta);
// Price and quantity as the sandbox sends them [OBS O3/O4]: no trailing zeros,
// e.g. 440 -> "0.44", 400 -> "0.4", 435 -> "0.435"; 200 -> "2", 250 -> "2.5".
std::string format_price(Price p);
std::string format_qty(Qty q);

}  // namespace cpm::md::sim
