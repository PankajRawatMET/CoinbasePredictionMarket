#include "fix/sim_application.hpp"

#include "cpm/common/signing.hpp"
#include "cpm/fix_common/logon_auth.hpp"

#include <quickfix/FixFieldNumbers.h>
#include <quickfix/FixValues.h>
#include <quickfix/Group.h>
#include <quickfix/Session.h>

#include <array>
#include <cstdio>
#include <ctime>

namespace cpm::md::sim::fix {
namespace {

namespace F = FIX::FIELD;

// --- time -----------------------------------------------------------------

std::tm utc(Clock::time_point t) {
    const std::time_t secs = Clock::to_time_t(t);
    std::tm tm{};
    gmtime_r(&secs, &tm);
    return tm;
}

// UTCTimestamp with microseconds, as the sandbox sends it [OBS].
std::string utc_timestamp(Clock::time_point t) {
    const std::tm tm = utc(t);
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(t.time_since_epoch()).count() % 1000000;
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d-%02d:%02d:%02d.%06lld", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<long long>(us));
    return buf;
}

// Parses SendingTime "yyyyMMdd-HH:mm:ss[.fff...]" (UTC).
std::optional<Clock::time_point> parse_sending_time(const std::string& text) {
    std::tm tm{};
    int ms = 0;
    if (std::sscanf(text.c_str(), "%4d%2d%2d-%2d:%2d:%2d.%3d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour,
                    &tm.tm_min, &tm.tm_sec, &ms) < 6) {
        return std::nullopt;
    }
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    return Clock::from_time_t(timegm(&tm)) + std::chrono::milliseconds(ms);
}

// --- encoding (layouts observed on the sandbox, docs/open-questions.txt O3-O7) ---

FIX::Message new_message(const char* type, const FIX::message_order& body) {
    FIX::Message m(FIX::message_order(FIX::message_order::header), FIX::message_order(FIX::message_order::trailer),
                   body);
    m.getHeader().setField(F::MsgType, type);
    return m;
}

const char* entry_type(Side s) { return s == Side::Yes ? "0" : "1"; }

FIX::Group md_entry(int delimiter) {
    // Order of the observed X entries: 279|269|55|48|1023|270|271|278|2446.
    return FIX::Group(F::NoMDEntries, delimiter,
                      FIX::message_order(F::MDUpdateAction, F::MDEntryType, F::Symbol, F::SecurityID,
                                         F::MDPriceLevel, F::MDEntryPx, F::MDEntrySize, F::MDEntryID,
                                         F::AggressorSide, 0));
}

// W: 262|55|48|268[269|270|271|1023]|60 (spec §5.1 entries; 60 [OBS O3]).
FIX::Message snapshot(const std::string& req_id, const Instrument& inst) {
    FIX::Message w = new_message(FIX::MsgType_MarketDataSnapshotFullRefresh,
                                 FIX::message_order(F::MDReqID, F::Symbol, F::SecurityID, F::NoMDEntries,
                                                    F::TransactTime, 0));
    w.setField(F::MDReqID, req_id);
    w.setField(F::Symbol, inst.symbol);
    w.setField(F::SecurityID, inst.security_id);
    return w;
}

FIX::Message book_snapshot(const Subscription& sub, const Instrument& inst) {
    FIX::Message w = snapshot(sub.req_id, inst);
    for (Side s : {Side::Yes, Side::No}) {
        if (sub.entry_types.count(entry_type(s)[0]) == 0) continue;
        const auto& levels = inst.side(s);
        const auto n = std::min<std::size_t>(levels.size(), static_cast<std::size_t>(sub.depth));
        for (std::size_t i = 0; i < n; ++i) {
            FIX::Group e(F::NoMDEntries, F::MDEntryType,
                         FIX::message_order(F::MDEntryType, F::MDEntryPx, F::MDEntrySize, F::MDPriceLevel, 0));
            e.setField(F::MDEntryType, entry_type(s));
            e.setField(F::MDEntryPx, format_price(levels[i].price));
            e.setField(F::MDEntrySize, format_qty(levels[i].qty));
            e.setField(F::MDPriceLevel, std::to_string(i + 1));
            w.addGroup(e);
        }
    }
    if (w.groupCount(F::NoMDEntries) == 0) w.setField(F::NoMDEntries, "0");  // empty book [OBS O3]
    w.setField(F::TransactTime, utc_timestamp(inst.last_book_change));
    return w;
}

// Volume/OI snapshot: 269=B/C with 271 only (spec §5.1 example).
FIX::Message stats_snapshot(const Subscription& sub, const Instrument& inst) {
    FIX::Message w = snapshot(sub.req_id, inst);
    for (const char type : {'B', 'C'}) {
        if (sub.entry_types.count(type) == 0) continue;
        FIX::Group e(F::NoMDEntries, F::MDEntryType, FIX::message_order(F::MDEntryType, F::MDEntrySize, 0));
        e.setField(F::MDEntryType, std::string(1, type));
        e.setField(F::MDEntrySize, format_qty(type == 'B' ? inst.volume : inst.open_interest));
        w.addGroup(e);
    }
    w.setField(F::TransactTime, utc_timestamp(inst.last_book_change));
    return w;
}

// X: 262|268[entries]|60, with 55/48 inside each entry [OBS O3/O6].
FIX::Message incremental(const std::string& req_id) {
    FIX::Message x = new_message(FIX::MsgType_MarketDataIncrementalRefresh,
                                 FIX::message_order(F::MDReqID, F::NoMDEntries, F::TransactTime, 0));
    x.setField(F::MDReqID, req_id);
    return x;
}

void add_book_entry(FIX::Message& x, const Step& step, Side side, BookDelta::Action action, Price price, Qty qty,
                    int level) {
    FIX::Group e = md_entry(F::MDUpdateAction);
    e.setField(F::MDUpdateAction, std::to_string(static_cast<int>(action)));
    e.setField(F::MDEntryType, entry_type(side));
    e.setField(F::Symbol, step.symbol);
    e.setField(F::SecurityID, step.security_id);
    if (action != BookDelta::Action::Delete) e.setField(F::MDPriceLevel, std::to_string(level));  // [OBS O3]
    e.setField(F::MDEntryPx, format_price(price));
    e.setField(F::MDEntrySize, format_qty(qty));
    x.addGroup(e);
}

// Book X for one subscription. L2 sends the level deltas as they are. L1
// (264=1) keeps one level per side: a size change at the same best price is a
// Change; a new best price is a Delete of the old best plus a New (design Q8,
// not observable on the sandbox).
std::optional<FIX::Message> book_update(const Subscription& sub, const Step& step) {
    FIX::Message x = incremental(sub.req_id);
    for (Side s : {Side::Yes, Side::No}) {
        if (sub.entry_types.count(entry_type(s)[0]) == 0) continue;
        if (sub.depth == 1) {
            const auto& before = step.best_before[s == Side::Yes ? 0 : 1];
            const auto& after = step.best_after[s == Side::Yes ? 0 : 1];
            if (before == after) continue;
            if (before && after && before->price == after->price) {
                add_book_entry(x, step, s, BookDelta::Action::Change, after->price, after->qty, 1);
                continue;
            }
            if (before) add_book_entry(x, step, s, BookDelta::Action::Delete, before->price, 0, 0);
            if (after) add_book_entry(x, step, s, BookDelta::Action::New, after->price, after->qty, 1);
        } else {
            for (const auto& d : step.book) {
                if (d.side == s) add_book_entry(x, step, s, d.action, d.price, d.qty, d.level);
            }
        }
    }
    if (x.groupCount(F::NoMDEntries) == 0) return std::nullopt;
    x.setField(F::TransactTime, utc_timestamp(step.time));
    return x;
}

// Trade X: 279=0|269=2|55|48|270|271|278|2446 [OBS O4].
FIX::Message trade_update(const Subscription& sub, const Step& step) {
    FIX::Message x = incremental(sub.req_id);
    FIX::Group e = md_entry(F::MDUpdateAction);
    e.setField(F::MDUpdateAction, "0");
    e.setField(F::MDEntryType, "2");
    e.setField(F::Symbol, step.symbol);
    e.setField(F::SecurityID, step.security_id);
    e.setField(F::MDEntryPx, format_price(step.trade->yes_price));
    e.setField(F::MDEntrySize, format_qty(step.trade->qty));
    e.setField(F::MDEntryID, step.trade->entry_id);
    e.setField(F::AggressorSide, std::to_string(step.trade->aggressor));
    x.addGroup(e);
    x.setField(F::TransactTime, utc_timestamp(step.time));
    return x;
}

// Volume/OI X: 279=1|269=B/C|55|48|271 per entry (spec §5.2 example, design Q6).
FIX::Message stats_update(const Subscription& sub, const Step& step) {
    FIX::Message x = incremental(sub.req_id);
    for (const char type : {'B', 'C'}) {
        if (sub.entry_types.count(type) == 0) continue;
        FIX::Group e = md_entry(F::MDUpdateAction);
        e.setField(F::MDUpdateAction, "1");
        e.setField(F::MDEntryType, std::string(1, type));
        e.setField(F::Symbol, step.symbol);
        e.setField(F::SecurityID, step.security_id);
        e.setField(F::MDEntrySize, format_qty(type == 'B' ? step.volume : step.open_interest));
        x.addGroup(e);
    }
    x.setField(F::TransactTime, utc_timestamp(step.time));
    return x;
}

// f: 55|48|326|60, no 324 and no 58 [OBS O3/O6].
FIX::Message status_message(const StatusEvent& event) {
    FIX::Message f = new_message(FIX::MsgType_SecurityStatus,
                                 FIX::message_order(F::Symbol, F::SecurityID, F::SecurityTradingStatus,
                                                    F::TransactTime, 0));
    f.setField(F::Symbol, event.symbol);
    f.setField(F::SecurityID, event.security_id);
    f.setField(F::SecurityTradingStatus, std::to_string(static_cast<int>(event.status)));
    f.setField(F::TransactTime, utc_timestamp(event.time));
    return f;
}

// Y: 262|281|58 [OBS O6].
FIX::Message reject_message(const std::string& req_id, const Reject& reject) {
    FIX::Message y = new_message(FIX::MsgType_MarketDataRequestReject,
                                 FIX::message_order(F::MDReqID, F::MDReqRejReason, F::Text, 0));
    y.setField(F::MDReqID, req_id);
    y.setField(F::MDReqRejReason, std::string(1, reject.reason));
    y.setField(F::Text, reject.text);
    return y;
}

std::string field_or_empty(const FIX::FieldMap& map, int tag) {
    return map.isSetField(tag) ? map.getField(tag) : std::string();
}

}  // namespace

SimApplication::SimApplication(const SimOptions& options, Logger& logger, Clock::time_point now)
    : options_(options),
      logger_(logger),
      market_({}, options.seed, now),
      next_event_(now),
      event_interval_(std::chrono::duration_cast<Clock::duration>(
          std::chrono::duration<double>(1.0 / std::max(options.events_per_sec, 0.001)))) {
    std::vector<InstrumentSpec> specs = options.instruments;
    if (options_.roll_secs > 0) {
        next_roll_ = now + std::chrono::seconds(options_.roll_secs);
        specs.push_back(next_roll_spec(next_roll_));
        roll_symbol_ = specs.back().symbol;
    }
    market_ = Market(std::move(specs), options.seed, now);
}

// Rolling series named like the sandbox's [OBS O2/O6]:
// SIMROLL-SIM<YY><MON><DD><HHMMSS of window end, UTC>-0 (seconds included,
// because windows can be shorter than a minute).
InstrumentSpec SimApplication::next_roll_spec(Clock::time_point window_end) {
    static constexpr std::array<const char*, 12> kMonths{"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                                         "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
    const std::tm tm = utc(window_end);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "SIMROLL-SIM%02d%s%02d%02d%02d%02d-0", tm.tm_year % 100,
                  kMonths[static_cast<std::size_t>(tm.tm_mon)], tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return InstrumentSpec{buf, std::to_string(next_roll_id_++), 500, 3};
}

// --- session -------------------------------------------------------------

void SimApplication::onLogon(const FIX::SessionID& session_id) {
    {
        std::lock_guard lock(mutex_);
        session_id_ = session_id;
        logged_on_ = true;
        ++logons_;
        client_logout_ = false;
        logon_time_ = Clock::now();
        app_messages_sent_ = 0;
    }
    logged_on_cv_.notify_all();
    logger_.info("sim: client logged on");
}

void SimApplication::onLogout(const FIX::SessionID&) {
    std::size_t dropped = 0;
    {
        std::lock_guard lock(mutex_);
        logged_on_ = false;
        // Subscriptions do not survive the session (design Q22).
        subs_.clear();
        dropped = queue_.size();
        queue_.clear();
    }
    logger_.info("sim: client logged out; subscriptions cleared, " + std::to_string(dropped) +
                 " queued message(s) dropped");
}

void SimApplication::toAdmin(FIX::Message& message, const FIX::SessionID&) {
    message.getHeader().setField(F::SenderSubID, "TEST");  // [OBS] sandbox header
    if (message.getHeader().getField(F::MsgType) == FIX::MsgType_Logout && !message.isSetField(F::Text)) {
        std::lock_guard lock(mutex_);
        if (client_logout_) message.setField(F::Text, "CLIENT_LOGOUT");  // the sandbox's reply [OBS]
    }
}

void SimApplication::toApp(FIX::Message& message, const FIX::SessionID& session_id) {
    message.getHeader().setField(F::SenderSubID, "TEST");
    const int seq = std::stoi(message.getHeader().getField(F::MsgSeqNum));
    if (options_.drop_seq && !drop_done_ && seq >= *options_.drop_seq) {
        drop_done_ = true;
        // Skip this sequence number: the next message carries seq + 1.
        if (auto* session = FIX::Session::lookupSession(session_id)) session->setNextSenderMsgSeqNum(seq + 1);
        logger_.warn("sim fault: dropped outbound MsgSeqNum " + std::to_string(seq) + " (--drop-seq)");
        throw FIX::DoNotSend();
    }
    const int sent = ++app_messages_sent_;
    if (options_.disconnect_after && !disconnect_done_ && sent >= *options_.disconnect_after) {
        disconnect_pending_ = true;
    }
}

void SimApplication::fromAdmin(const FIX::Message& message, const FIX::SessionID&) {
    const auto type = message.getHeader().getField(F::MsgType);
    if (type == FIX::MsgType_Logout) {
        std::lock_guard lock(mutex_);
        client_logout_ = true;
        return;
    }
    if (type != FIX::MsgType_Logon) return;
    // Spec §3: Password(554) and the HMAC in Text(58) over
    // SendingTime(ms) + SenderCompID + TargetCompID + Password.
    const auto& h = message.getHeader();
    const auto& creds = options_.credentials;
    if (field_or_empty(message, F::Password) != creds.passphrase) throw FIX::RejectLogon("invalid passphrase");
    const auto aligned = cpm::fix::align_sending_time(h.getField(F::SendingTime));
    const auto expected = cpm::fix_logon_signature(aligned.signed_, h.getField(F::SenderCompID),
                                                   h.getField(F::TargetCompID), creds.passphrase, creds.secret_b64);
    if (field_or_empty(message, F::Text) != expected) throw FIX::RejectLogon("invalid signature");
    const auto sent = parse_sending_time(h.getField(F::SendingTime));
    const auto skew = sent ? Clock::now() - *sent : Clock::duration::max();
    if (!sent || std::chrono::abs(skew) > std::chrono::seconds(options_.clock_window_secs)) {
        throw FIX::RejectLogon("SendingTime outside the signature window");
    }
}

void SimApplication::fromApp(const FIX::Message& message, const FIX::SessionID&) {
    const auto type = message.getHeader().getField(F::MsgType);
    if (type == FIX::MsgType_MarketDataRequest) {
        handle_request(message);
        return;
    }
    logger_.warn("sim: ignoring unexpected application message 35=" + type);
}

void SimApplication::handle_request(const FIX::Message& message) {
    RequestFields r;
    r.req_id = message.getField(F::MDReqID);
    r.subscription_type = message.getField(F::SubscriptionRequestType);
    if (message.isSetField(F::MarketDepth)) r.depth = message.getField(F::MarketDepth);
    FIX::Group types(F::NoMDEntryTypes, F::MDEntryType);
    for (std::size_t i = 1; i <= message.groupCount(F::NoMDEntryTypes); ++i) {
        message.getGroup(static_cast<unsigned>(i), types);
        r.entry_types.push_back(types.getField(F::MDEntryType));
    }
    if (message.isSetField(F::NoRelatedSym)) r.no_related_sym = std::stoi(message.getField(F::NoRelatedSym));
    FIX::Group syms(F::NoRelatedSym, F::Symbol);
    for (std::size_t i = 1; i <= message.groupCount(F::NoRelatedSym); ++i) {
        message.getGroup(static_cast<unsigned>(i), syms);
        r.symbols.push_back(field_or_empty(syms, F::Symbol));
    }

    std::lock_guard lock(mutex_);
    const auto outcome = validate_request(
        r, [&](const std::string& s) { return market_.find(s) != nullptr; },
        [&](const std::string& id) { return subs_.count(id) != 0; });

    if (const auto* rej = std::get_if<Reject>(&outcome)) {
        queue_.push_back(reject_message(r.req_id, *rej));
        return;
    }
    if (const auto* unsub = std::get_if<Unsubscribe>(&outcome)) {
        subs_.erase(unsub->req_id);  // no reply [OBS O3]
        return;
    }
    const auto& sub = std::get<Subscription>(outcome);
    // Snapshots (spec §4.2): book and volume/OI get a W per instrument;
    // trades and lifecycle get nothing until something happens.
    if (sub.feed == Feed::Book || sub.feed == Feed::VolumeOi) {
        const auto symbols = sub.symbols.empty() ? market_.symbols()
                                                 : std::vector<std::string>(sub.symbols.begin(), sub.symbols.end());
        for (const auto& symbol : symbols) {
            const Instrument* inst = market_.find(symbol);
            if (!inst) continue;
            queue_.push_back(sub.feed == Feed::Book ? book_snapshot(sub, *inst) : stats_snapshot(sub, *inst));
        }
    }
    subs_.emplace(sub.req_id, ActiveSub{sub, false});
}

// --- market events (event thread; mutex_ held) ---------------------------

void SimApplication::publish_step(const Step& step) {
    if (step.empty()) return;
    // Trade first, then the book change it caused, same TransactTime [OBS O4].
    if (step.trade) {
        for (const auto& [id, active] : subs_) {
            if (!active.stale && active.sub.feed == Feed::Trades && active.sub.covers(step.symbol)) {
                queue_.push_back(trade_update(active.sub, step));
            }
        }
    }
    for (const auto& [id, active] : subs_) {
        if (active.stale || active.sub.feed != Feed::Book || !active.sub.covers(step.symbol)) continue;
        if (auto x = book_update(active.sub, step)) queue_.push_back(std::move(*x));
    }
    if (step.stats_changed) {
        for (const auto& [id, active] : subs_) {
            if (!active.stale && active.sub.feed == Feed::VolumeOi && active.sub.covers(step.symbol)) {
                queue_.push_back(stats_update(active.sub, step));
            }
        }
    }
}

// Each lifecycle subscription gets its own copy, as book subscriptions do [OBS O7].
void SimApplication::publish_status(const StatusEvent& event) {
    for (const auto& [id, active] : subs_) {
        if (!active.stale && active.sub.feed == Feed::Lifecycle && active.sub.covers(event.symbol)) {
            queue_.push_back(status_message(event));
        }
    }
}

void SimApplication::roll(Clock::time_point now) {
    const auto window_end = now + std::chrono::seconds(options_.roll_secs);
    auto result = market_.roll(roll_symbol_, next_roll_spec(window_end), now);
    for (const auto& e : result.closing) publish_status(e);
    publish_step(result.cleared);
    for (const auto& e : result.after) publish_status(e);
    pending_settles_.emplace_back(now + options_.settle_delay, roll_symbol_);
    logger_.info("sim: rolled " + roll_symbol_ + " -> " + result.after.back().symbol);
    roll_symbol_ = result.after.back().symbol;
    next_roll_ = window_end;
}

void SimApplication::apply_faults(Clock::time_point now) {
    if (options_.stale_after_secs && !stale_done_ && logged_on_ &&
        now - logon_time_ >= std::chrono::seconds(*options_.stale_after_secs)) {
        stale_done_ = true;
        for (auto& [id, active] : subs_) active.stale = true;
        logger_.warn("sim fault: feed stale, " + std::to_string(subs_.size()) +
                     " subscription(s) silenced (--stale-after)");
    }
}

void SimApplication::tick(Clock::time_point now) {
    {
        std::lock_guard lock(mutex_);
        // Catch up at most one second of events (e.g. after a pause).
        if (now - next_event_ > std::chrono::seconds(1)) next_event_ = now;
        while (next_event_ <= now) {
            publish_step(market_.step(next_event_));
            next_event_ += event_interval_;
        }
        if (options_.roll_secs > 0 && now >= next_roll_) roll(now);
        for (auto it = pending_settles_.begin(); it != pending_settles_.end();) {
            if (it->first > now) {
                ++it;
                continue;
            }
            publish_status(market_.settle(it->second, now));
            it = pending_settles_.erase(it);
        }
        apply_faults(now);
    }
    flush();
}

void SimApplication::flush() {
    std::deque<FIX::Message> batch;
    std::optional<FIX::SessionID> session_id;
    {
        std::lock_guard lock(mutex_);
        if (!logged_on_) {
            queue_.clear();
            return;
        }
        batch.swap(queue_);
        session_id = session_id_;
    }
    for (auto& message : batch) {
        try {
            FIX::Session::sendToTarget(message, *session_id);
        } catch (const FIX::SessionNotFound&) {
            return;
        }
        if (disconnect_pending_ && !disconnect_done_) {
            disconnect_done_ = true;
            logger_.warn("sim fault: disconnecting after " + std::to_string(app_messages_sent_.load()) +
                         " market data message(s) (--disconnect-after)");
            if (auto* session = FIX::Session::lookupSession(*session_id)) session->disconnect();
            return;
        }
    }
}

bool SimApplication::wait_logged_on(std::chrono::milliseconds timeout) const {
    std::unique_lock lock(mutex_);
    return logged_on_cv_.wait_for(lock, timeout, [this] { return logged_on_; });
}

int SimApplication::logons() const {
    std::lock_guard lock(mutex_);
    return logons_;
}

}  // namespace cpm::md::sim::fix
