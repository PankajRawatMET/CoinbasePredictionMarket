#include "cpm/trader/control_api.hpp"

#include "cpm/md/ticker.hpp"

#include <algorithm>
#include <utility>

namespace cpm::trader {

// How much of a window has to be left before arming into it is worth doing.
// Short enough that a normal roll is never refused, long enough that the
// last orders cannot be in flight at the close.
constexpr std::chrono::seconds kMinWindowToArm{10};

const char* to_string(DaemonState state) {
    switch (state) {
        case DaemonState::Starting: return "starting";
        case DaemonState::Live: return "live";
        case DaemonState::Halted: return "halted";
    }
    return "unknown";
}

ControlApi::ControlApi(StrategyHost& host, const StrategyRegistry& registry, Env env,
                       bool allow_orders)
    : host_(host), registry_(registry), env_(std::move(env)), allow_orders_(allow_orders) {}

void ControlApi::bump() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++revision_;
}

std::uint64_t ControlApi::revision() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return revision_;
}

std::vector<KindView> ControlApi::kinds() const {
    std::vector<KindView> out;
    for (const auto& name : registry_.kinds()) {
        const auto* kind = registry_.find(name);
        if (!kind) continue;
        out.push_back(KindView{kind->name, kind->help, kind->schema});
    }
    return out;
}

Result ControlApi::create(const StrategySpec& spec) {
    Result result;
    std::string id;

    // A series rather than an instrument: resolve it to the window trading
    // now. The daemon does that over REST, because nothing on either FIX
    // session can be asked what is open (R-Q2).
    StrategySpec resolved = spec;
    std::string target;
    const auto symbol = spec.params.find("symbol");
    if (symbol != spec.params.end() && !symbol->second.empty() &&
        !cpm::md::looks_like_instrument(symbol->second)) {
        if (!env_.resolve) {
            result.error = symbol->second +
                           " looks like a series, and this daemon cannot resolve one: REST is "
                           "not configured (the CPM_REST_* set in config/creds.env)";
            return result;
        }
        const auto found = env_.resolve(symbol->second);
        if (!found.error.empty()) {
            result.error = found.error;
            return result;
        }
        target = symbol->second;
        resolved.params["symbol"] = found.symbol;
    }

    if (const auto error = host_.create(resolved, id, target); !error.empty()) {
        result.error = error;
        return result;
    }

    // The feed has to be there before the strategy is: a strategy on a symbol
    // whose book never arrives looks identical to a wrong symbol, and that is
    // exactly the diagnosis crypto_mm had to print on its way out.
    const auto info = host_.info(id);
    std::vector<std::string> subscribed;
    for (const auto& symbol : info ? info->symbols : std::vector<std::string>{}) {
        // A name that could never be an instrument is caught here rather than
        // sent. The gateway's answer to one is "unknown symbol", and that is
        // a reject, and a reject halts the daemon - so an operator typing a
        // series name into the form would end the session rather than be told
        // they typed a series name.
        std::string error;
        if (!cpm::md::looks_like_instrument(symbol)) {
            error = symbol +
                    " is not an instrument - it looks like a series. Quoting a series and "
                    "rolling with it is not built yet (docs/design-roll.md); give the window's "
                    "full ticker, which looks like " +
                    symbol + "-<ASSET><YY><MON><DD><HHMM>-0";
        } else if (env_.subscribe) {
            error = env_.subscribe(symbol);
        }
        if (!error.empty()) {
            // Unwind: drop what was opened and the strategy with it, so a
            // failure leaves nothing half-built behind.
            for (const auto& done : subscribed) {
                if (env_.unsubscribe) env_.unsubscribe(done);
            }
            host_.destroy(id);
            result.error = error;
            return result;
        }
        subscribed.push_back(symbol);
    }

    result.id = id;
    bump();
    return result;
}

Result ControlApi::update(const std::string& id, const ParamValues& params) {
    Result result{host_.update(id, params), {}};
    if (result.ok()) {
        result.id = id;
        bump();
    }
    return result;
}

Result ControlApi::start(const std::string& id) {
    if (const auto info = host_.info(id); env_.recover_fills && info && info->feed &&
                                         info->state == cpm::strategy::RunState::Stopped &&
                                         !host_.halted()) {
        for (const auto& symbol : info->symbols) {
            if (const auto error = env_.recover_fills(symbol); !error.empty()) return {error, {}};
        }
    }
    Result result{host_.start(id), {}};
    if (result.ok()) bump();
    return result;
}

Result ControlApi::stop(const std::string& id) {
    Result result{host_.stop(id, "stopped by the operator"), {}};
    if (result.ok()) bump();
    return result;
}

Result ControlApi::arm(const std::string& id) {
    // Arming into the last seconds of a window is a bell, not a trade: the
    // strategy quotes for a moment and then has to come straight back off,
    // and anything still in flight at the close is answered
    // INSTRUMENT_NOT_ACTIVE (2026-11-04). The window's end is in its own
    // name, so this needs nothing to have arrived.
    if (const auto info = host_.info(id)) {
        const auto now = Clock::now();
        for (const auto& symbol : info->symbols) {
            const auto parsed = cpm::md::parse_ticker(symbol);
            if (!parsed) continue;  // no scheduled end to be near
            if (now >= parsed->window_end) {
                return Result{symbol + "'s window has already ended; roll it or point the "
                                       "strategy at the window trading now", {}};
            }
            const auto left =
                std::chrono::duration_cast<std::chrono::seconds>(parsed->window_end - now);
            if (left < kMinWindowToArm) {
                return Result{symbol + " ends in " + std::to_string(left.count()) +
                                  "s, which is not a window worth arming into: it would quote "
                                  "and withdraw, and an order caught by the close is a reject",
                              {}};
            }
        }
    }
    if (const auto info = host_.info(id); env_.recover_fills && info &&
                                         info->state == cpm::strategy::RunState::Running &&
                                         !host_.halted()) {
        for (const auto& symbol : info->symbols) {
            if (const auto error = env_.recover_fills(symbol); !error.empty()) return {error, {}};
        }
    }
    Result result{host_.arm(id), {}};
    if (result.ok()) bump();
    return result;
}

Result ControlApi::disarm(const std::string& id) {
    Result result{host_.disarm(id), {}};
    if (result.ok()) bump();
    return result;
}

void ControlApi::drop_feed(const std::string& id, const std::vector<std::string>& symbols) {
    if (!env_.unsubscribe) return;
    const auto rows = host_.list();
    for (const auto& symbol : symbols) {
        const bool another_reads_it =
            std::any_of(rows.begin(), rows.end(), [&](const StrategyInfo& row) {
                return row.id != id && row.feed &&
                       std::find(row.symbols.begin(), row.symbols.end(), symbol) !=
                           row.symbols.end();
            });
        if (!another_reads_it) env_.unsubscribe(symbol);
    }
}

Result ControlApi::stop_feed(const std::string& id) {
    Result result;
    const auto info = host_.info(id);
    if (!info) {
        result.error = "no such strategy: " + id;
        return result;
    }
    if (!info->feed) {
        result.error = id + " has no feed to stop";
        return result;
    }
    if (info->state != cpm::strategy::RunState::Stopped &&
        info->state != cpm::strategy::RunState::Halted) {
        result.error = id + " is " + to_string(info->state) + "; stop it first";
        return result;
    }
    if (const auto error = host_.set_feed(id, false); !error.empty()) {
        result.error = error;
        return result;
    }
    drop_feed(id, info->symbols);
    result.id = id;
    bump();
    return result;
}

Result ControlApi::start_feed(const std::string& id) {
    Result result;
    const auto info = host_.info(id);
    if (!info) {
        result.error = "no such strategy: " + id;
        return result;
    }
    if (info->feed) {
        result.error = id + " already has its feed";
        return result;
    }

    std::string symbol = info->symbols.empty() ? std::string() : info->symbols.front();
    // A series row stopped for longer than a window is pointed at an
    // instrument that has closed. Resolve the series again and move it,
    // exactly as create does, rather than subscribing to a closed one.
    if (!info->target.empty() && env_.resolve) {
        const auto found = env_.resolve(info->target);
        if (!found.error.empty()) {
            result.error = found.error;
            return result;
        }
        if (found.symbol != symbol) {
            if (const auto error = host_.roll(id, found.symbol); !error.empty()) {
                result.error = error;
                return result;
            }
            symbol = found.symbol;
        }
    }

    const auto moved = host_.info(id);
    const auto symbols = moved ? moved->symbols : std::vector<std::string>{};
    std::vector<std::string> subscribed;
    for (const auto& name : symbols) {
        std::string error;
        if (!cpm::md::looks_like_instrument(name)) {
            error = name + " is not an instrument - it looks like a series";
        } else if (env_.subscribe) {
            error = env_.subscribe(name);
        }
        if (!error.empty()) {
            // Unwind: the row keeps no feed rather than half of one.
            drop_feed(id, subscribed);
            result.error = error;
            return result;
        }
        subscribed.push_back(name);
    }
    if (const auto error = host_.set_feed(id, true); !error.empty()) {
        drop_feed(id, subscribed);
        result.error = error;
        return result;
    }
    result.id = id;
    bump();
    return result;
}

Result ControlApi::destroy(const std::string& id) {
    // Read the symbols before it goes: afterwards there is nothing to ask.
    const auto info = host_.info(id);
    Result result{host_.destroy(id), {}};
    if (!result.ok()) return result;
    // A row whose feed was already stopped has nothing to drop, and the
    // symbol it used to read may belong to another row by now.
    if (info && info->feed) drop_feed(id, info->symbols);
    bump();
    return result;
}

Result ControlApi::stop_all() {
    host_.stop_all("stop all");
    bump();
    return {};
}

ControlApi::ExitPlan ControlApi::plan_exit(const std::string& symbol, Qty qty) const {
    ExitPlan plan;
    const auto refuse = [&plan](std::string why) -> ExitPlan {
        plan.refusal = std::move(why);
        return plan;
    };

    // The gates first, cheapest and most absolute first, because every one
    // of them is a reason the page should not even offer the button.
    if (!allow_orders_) {
        return refuse("this daemon was started without --allow-orders, so nothing of its own can "
                      "reach the venue");
    }
    if (host_.halted()) {
        return refuse("the daemon has halted and nothing may be submitted again: " +
                      host_.halt_reason());
    }
    if (env_.can_submit && !env_.can_submit()) {
        return refuse("nothing may be submitted yet; recovery is " +
                      (env_.recovery ? env_.recovery() : std::string("not live")));
    }

    // What is held. A position the daemon has never seen a fill for is not a
    // position: after a restart past the replay window it has none, and
    // guessing one from a settlement report is exactly the mistake that
    // halted this daemon twice (OE-Q7).
    const PositionView* held = nullptr;
    std::vector<PositionView> positions;
    if (env_.positions) positions = env_.positions();
    for (const auto& position : positions) {
        if (position.symbol == symbol) held = &position;
    }
    if (held == nullptr) return refuse("this daemon holds no position in " + symbol);
    if (held->net == 0) return refuse(symbol + " is flat, so there is nothing to exit");

    const Qty net = held->net;
    const Qty size = net > 0 ? net : -net;
    const Qty wanted = qty == 0 ? size : qty;
    if (wanted < 0) return refuse("a quantity to exit cannot be negative");
    if (wanted > size) {
        return refuse("asked to exit " + format_qty(wanted) + " of " + symbol + ", which is more "
                      "than the " + format_qty(size) + " held");
    }

    // A strategy that is armed on this instrument is quoting it, and would
    // quote straight back over the exit on its next pass. Disarming is the
    // operator's call, not this one's: an exit that silently stopped someone
    // else's strategy would be a worse surprise than a refusal.
    for (const auto& strategy : host_.list()) {
        if (!strategy.armed) continue;
        const bool on_it = std::any_of(strategy.symbols.begin(), strategy.symbols.end(),
                                       [&](const std::string& name) { return name == symbol; });
        if (on_it) {
            return refuse(strategy.id + " is armed on " + symbol + " and would quote over the "
                          "exit; disarm it first");
        }
    }

    // The book, for the price to cross to. No book and there is no price: a
    // position whose feed was stopped can be seen but not exited, which is
    // honest, and Start Feed is the way back.
    if (!env_.book) return refuse("this daemon cannot read a book, so it cannot price an exit");
    const auto book = env_.book(symbol);
    if (!book) {
        return refuse("no book for " + symbol + ": nothing is subscribed to it, so there is no "
                      "price to cross. Start its feed first");
    }
    // The window's own clock first: a name that says the window has ended is
    // the one fact about the close that needs nothing to have arrived, and
    // an order on an ended window is INSTRUMENT_NOT_ACTIVE at the gateway.
    if (const auto parsed = cpm::md::parse_ticker(symbol); parsed) {
        if (Clock::now() >= parsed->window_end) {
            return refuse(symbol + "'s window has ended, so it settles rather than trades: "
                                   "an order on it would be rejected, and a reject halts");
        }
    }

    // Closed, Settled, Paused: the gateway answers an order on one of those
    // with INSTRUMENT_NOT_ACTIVE, and that reject halts the whole run
    // (OE-Q6). A position on a window that has stopped trading settles; it
    // cannot be exited.
    //
    // Open, or nothing said yet, is the same rule the strategies quote by
    // (StrategyHost's default_tradable, [MD] Q13): the status is Unknown
    // until the first SecurityStatus arrives, and refusing that would refuse
    // every exit on a venue that sends none - including both simulators.
    if (book->status != "Open" && book->status != "Unknown") {
        return refuse(symbol + " is " + book->status + ", not Open: an order on it would be "
                      "rejected, and a reject halts the run");
    }
    // An invalid book is one no live subscription has bound: either nothing
    // ever arrived, or the feed was stopped and the book was unbound with it.
    // Either way there is no touch to cross to, and the last one we saw is
    // not a price - it is a memory.
    if (!book->valid) {
        return refuse("the book for " + symbol + " is not live: no subscription is building it, "
                      "so its last touch is a memory and not a price. Start its feed first");
    }

    // Long exits by selling into the best YES bid; short exits by buying the
    // cheapest YES ask, which on this venue is implied from the best NO bid
    // (Q9). The quantity resting there is what one press can actually fill.
    const bool selling = net > 0;
    const auto price = selling ? book->best_yes_bid : book->implied_yes_ask;
    const auto available = selling ? book->best_yes_bid_qty : book->best_no_bid_qty;
    if (!price) {
        return refuse(std::string("nothing is ") + (selling ? "bid for " : "offered on ") + symbol +
                      ", so there is nothing to cross");
    }
    plan.available = available.value_or(0);

    // Our own order resting where the exit would cross is a self-match. The
    // venue's answer to one is to cancel a side (SMP), so the exit could
    // quietly vanish instead of exiting - and a strategy that has been
    // disarmed still has its last quotes in the market until the cancels are
    // confirmed, which is exactly when an operator reaches for this button.
    if (env_.resting_orders) {
        for (const auto& leg : env_.resting_orders(symbol)) {
            const bool opposes =
                selling ? leg.side == cpm::oms::Side::Buy : leg.side == cpm::oms::Side::Sell;
            if (!opposes) continue;
            const bool would_cross = selling ? leg.price >= *price : leg.price <= *price;
            if (would_cross) {
                return refuse("our own order " + leg.cl_ord_id + " rests at " +
                              cpm::format_price(leg.price) + " and the exit would trade with "
                              "itself; stop whatever is quoting " + symbol + " first");
            }
        }
    }

    const auto limits = env_.limits ? env_.limits() : LimitsView{};
    if (limits.max_quantity > 0 && wanted > limits.max_quantity) {
        return refuse("exiting " + format_qty(wanted) + " would break this run's per-order limit "
                      "of " + format_qty(limits.max_quantity) + "; exit it in parts");
    }

    plan.order.symbol = symbol;
    plan.order.side = selling ? cpm::oms::Side::Sell : cpm::oms::Side::Buy;
    plan.order.quantity = wanted;
    plan.order.type = cpm::oms::OrderType::Limit;
    plan.order.price = *price;
    // IOC, and never post-only: this order is meant to cross.
    plan.order.time_in_force = cpm::oms::TimeInForce::Ioc;
    plan.order.post_only = cpm::oms::PostOnly::None;
    return plan;
}

Result ControlApi::exit_position(const std::string& symbol, Qty qty) {
    Result result;
    auto plan = plan_exit(symbol, qty);
    if (!plan.refusal.empty()) {
        result.error = std::move(plan.refusal);
        return result;
    }
    if (!env_.send_order || !env_.next_cl_ord_id) {
        result.error = "this daemon has no order path of its own, so it cannot exit a position";
        return result;
    }
    // The id is stamped here and nowhere else: plan_exit() runs for every
    // snapshot, and an id allocated there would move the counter the
    // bookmark persists twice a second for nothing.
    plan.order.cl_ord_id = env_.next_cl_ord_id();
    if (const auto error = env_.send_order(plan.order); !error.empty()) {
        result.error = error;
        return result;
    }
    result.id = plan.order.cl_ord_id;
    bump();
    return result;
}

AxisView ControlApi::axis(const std::string& symbol) const {
    AxisView view;
    view.symbol = symbol;
    // The instruments this daemon is actually watching: what it watches for
    // its own sake, and whatever the strategies claim on top. A watched
    // instrument has a book on the page from startup, so it must have a
    // ladder too - the grid is what the ladder is drawn on.
    bool watched = false;
    if (env_.watched) {
        for (const auto& name : env_.watched()) {
            if (name == symbol) watched = true;
        }
    }
    for (const auto& strategy : host_.list()) {
        for (const auto& name : strategy.symbols) {
            if (name == symbol) watched = true;
        }
    }
    if (!watched || !env_.axis) return view;
    return env_.axis(symbol);
}

Result ControlApi::roll(const std::string& id, const std::string& symbol) {
    Result result;
    const auto info = host_.info(id);
    if (!info) {
        result.error = "no strategy called " + id;
        return result;
    }
    const auto previous = info->symbols;

    // The new window's book before the strategy that will quote it: a
    // strategy pointed at a symbol with no subscription would quote nothing
    // and not say why.
    if (env_.subscribe) {
        if (const auto error = env_.subscribe(symbol); !error.empty()) {
            result.error = error;
            return result;
        }
    }
    if (const auto error = host_.roll(id, symbol); !error.empty()) {
        if (env_.unsubscribe) env_.unsubscribe(symbol);
        result.error = error;
        return result;
    }
    // Only once the strategy is off it. Unsubscribing on Closed rather than
    // later matters: an instrument that closed a couple of minutes ago is
    // already "unknown symbol", and that reject halts ([MD] O6, R-N2).
    for (const auto& old_symbol : previous) {
        if (old_symbol != symbol && env_.unsubscribe) env_.unsubscribe(old_symbol);
    }
    result.id = id;
    bump();
    return result;
}

Snapshot ControlApi::snapshot() const {
    Snapshot out;
    out.taken = Clock::now();
    out.allow_orders = allow_orders_;
    out.strategies = host_.list();
    if (host_.halted()) {
        out.state = DaemonState::Halted;
        out.halt_reason = host_.halt_reason();
    } else {
        // Live once both sessions are up; until then the page should say so
        // rather than showing an empty table as though all were well.
        const bool ready = (!env_.market_data || env_.market_data().logged_on) &&
                           (!env_.order_entry || env_.order_entry().logged_on);
        out.state = ready ? DaemonState::Live : DaemonState::Starting;
    }
    if (env_.market_data) out.market_data = env_.market_data();
    if (env_.order_entry) out.order_entry = env_.order_entry();
    if (env_.recovery) out.recovery = env_.recovery();
    if (env_.reference) out.reference = env_.reference();
    if (env_.positions) out.positions = env_.positions();
    if (env_.fills) out.fills = env_.fills();
    // Whether each position can be exited, and at what, answered by the same
    // function the order itself goes through (G13). Computed here rather than
    // in the browser so the button is never offered for an order the daemon
    // would refuse - on a closed window, that refusal is the difference
    // between a click and a halt.
    for (auto& position : out.positions) {
        const auto plan = plan_exit(position.symbol, 0);
        position.exit_refusal = plan.refusal;
        position.exit_available = plan.available;
        if (plan.refusal.empty()) position.exit_price = plan.order.price;
        if (env_.book) {
            if (const auto book = env_.book(position.symbol)) position.status = book->status;
        }
    }
    if (env_.pnl) {
        out.pnl = env_.pnl();
        // Realized always sums. Unrealized only sums while every row has one:
        // one unmarked position and the total is not known, which is a
        // different thing from the total being smaller.
        Amount unrealized = 0;
        bool all_marked = true;
        for (const auto& row : out.pnl) {
            out.pnl_total.realized += row.realized;
            if (row.unrealized) {
                unrealized += *row.unrealized;
            } else {
                all_marked = false;
            }
        }
        if (all_marked) {
            out.pnl_total.unrealized = unrealized;
            out.pnl_total.total = out.pnl_total.realized + unrealized;
        }
    }
    if (env_.limits) out.limits = env_.limits();

    // One book per symbol, without duplicates: what the daemon watches
    // first, in the order it was asked for, then whatever the strategies
    // claim on top. A watched instrument is on the page before any strategy
    // exists, which is the point of watching it.
    if (env_.book) {
        const auto add = [&](const std::string& symbol) {
            const bool seen = std::any_of(out.books.begin(), out.books.end(),
                                          [&](const BookView& view) { return view.symbol == symbol; });
            if (seen) return;
            if (auto view = env_.book(symbol)) out.books.push_back(std::move(*view));
        };
        if (env_.watched) {
            for (const auto& symbol : env_.watched()) add(symbol);
        }
        for (const auto& strategy : out.strategies) {
            for (const auto& symbol : strategy.symbols) {
                add(symbol);
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        out.revision = revision_;
    }
    return out;
}

}  // namespace cpm::trader
