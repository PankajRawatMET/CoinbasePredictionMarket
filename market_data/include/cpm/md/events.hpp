#pragma once

#include "cpm/common/fixed.hpp"

#include <chrono>
#include <string>
#include <vector>

// Decoded market data, as plain structs with no QuickFIX in them (design §8).
// Prices are thousandths and sizes hundredths; see cpm/common/fixed.hpp.
namespace cpm::md {

using Clock = std::chrono::system_clock;

// Every event carries the gateway's TransactTime(60). The spec marks that
// field optional, so a default-constructed (epoch) time means the gateway did
// not send one; the sandbox always does.

// Both book sides are bids (spec §4.1): 269=0 the YES bids, 269=1 the NO bids,
// each quoted in its own contract's price. The implied YES ask is
// 1.000 - best NO bid.
enum class BookSide { Yes, No };

// 279: New(0) and Change(1) both mean "this price level now holds this
// absolute size"; Delete(2) removes the level, as does a Set with size 0.
enum class BookAction { Set, Delete };

// 2446 AggressorSide as the taker's side of the YES contract (Q10, confirmed
// by O4): 1 = bought YES, 2 = sold YES (= bought NO), 0 = opening/uncrossing.
enum class Aggressor { None, YesTaker, NoTaker };

// 326 SecurityTradingStatus with CPM's meanings (design §8). A code the spec
// does not define decodes as Unknown, with the code kept in `status_code`.
enum class LifecycleStatus { Unknown, Listed, Open, Pause, Closed, Determined, Settled, Amended, Delisted };

// "yyyyMMdd-HH:mm:ss.ffffff" in UTC, the way the gateway writes TransactTime.
std::string to_utc_string(Clock::time_point time);

const char* to_string(BookSide side);
const char* to_string(BookAction action);
const char* to_string(Aggressor aggressor);
const char* to_string(LifecycleStatus status);

struct BookLevel {
    Price price = 0;
    Qty qty = 0;
    bool operator==(const BookLevel&) const = default;
};

struct BookLevelChange {
    BookSide side = BookSide::Yes;
    BookAction action = BookAction::Set;
    Price price = 0;
    Qty qty = 0;   // absolute size now at this price; never a delta
    int level = 0; // MDPriceLevel(1023), 0 when absent. Not used for book state (Q7).
    // 279=0 (New) rather than 279=1 (Change). Both set the level, so the book
    // applies them identically, but the distinction is a consistency check:
    // a New should name a price the book does not hold, and a Change one it does.
    bool was_new = false;
    bool operator==(const BookLevelChange&) const = default;
};

// W with 269=0/1 entries: authoritative, so it replaces the local book.
struct BookSnapshot {
    std::string md_req_id;
    std::string symbol;
    std::string security_id;
    std::vector<BookLevel> yes;  // best (highest) price first, as sent
    std::vector<BookLevel> no;
    Clock::time_point transact_time{};
};

// X with 269=0/1 entries: the changes one message carried for one instrument.
struct BookUpdate {
    std::string md_req_id;
    std::string symbol;
    std::string security_id;
    std::vector<BookLevelChange> changes;
    Clock::time_point transact_time{};
};

// X with 269=2. There is no trade snapshot (spec §4.2).
struct Trade {
    std::string md_req_id;
    std::string symbol;
    std::string security_id;
    Price yes_price = 0;  // 270 is the YES price; the NO price is 1.000 - this
    Qty qty = 0;
    Aggressor aggressor = Aggressor::None;
    std::string entry_id;  // MDEntryID(278), unique per execution
    Clock::time_point transact_time{};
};

// 269=B cumulative contracts traded, from a W snapshot or an X update.
struct Volume {
    std::string md_req_id;
    std::string symbol;
    std::string security_id;
    Qty contracts = 0;
    bool snapshot = false;
    Clock::time_point transact_time{};
};

// 269=C open contracts, from a W snapshot or an X update.
struct OpenInterest {
    std::string md_req_id;
    std::string symbol;
    std::string security_id;
    Qty contracts = 0;
    bool snapshot = false;
    Clock::time_point transact_time{};
};

// SecurityStatus(f). No snapshot is sent, so an instrument's state is unknown
// until its first f arrives (Q13).
struct InstrumentLifecycle {
    std::string md_req_id;  // echo of 324 if the gateway sends it; usually empty
    std::string symbol;
    std::string security_id;
    LifecycleStatus status = LifecycleStatus::Unknown;
    int status_code = 0;  // 326 as sent
    std::string text;     // 58, if any
    Clock::time_point transact_time{};
};

// MarketDataRequestReject(Y) or BusinessMessageReject(j).
struct MdReject {
    enum class Kind { MarketDataRequest, BusinessMessage };
    Kind kind = Kind::MarketDataRequest;
    std::string md_req_id;    // 262; empty on j
    std::string reason_code;  // 281 on Y, 380 on j, as sent
    std::string text;         // 58, which distinguishes cases sharing a code
};

// A message that could not be decoded: a missing required field, a malformed
// decimal or an unknown entry type. Reported instead of thrown, because
// decoding runs on a QuickFIX callback thread.
struct DecodeError {
    std::string reason;
    std::string msg_type;   // 35
    std::string md_req_id;  // 262, if the message had one
    std::string raw;        // the message with SOH shown as '|'
};

}  // namespace cpm::md
