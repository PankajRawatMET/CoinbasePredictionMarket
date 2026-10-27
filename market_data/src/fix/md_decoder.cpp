#include "fix/md_decoder.hpp"

#include "cpm/fix_common/raw_log.hpp"

#include <quickfix/FixFieldNumbers.h>
#include <quickfix/FixValues.h>
#include <quickfix/Group.h>

#include <algorithm>
#include <ctime>
#include <optional>
#include <utility>

namespace cpm::md::fix {
namespace {

namespace F = FIX::FIELD;

std::string field(const FIX::FieldMap& map, int tag) {
    return map.isSetField(tag) ? map.getField(tag) : std::string();
}

std::string shown(const std::string& value) { return value.empty() ? "<missing>" : value; }

bool digits(const std::string& text, std::size_t pos, std::size_t count) {
    return std::all_of(text.begin() + static_cast<std::ptrdiff_t>(pos),
                       text.begin() + static_cast<std::ptrdiff_t>(pos + count),
                       [](char c) { return c >= '0' && c <= '9'; });
}

int number(const std::string& text, std::size_t pos, std::size_t count) {
    return std::stoi(text.substr(pos, count));
}

// TransactTime(60): "yyyyMMdd-HH:mm:ss[.S+]" in UTC, with up to 9 fractional
// digits. The sandbox sends 6 (O3); the spec also allows milliseconds (Q16).
std::optional<Clock::time_point> parse_utc_timestamp(const std::string& text) {
    constexpr std::size_t kBase = 17;  // yyyyMMdd-HH:mm:ss
    if (text.size() < kBase) return std::nullopt;
    if (!digits(text, 0, 8) || text[8] != '-' || !digits(text, 9, 2) || text[11] != ':' || !digits(text, 12, 2) ||
        text[14] != ':' || !digits(text, 15, 2)) {
        return std::nullopt;
    }
    std::int64_t micros = 0;
    if (text.size() > kBase) {
        if (text[kBase] != '.') return std::nullopt;
        std::string fraction = text.substr(kBase + 1);
        if (fraction.empty() || fraction.size() > 9 || !digits(fraction, 0, fraction.size())) return std::nullopt;
        fraction.resize(6, '0');  // to microseconds, dropping anything finer
        micros = std::stoll(fraction);
    }
    std::tm tm{};
    tm.tm_year = number(text, 0, 4) - 1900;
    tm.tm_mon = number(text, 4, 2) - 1;
    tm.tm_mday = number(text, 6, 2);
    tm.tm_hour = number(text, 9, 2);
    tm.tm_min = number(text, 12, 2);
    tm.tm_sec = number(text, 15, 2);
    const std::time_t secs = timegm(&tm);
    if (secs == static_cast<std::time_t>(-1)) return std::nullopt;
    return Clock::from_time_t(secs) + std::chrono::microseconds(micros);
}

LifecycleStatus status_of(std::int64_t code) {
    switch (code) {
        case 2: return LifecycleStatus::Pause;
        case 17: return LifecycleStatus::Open;
        case 18: return LifecycleStatus::Closed;
        case 21: return LifecycleStatus::Listed;
        case 26: return LifecycleStatus::Amended;
        case 27: return LifecycleStatus::Determined;
        case 28: return LifecycleStatus::Settled;
        case 29: return LifecycleStatus::Delisted;
        default: return LifecycleStatus::Unknown;
    }
}

struct Instrument {
    std::string symbol;
    std::string security_id;
};

// One message's worth of decoding. QuickFIX stays inside this class; what
// comes out are the plain structs of cpm/md/events.hpp.
class Decoder {
public:
    Decoder(const FIX::Message& message, std::string_view api_key)
        : message_(message),
          api_key_(api_key),
          type_(field(message.getHeader(), F::MsgType)),
          req_id_(field(message, F::MDReqID)) {}

    DecodedBatch run() {
        try {
            if (type_ == FIX::MsgType_MarketDataSnapshotFullRefresh) {
                snapshot();
            } else if (type_ == FIX::MsgType_MarketDataIncrementalRefresh) {
                incremental();
            } else if (type_ == FIX::MsgType_SecurityStatus) {
                lifecycle();
            } else if (type_ == FIX::MsgType_MarketDataRequestReject) {
                out_.rejects.push_back({MdReject::Kind::MarketDataRequest, req_id_,
                                        field(message_, F::MDReqRejReason), field(message_, F::Text)});
            } else if (type_ == FIX::MsgType_BusinessMessageReject) {
                out_.rejects.push_back({MdReject::Kind::BusinessMessage, {},
                                        field(message_, F::BusinessRejectReason), field(message_, F::Text)});
            }
        } catch (const std::exception& e) {
            // QuickFIX throws on a field it cannot deliver; a decode must not.
            fail(std::string("malformed message: ") + e.what());
        }
        return std::move(out_);
    }

private:
    void fail(const std::string& reason) {
        out_.errors.push_back({reason, type_, req_id_, cpm::fix::format_raw(message_.toString(), api_key_)});
    }

    // 60 is optional in every market data message (spec tables MD:311, MD:364,
    // MD:782), though the sandbox always sends it. Absent leaves the event's
    // time at the epoch; malformed is a protocol error and drops the message.
    std::optional<Clock::time_point> transact_time() {
        const auto text = field(message_, F::TransactTime);
        if (text.empty()) return Clock::time_point{};
        const auto time = parse_utc_timestamp(text);
        if (!time) fail("35=" + type_ + " with malformed TransactTime(60)=" + text);
        return time;
    }

    // X carries Symbol/SecurityID per entry, but the spec's trade examples put
    // them at message level (Q6), so both places are accepted.
    Instrument instrument_of(const FIX::FieldMap& entry) const {
        Instrument inst{field(entry, F::Symbol), field(entry, F::SecurityID)};
        if (inst.symbol.empty()) inst.symbol = field(message_, F::Symbol);
        if (inst.security_id.empty()) inst.security_id = field(message_, F::SecurityID);
        return inst;
    }

    // W, 269=0/1: the book as it now stands, at most 10 levels per side.
    void snapshot() {
        const auto time = transact_time();
        if (!time) return;
        const Instrument inst{field(message_, F::Symbol), field(message_, F::SecurityID)};
        if (inst.symbol.empty()) {
            fail("W without Symbol(55)");
            return;
        }
        BookSnapshot book;
        book.md_req_id = req_id_;
        book.symbol = inst.symbol;
        book.security_id = inst.security_id;
        book.transact_time = *time;

        bool saw_book_entry = false;
        bool book_ok = true;
        FIX::Group entry(F::NoMDEntries, F::MDEntryType);
        const auto count = message_.groupCount(F::NoMDEntries);
        for (unsigned i = 1; i <= count; ++i) {
            message_.getGroup(i, entry);
            const auto entry_type = field(entry, F::MDEntryType);
            if (entry_type == "0" || entry_type == "1") {
                saw_book_entry = true;
                const auto price = price_of(entry, "W book");
                const auto qty = qty_of(entry, "W book");
                if (!price || !qty) {
                    book_ok = false;
                    continue;
                }
                (entry_type == "0" ? book.yes : book.no).push_back({*price, *qty});
            } else if (entry_type == "B" || entry_type == "C") {
                stats(entry, inst, *time, true, entry_type);
            } else {
                fail("W entry with unsupported MDEntryType(269)=" + shown(entry_type));
            }
        }
        // 268=0 is an empty book (O3); a volume/OI snapshot always carries 271.
        if (!book_ok || !(saw_book_entry || count == 0)) return;
        out_.book_snapshots.push_back(std::move(book));
    }

    void incremental() {
        const auto time = transact_time();
        if (!time) return;
        FIX::Group entry(F::NoMDEntries, F::MDUpdateAction);
        for (unsigned i = 1; i <= message_.groupCount(F::NoMDEntries); ++i) {
            message_.getGroup(i, entry);
            const auto entry_type = field(entry, F::MDEntryType);
            const Instrument inst = instrument_of(entry);
            if (inst.symbol.empty()) {
                fail("X entry " + std::to_string(i) + " without Symbol(55)");
                continue;
            }
            if (entry_type == "0" || entry_type == "1") {
                book_change(entry, inst, *time, entry_type == "0" ? BookSide::Yes : BookSide::No);
            } else if (entry_type == "2") {
                trade(entry, inst, *time);
            } else if (entry_type == "B" || entry_type == "C") {
                stats(entry, inst, *time, false, entry_type);
            } else {
                fail("X entry with unsupported MDEntryType(269)=" + shown(entry_type));
            }
        }
    }

    // X, 269=0/1. 271 is the absolute size now at that price, never a delta.
    void book_change(const FIX::FieldMap& entry, const Instrument& inst, Clock::time_point time, BookSide side) {
        const auto action_text = field(entry, F::MDUpdateAction);
        BookLevelChange change;
        change.side = side;
        if (action_text == "0" || action_text == "1") {
            change.action = BookAction::Set;
            change.was_new = action_text == "0";
        } else if (action_text == "2") {
            change.action = BookAction::Delete;
        } else {
            fail("X book entry with MDUpdateAction(279)=" + shown(action_text));
            return;
        }
        const auto price = price_of(entry, "X book");
        if (!price) return;
        change.price = *price;
        // A delete carries 271=0 (O4); treat an absent size on a delete as 0.
        const auto size_text = field(entry, F::MDEntrySize);
        if (change.action == BookAction::Delete && size_text.empty()) {
            change.qty = 0;
        } else {
            const auto qty = qty_of(entry, "X book");
            if (!qty) return;
            change.qty = *qty;
        }
        // 1023 is optional and is not used for book state (Q7), so a bad one
        // is reported but does not drop the change.
        const auto level_text = field(entry, F::MDPriceLevel);
        if (!level_text.empty()) {
            const auto level = cpm::parse_scaled(level_text, 0);
            if (level && *level <= 1000) {
                change.level = static_cast<int>(*level);
            } else {
                fail("X book entry with MDPriceLevel(1023)=" + level_text);
            }
        }
        update_for(inst, time).changes.push_back(change);
    }

    void trade(const FIX::FieldMap& entry, const Instrument& inst, Clock::time_point time) {
        const auto price = price_of(entry, "trade");
        const auto qty = qty_of(entry, "trade");
        if (!price || !qty) return;
        Trade decoded;
        decoded.md_req_id = req_id_;
        decoded.symbol = inst.symbol;
        decoded.security_id = inst.security_id;
        decoded.yes_price = *price;
        decoded.qty = *qty;
        decoded.entry_id = field(entry, F::MDEntryID);
        decoded.transact_time = time;
        const auto aggressor = field(entry, F::AggressorSide);
        if (aggressor == "1") {
            decoded.aggressor = Aggressor::YesTaker;
        } else if (aggressor == "2") {
            decoded.aggressor = Aggressor::NoTaker;
        } else if (!aggressor.empty() && aggressor != "0") {
            fail("trade with AggressorSide(2446)=" + aggressor);
            return;
        }
        out_.trades.push_back(std::move(decoded));
    }

    // 269=B/C: only 271 is set. An absent or zero value is not an error (Q15).
    void stats(const FIX::FieldMap& entry, const Instrument& inst, Clock::time_point time, bool is_snapshot,
               const std::string& entry_type) {
        if (inst.symbol.empty()) {
            fail("269=" + entry_type + " entry without Symbol(55)");
            return;
        }
        const auto size_text = field(entry, F::MDEntrySize);
        Qty contracts = 0;
        if (!size_text.empty()) {
            const auto qty = cpm::parse_qty(size_text);
            if (!qty) {
                fail("269=" + entry_type + " entry with MDEntrySize(271)=" + size_text);
                return;
            }
            contracts = *qty;
        }
        if (entry_type == "B") {
            out_.volumes.push_back({req_id_, inst.symbol, inst.security_id, contracts, is_snapshot, time});
        } else {
            out_.open_interest.push_back({req_id_, inst.symbol, inst.security_id, contracts, is_snapshot, time});
        }
    }

    void lifecycle() {
        const auto time = transact_time();
        if (!time) return;
        const auto symbol = field(message_, F::Symbol);
        if (symbol.empty()) {
            fail("f without Symbol(55)");
            return;
        }
        const auto code_text = field(message_, F::SecurityTradingStatus);
        const auto code = cpm::parse_scaled(code_text, 0);
        if (!code) {
            fail("f with SecurityTradingStatus(326)=" + shown(code_text));
            return;
        }
        InstrumentLifecycle event;
        // The echo of the request id is SecurityStatusReqID(324) on f, not 262.
        event.md_req_id = field(message_, F::SecurityStatusReqID);
        event.symbol = symbol;
        event.security_id = field(message_, F::SecurityID);
        event.status_code = static_cast<int>(*code);
        event.status = status_of(*code);
        event.text = field(message_, F::Text);
        event.transact_time = *time;
        if (event.status == LifecycleStatus::Unknown) {
            fail("f with SecurityTradingStatus(326)=" + code_text + " which the spec does not define");
        }
        out_.lifecycle.push_back(std::move(event));
    }

    std::optional<Price> price_of(const FIX::FieldMap& entry, const char* what) {
        const auto text = field(entry, F::MDEntryPx);
        const auto price = cpm::parse_price(text);
        if (!price) fail(std::string(what) + " entry with MDEntryPx(270)=" + shown(text));
        return price;
    }

    std::optional<Qty> qty_of(const FIX::FieldMap& entry, const char* what) {
        const auto text = field(entry, F::MDEntrySize);
        const auto qty = cpm::parse_qty(text);
        if (!qty) fail(std::string(what) + " entry with MDEntrySize(271)=" + shown(text));
        return qty;
    }

    // One X can carry entries for several instruments (all-instrument
    // subscriptions), so changes are grouped by instrument, first seen first.
    BookUpdate& update_for(const Instrument& inst, Clock::time_point time) {
        const auto it = std::find_if(out_.book_updates.begin(), out_.book_updates.end(),
                                     [&](const BookUpdate& u) { return u.symbol == inst.symbol; });
        if (it != out_.book_updates.end()) return *it;
        BookUpdate update;
        update.md_req_id = req_id_;
        update.symbol = inst.symbol;
        update.security_id = inst.security_id;
        update.transact_time = time;
        out_.book_updates.push_back(std::move(update));
        return out_.book_updates.back();
    }

    const FIX::Message& message_;
    std::string_view api_key_;
    std::string type_;
    std::string req_id_;
    DecodedBatch out_;
};

}  // namespace

bool DecodedBatch::empty() const {
    return book_snapshots.empty() && book_updates.empty() && trades.empty() && volumes.empty() &&
           open_interest.empty() && lifecycle.empty() && rejects.empty() && errors.empty();
}

DecodedBatch decode_message(const FIX::Message& message, std::string_view api_key) {
    return Decoder(message, api_key).run();
}

}  // namespace cpm::md::fix
