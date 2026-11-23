#include "cpm/strategy/host.hpp"

#include "cpm/md/ticker.hpp"

#include <algorithm>
#include <utility>

namespace cpm::strategy {
namespace {

// What crypto_mm checked inline: an instrument is quotable while it is Open,
// or while nothing has said otherwise yet (Q13 - the status is Unknown until
// the first SecurityStatus arrives).
bool default_tradable(const InstrumentView& view) {
    return view.status == cpm::md::LifecycleStatus::Open ||
           view.status == cpm::md::LifecycleStatus::Unknown;
}

// The instrument's own name says when it stops trading, and that is the only
// thing about the close that needs nothing to arrive: the lifecycle feed is
// changes-only and the book goes on showing a market that has ended. On
// 2026-11-04 a strategy quoted for 475 ms past the bell on a book that still
// read 0.51/0.52, and the third of those orders came back
// INSTRUMENT_NOT_ACTIVE, which halted the daemon.
//
// A name with no scheduled end - a sports market, a simulator instrument -
// has nothing to check and is left to the lifecycle status alone.
bool window_has_ended(const std::string& symbol, Clock::time_point now) {
    const auto parsed = cpm::md::parse_ticker(symbol);
    return parsed && now >= parsed->window_end;
}

std::string id_prefix(const std::string& kind) {
    std::string prefix;
    for (const char c : kind) {
        if (c >= 'A' && c <= 'Z') {
            prefix += static_cast<char>(c - 'A' + 'a');
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            prefix += c;
        }
    }
    return prefix.empty() ? "strategy" : prefix;
}

}  // namespace

const char* to_string(RunState state) {
    switch (state) {
        case RunState::Stopped: return "stopped";
        case RunState::Running: return "running";
        case RunState::Stopping: return "stopping";
        case RunState::Halted: return "halted";
    }
    return "unknown";
}

StrategyHost::StrategyHost(Options options, const StrategyRegistry& registry, StrategyHooks venue,
                           Feeds feeds, Logger& logger)
    : options_(options),
      registry_(registry),
      venue_(std::move(venue)),
      feeds_(std::move(feeds)),
      logger_(logger) {}

StrategyHost::~StrategyHost() = default;

StrategyHost::Entry* StrategyHost::find(const std::string& id) {
    const auto it = std::find_if(entries_.begin(), entries_.end(),
                                 [&](const std::unique_ptr<Entry>& e) { return e->id == id; });
    return it == entries_.end() ? nullptr : it->get();
}

const StrategyHost::Entry* StrategyHost::find(const std::string& id) const {
    const auto it = std::find_if(entries_.begin(), entries_.end(),
                                 [&](const std::unique_ptr<Entry>& e) { return e->id == id; });
    return it == entries_.end() ? nullptr : it->get();
}

StrategyHooks StrategyHost::hooks_for(Entry& entry) const {
    StrategyHooks hooks = venue_;
    Entry* target = &entry;
    const auto owner_gate = venue_.can_submit;
    // Three independent answers, all of which must be yes: the owner's gate
    // (the daemon's --allow-orders and the reconciler), this strategy's arming,
    // and the absence of a halt. The strategy never sees which one said no.
    hooks.can_submit = [this, target, owner_gate] {
        if (halted_.load()) return false;
        if (!target->armed.load()) return false;
        return !owner_gate || owner_gate();
    };
    // The id goes to the owner on the way past, so the record files can say
    // which strategy an order came from. Only the ids we mint are named: a
    // cancel's own ClOrdID here, and the order's when it was sent.
    // A strategy that has decided it is done - at its inventory limit, say.
    // Recorded here and acted on by tick(), because this is called from
    // inside the pass and the host's lock is already held.
    hooks.stop_me = [target](const std::string& why) {
        if (target->stop_requested.empty()) {
            target->stop_requested = why.empty() ? "the strategy stopped itself" : why;
        }
    };
    // Which instrument a strategy's commentary was about. The tag is put on
    // here and not in the strategies, so no kind can forget it and none has
    // to be told its own id; it is read at log time rather than frozen,
    // because a roll gives this entry a new window and the tag has to follow
    // it. A line is only ever logged from inside the pass, under the host's
    // lock, which is also the only place the symbols are written.
    //
    // One symbol is the shape there is: one kind, and a symbol is claimed by
    // one strategy (GUI-Q6), so [id symbol] identifies the line completely.
    // A kind that quotes two legs would make the symbol a guess - the hook
    // carries a line and not what it was about - so it gets [id] alone and
    // says the leg itself.
    if (const auto log = venue_.log) {
        hooks.log = [log, target](const std::string& line) {
            log(log_tag(target->id, target->symbols) + line);
        };
    }
    if (venue_.attribute) {
        const std::string id = entry.id;
        const auto attribute = venue_.attribute;
        if (const auto send = venue_.send) {
            hooks.send = [send, attribute, id](const NewOrder& order) {
                attribute(order.cl_ord_id, id);
                return send(order);
            };
        }
        if (const auto cancel = venue_.cancel) {
            hooks.cancel = [cancel, attribute, id](const CancelOrder& request) {
                attribute(request.cl_ord_id, id);
                return cancel(request);
            };
        }
    }
    return hooks;
}

std::string StrategyHost::create(const StrategySpec& spec, std::string& id_out,
                                 const std::string& target) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (halted_.load()) return "halted: " + halt_reason_;

    auto entry = std::make_unique<Entry>();
    entry->id = id_prefix(spec.kind) + "-" + std::to_string(++next_id_);
    entry->kind = spec.kind;
    entry->target = target;

    auto built = registry_.create(spec, hooks_for(*entry));
    if (!built.ok()) {
        --next_id_;  // the id was never handed out
        return built.error;
    }
    entry->params = std::move(built.values);
    entry->strategy = std::move(built.strategy);
    entry->symbols = entry->strategy->symbols();
    if (entry->symbols.empty()) {
        --next_id_;
        return "a strategy must name at least one symbol";
    }

    // GUI-Q6: two strategies on one symbol share a position, so neither one's
    // inventory limit would mean anything. Refused here rather than explained
    // later.
    //
    // A symbol is claimed by any strategy that exists, whatever its run state.
    // Checking only the running ones would not be a check at all, since a
    // strategy is Stopped the moment it is created; it would also let destroy()
    // unsubscribe a symbol out from under the other claimant. Freeing a symbol
    // therefore means removing the strategy, which can only be done once it has
    // stopped.
    if (options_.one_strategy_per_symbol) {
        for (const auto& other : entries_) {
            // A series claims every window it will ever roll onto, so two
            // strategies on BTCD15M collide even before either has resolved
            // (R-Q6). Two on the same instrument collide as they always did.
            if (!entry->target.empty() && other->target == entry->target) {
                --next_id_;
                return other->id + " already follows " + entry->target +
                       "; two strategies on one series would share its position";
            }
            for (const auto& symbol : entry->symbols) {
                if (std::find(other->symbols.begin(), other->symbols.end(), symbol) !=
                    other->symbols.end()) {
                    --next_id_;
                    return other->id + " already has " + symbol +
                           "; two strategies on one symbol would share its position";
                }
            }
        }
    }

    id_out = entry->id;
    logger_.info("created " + entry->id + " (" + entry->kind + ") on " + entry->symbols.front() +
                 (entry->symbols.size() > 1
                      ? " and " + std::to_string(entry->symbols.size() - 1) + " more"
                      : ""));
    entries_.push_back(std::move(entry));
    return {};
}

std::string StrategyHost::start(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (halted_.load()) return "halted: " + halt_reason_;
    Entry* entry = find(id);
    if (!entry) return "no such strategy: " + id;
    if (entry->state == RunState::Running) return {};
    if (entry->state == RunState::Stopping) return id + " is still stopping";
    // A strategy with no book to read would quote nothing and not say why,
    // which is the diagnosis crypto_mm had to print on its way out. The feed
    // comes first, the way it does when a row is created.
    if (!entry->feed) return id + " has no feed: start its feed first";
    if (const auto reason = stale_book_reason(*entry, Clock::now()); !reason.empty()) return reason;

    entry->state = RunState::Running;
    entry->stopped_for_stale_book = false;
    entry->note.clear();
    entry->withdrawn_for_lifecycle = false;
    entry->running_since = Clock::now();
    entry->last_book_warning = entry->running_since;
    logger_.info("started " + id + (entry->armed.load() ? " (armed)" : " (disarmed: it will compute quotes and send nothing)"));
    log_params(*entry);
    return {};
}

void StrategyHost::log_params(const Entry& entry) const {
    // Every parameter this run is using, in the strategy's own log, at the
    // moment it starts. On start and not on create, because a stopped row's
    // parameters are editable and Start is what applies them: the values that
    // matter are the ones it is about to trade on. It is written again on
    // every start, so a row stopped, retuned and restarted leaves both sets
    // in the file in the order they were used.
    //
    // strategy.log and not the narrative, because this is the key to reading
    // the rest of that file - what the model's working was computed from -
    // and because an operator asking "what was BADQ set to at 06:13" should
    // find the answer in the same file as the lines it explains, without
    // having to trust a screenshot of the page.
    //
    // Schema order and the operator's label, so the line reads like the
    // columns on the page rather than like the map's alphabetical order.
    if (!venue_.log) return;
    const auto* kind = registry_.find(entry.kind);
    if (kind == nullptr) return;
    std::string line = entry.kind + " params:";
    for (const auto& spec : kind->schema) {
        const auto value = entry.params.find(spec.name);
        if (value == entry.params.end()) continue;
        line += " " + (spec.label.empty() ? spec.name : spec.label) + "=" +
                (value->second.empty() ? "(unset)" : value->second);
    }
    venue_.log(log_tag(entry.id, entry.symbols) + line);
}

void StrategyHost::begin_stop(Entry& entry, const std::string& why) {
    // Stopping disarms. Otherwise a strategy restarted later would begin
    // trading immediately on the strength of an arming the operator did
    // before whatever made them stop it, with no second confirmation.
    entry.armed.store(false);
    entry.strategy->withdraw(why);
    entry.note = why;
    // Already flat at the venue: nothing to wait for.
    entry.state = entry.strategy->quiescent() ? RunState::Stopped : RunState::Stopping;
    logger_.info("stopping " + entry.id + ": " + why +
                 (entry.state == RunState::Stopped ? " (nothing was resting)"
                                                   : " (waiting for its cancels)"));
}

std::string StrategyHost::stop(const std::string& id, const std::string& why) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* entry = find(id);
    if (!entry) return "no such strategy: " + id;
    if (entry->state == RunState::Stopped || entry->state == RunState::Halted) return {};
    begin_stop(*entry, why.empty() ? "stopped by the operator" : why);
    return {};
}

std::string StrategyHost::set_feed(const std::string& id, bool up) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* entry = find(id);
    if (!entry) return "no such strategy: " + id;
    if (entry->feed == up) return {};
    entry->feed = up;
    // The operator's narrative has every other thing that happens to a row
    // in it - created, started, armed, stopped, removed - and a row that
    // went quiet because its book was dropped belongs there too.
    logger_.info((up ? "feed started for " : "feed stopped for ") + id +
                 (up ? "" : " (its row stays: Start Feed watches it again)"));
    return {};
}

std::string StrategyHost::arm(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (halted_.load()) return "halted: " + halt_reason_;
    Entry* entry = find(id);
    if (!entry) return "no such strategy: " + id;
    if (entry->state == RunState::Stopping) return id + " is still stopping";
    if (const auto reason = stale_book_reason(*entry, Clock::now()); !reason.empty()) return reason;
    // The daemon-wide key is the other one: without --allow-orders nothing can
    // be armed at all, and saying so here beats arming something that then
    // silently sends nothing. Deliberately not can_submit(), which is also
    // false while the reconciler is working - that is a reason to wait, not a
    // reason to refuse, and the gate already holds the orders back.
    if (venue_.orders_permitted && !venue_.orders_permitted()) {
        return "the daemon is not allowing orders (start it with --allow-orders)";
    }
    if (!entry->armed.exchange(true)) logger_.warn("ARMED " + id + ": it may now place orders");
    return {};
}

std::string StrategyHost::disarm(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* entry = find(id);
    if (!entry) return "no such strategy: " + id;
    if (entry->armed.exchange(false)) logger_.info("disarmed " + id);
    return {};
}

std::string StrategyHost::destroy(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = std::find_if(entries_.begin(), entries_.end(),
                                 [&](const std::unique_ptr<Entry>& e) { return e->id == id; });
    if (it == entries_.end()) return "no such strategy: " + id;
    if ((*it)->state != RunState::Stopped) {
        return id + " is " + to_string((*it)->state) + "; stop it first";
    }
    logger_.info("removed " + id);
    entries_.erase(it);
    return {};
}

std::string StrategyHost::roll(const std::string& id, const std::string& symbol) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (halted_.load()) return "halted: " + halt_reason_;
    auto* entry = find(id);
    if (entry == nullptr) return "no strategy called " + id;
    if (entry->target.empty()) return id + " follows one instrument, not a series";
    if (symbol.empty()) return "a roll needs a symbol to roll to";
    if (!entry->symbols.empty() && entry->symbols.front() == symbol) return {};  // already there
    if (entry->state == RunState::Stopping) return id + " is still stopping";

    // Nothing is cancelled here. A roll happens because the old window closed,
    // and the venue cancels what rests on an instrument it closes - a cancel
    // of ours now would race that one and come back UNKNOWN_ORDER, which
    // halts (OE-Q6).
    const auto previous = entry->symbols.empty() ? std::string("nothing") : entry->symbols.front();
    entry->strategy->abandon("rolled from " + previous + " to " + symbol);

    carry_counters(*entry);

    auto params = entry->params;
    params["symbol"] = symbol;
    if (const auto* kind = registry_.find(entry->kind)) {
        for (const auto& param : kind->schema) {
            if (param.reset_on_roll) params.erase(param.name);
        }
    }
    auto built = registry_.create(StrategySpec{entry->kind, params}, hooks_for(*entry));
    if (!built.ok()) {
        // The old instance is kept: a strategy with no instance at all would
        // be worse than one on a window that has closed, which simply quotes
        // nothing.
        return "could not move " + id + " to " + symbol + ": " + built.error;
    }

    entry->params = std::move(built.values);
    entry->strategy = std::move(built.strategy);
    entry->symbols = entry->strategy->symbols();
    entry->withdrawn_for_lifecycle = false;
    if (!entry->stopped_for_stale_book) entry->note.clear();
    entry->last_book_warning = Clock::now();
    logger_.info("rolled " + id + " from " + previous + " to " + symbol +
                 (entry->armed.load() ? " (still armed)" : " (disarmed)"));
    return {};
}

void StrategyHost::carry_counters(Entry& entry) {
    const auto before = entry.strategy->status();
    entry.carried.passes += before.passes;
    entry.carried.passes_without_target += before.passes_without_target;
    entry.carried.placed += before.placed;
    entry.carried.cancelled += before.cancelled;
    entry.carried.fills += before.fills;
}

std::string StrategyHost::update(const std::string& id, const ParamValues& params) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (halted_.load()) return "halted: " + halt_reason_;
    auto* entry = find(id);
    if (entry == nullptr) return "no strategy called " + id;
    // The one rule. Arming is the operator's confirmed intent to quote with
    // the parameters they were shown, so changing them is something they do
    // before arming or after disarming, never underneath (GUI-Q30). A feed
    // that quotes nothing - created, or Running and disarmed - is exactly
    // where the tuning belongs.
    if (entry->armed.load()) {
        return id + " is armed and may be quoting: disarm it before changing its parameters";
    }
    // Still cancelling what it had. Rebuilding now would hand the new
    // instance a strategy that is already waiting for reports it has never
    // heard of, so this one waits rather than guesses.
    if (entry->state == RunState::Stopping) {
        return id + " is still stopping; its cancels have not come back yet";
    }

    // The current set is the base, so an edit may send one value or all of
    // them and mean the same thing by it. Unknown and out-of-range names are
    // the schema's business, below.
    ParamValues next = entry->params;
    for (const auto& [name, value] : params) {
        if (name == "symbol") {
            // The form shows the series for a strategy that follows one and
            // the window for one pinned to an instrument, and either is the
            // symbol it already has. Anything else is a move, not an edit.
            const auto current = entry->params.find("symbol");
            const bool same = (current != entry->params.end() && value == current->second) ||
                              value == entry->target;
            if (!same) {
                return "a strategy cannot be moved to " + value +
                       " by changing its parameters: stop " + id +
                       ", remove it, and start a feed on the new symbol";
            }
            continue;
        }
        next[name] = value;
    }

    auto built = registry_.create(StrategySpec{entry->kind, next}, hooks_for(*entry));
    if (!built.ok()) return built.error;  // nothing has been touched yet
    // A kind whose symbols are not the symbol parameter alone could still
    // move itself here, and the claim check and the subscriptions were made
    // against the old set. Refused rather than trusted.
    if (built.strategy->symbols() != entry->symbols) {
        return "changing those parameters would move " + id + " to another instrument";
    }

    // Only now, with a built replacement in hand, is anything disturbed.
    // Disarmed does not mean empty-handed: a disarm leaves whatever was
    // resting to be taken off deliberately, and the instance that placed it
    // is about to go. So cancel first - and unlike a roll, the market is
    // open and the venue is not going to do it for us.
    if (!entry->strategy->quiescent()) {
        entry->strategy->withdraw("parameters changed");
    }
    carry_counters(*entry);

    entry->params = std::move(built.values);
    entry->strategy = std::move(built.strategy);
    entry->withdrawn_for_lifecycle = false;
    if (!entry->stopped_for_stale_book) entry->note.clear();
    entry->last_book_warning = Clock::now();
    logger_.info("updated " + id + "'s parameters while " + to_string(entry->state));
    return {};
}

void StrategyHost::stop_all(const std::string& why) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : entries_) {
        entry->armed.store(false);
        if (entry->state == RunState::Stopped || entry->state == RunState::Halted) continue;
        begin_stop(*entry, why);
    }
}

void StrategyHost::halt(const std::string& why) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (halted_.exchange(true)) return;  // the first reason is the real one
    halt_reason_ = why;
    logger_.error("HALTED: " + why + " - every strategy is withdrawing and nothing can restart");
    for (auto& entry : entries_) {
        entry->armed.store(false);
        if (entry->state != RunState::Stopped) entry->strategy->withdraw("halted: " + why);
        entry->note = "halted: " + why;
        entry->state = RunState::Halted;
    }
}

bool StrategyHost::halted() const { return halted_.load(); }

std::string StrategyHost::halt_reason() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return halt_reason_;
}

void StrategyHost::on_report(const ExecutionReport& report) {
    std::lock_guard<std::mutex> lock(reports_mutex_);
    reports_.push_back(report);
}

void StrategyHost::drain_reports() {
    std::deque<ExecutionReport> batch;
    {
        std::lock_guard<std::mutex> lock(reports_mutex_);
        batch.swap(reports_);
    }
    if (batch.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    // Every strategy sees every report and ignores the ones whose ClOrdID is
    // not its own. Filtering by symbol here would be an optimisation with a
    // failure mode: a report for a deleted instrument carries
    // "deleted_instrument" as its symbol, not the one the order was placed on.
    for (const auto& report : batch) {
        for (auto& entry : entries_) entry->strategy->on_report(report);
    }
}

void StrategyHost::drive(Entry& entry, Clock::time_point now) {
    // The reference first, and for every symbol whatever its book is doing.
    // drive() below returns early when a book is missing or closed, and a
    // guard that lived there would be silent exactly when our market has
    // gone quiet with our orders still in it (B-Q5).
    if (feeds_.reference) {
        for (const auto& symbol : entry.symbols) {
            entry.strategy->on_reference(feeds_.reference(symbol), now);
        }
    }
    for (const auto& symbol : entry.symbols) {
        auto view = feeds_.book ? feeds_.book(symbol) : std::nullopt;
        if (!view) {
            // A symbol with no book at all is almost always the wrong symbol,
            // or a window that has already closed (O2/O6). Said on a cadence,
            // because it is a standing condition rather than an event.
            if (now - entry.last_book_warning >= options_.book_warn_after) {
                entry.last_book_warning = now;
                entry.note = "no book for " + symbol + " yet: is the symbol live? " +
                             "md_client --list-instruments names the live ones";
                logger_.warn(entry.id + ": " + entry.note);
            }
            continue;
        }
        const bool ended = window_has_ended(symbol, now);
        const bool tradable =
            !ended && (feeds_.tradable ? feeds_.tradable(*view) : default_tradable(*view));
        if (!tradable) {
            // These are 15-minute windows (O2/O6) and they close without
            // warning, so this is expected rather than an error.
            if (!entry.withdrawn_for_lifecycle) {
                entry.withdrawn_for_lifecycle = true;
                entry.note = ended ? symbol + "'s window has ended"
                                   : std::string("instrument is ") + to_string(view->status);
                // Abandoned, not withdrawn: the venue cancels what is resting
                // on an instrument it closes, and a cancel of ours racing
                // that one is an OrderCancelReject, which halts.
                entry.strategy->abandon(entry.note);
            }
            continue;
        }
        entry.withdrawn_for_lifecycle = false;
        const auto position = feeds_.position ? feeds_.position(symbol) : Position{};
        entry.strategy->on_market(*view, position, now);
    }
}

std::string StrategyHost::stale_book_reason(const Entry& entry, Clock::time_point now) const {
    if (!feeds_.stale_book_reason || !entry.strategy->requires_book_updates()) return {};
    if (!entry.feed) {
        return entry.stopped_for_stale_book ? entry.id + " has no feed: start its feed first" : "";
    }
    for (const auto& symbol : entry.symbols) {
        if (window_has_ended(symbol, now)) {
            if (entry.stopped_for_stale_book) return symbol + "'s window has ended";
            continue;  // Let drive() handle the venue's normal window close.
        }
        auto reason = feeds_.stale_book_reason(symbol, now);
        if (!reason.empty()) return reason;
        if (entry.stopped_for_stale_book) {
            const auto view = feeds_.book ? feeds_.book(symbol) : std::nullopt;
            if (!view || !view->book_valid) return "waiting for a fresh book on " + symbol;
            if (!(feeds_.tradable ? feeds_.tradable(*view) : default_tradable(*view))) {
                return symbol + " is not open for trading";
            }
        }
    }
    return {};
}

void StrategyHost::tick(Clock::time_point now) {
    drain_reports();

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : entries_) {
        switch (entry->state) {
            case RunState::Running:
                if (const auto reason = stale_book_reason(*entry, now); !reason.empty()) {
                    entry->stopped_for_stale_book = true;
                    begin_stop(*entry, reason);
                    break;
                }
                drive(*entry, now);
                // It asked to stop during that pass. Acted on here rather
                // than inside it: the quotes come off at once, and nothing
                // of it is driven again.
                if (!entry->stop_requested.empty()) {
                    const auto why = entry->stop_requested;
                    entry->stop_requested.clear();
                    // Not an operator action, so it is a warning: a strategy
                    // that took itself out of the market is the one line
                    // worth seeing on the page.
                    logger_.warn(entry->id + " stopped itself: " + why);
                    begin_stop(*entry, why);
                }
                break;
            case RunState::Stopping:
                // Its cancels are confirmed once nothing of its own is left.
                if (entry->strategy->quiescent()) {
                    entry->state = RunState::Stopped;
                    logger_.info("stopped " + entry->id);
                }
                break;
            case RunState::Stopped:
            case RunState::Halted:
                break;
        }
    }
}

bool StrategyHost::quiescent() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::all_of(entries_.begin(), entries_.end(),
                       [](const std::unique_ptr<Entry>& e) { return e->strategy->quiescent(); });
}

StrategyInfo StrategyHost::describe(const Entry& entry) const {
    StrategyInfo info;
    info.id = entry.id;
    info.kind = entry.kind;
    info.target = entry.target;
    info.params = entry.params;
    info.symbols = entry.symbols;
    info.state = entry.state;
    info.armed = entry.armed.load();
    info.feed = entry.feed;
    info.note = entry.note;
    info.status = entry.strategy->status();
    info.requires_book_updates = entry.strategy->requires_book_updates();
    // What this window has done, plus what the ones before it did.
    info.status.passes += entry.carried.passes;
    info.status.passes_without_target += entry.carried.passes_without_target;
    info.status.placed += entry.carried.placed;
    info.status.cancelled += entry.carried.cancelled;
    info.status.fills += entry.carried.fills;
    return info;
}

std::vector<StrategyInfo> StrategyHost::list() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<StrategyInfo> out;
    out.reserve(entries_.size());
    for (const auto& entry : entries_) out.push_back(describe(*entry));
    return out;
}

std::optional<StrategyInfo> StrategyHost::info(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const Entry* entry = find(id);
    if (!entry) return std::nullopt;
    return describe(*entry);
}

}  // namespace cpm::strategy
