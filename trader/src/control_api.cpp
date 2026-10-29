#include "cpm/trader/control_api.hpp"

#include "cpm/md/ticker.hpp"

#include <algorithm>
#include <utility>

namespace cpm::trader {

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
                           "not configured (creds-rest.env)";
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
    Result result{host_.arm(id), {}};
    if (result.ok()) bump();
    return result;
}

Result ControlApi::disarm(const std::string& id) {
    Result result{host_.disarm(id), {}};
    if (result.ok()) bump();
    return result;
}

Result ControlApi::destroy(const std::string& id) {
    // Read the symbols before it goes: afterwards there is nothing to ask.
    const auto info = host_.info(id);
    Result result{host_.destroy(id), {}};
    if (!result.ok()) return result;
    if (info && env_.unsubscribe) {
        for (const auto& symbol : info->symbols) env_.unsubscribe(symbol);
    }
    bump();
    return result;
}

Result ControlApi::stop_all() {
    host_.stop_all("stop all");
    bump();
    return {};
}

AxisView ControlApi::axis(const std::string& symbol) const {
    AxisView view;
    view.symbol = symbol;
    bool claimed = false;
    for (const auto& strategy : host_.list()) {
        for (const auto& name : strategy.symbols) {
            if (name == symbol) claimed = true;
        }
    }
    if (!claimed || !env_.axis) return view;
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
    if (env_.limits) out.limits = env_.limits();

    // One book per symbol some strategy claims, in the order the strategies
    // appear, without duplicates.
    if (env_.book) {
        for (const auto& strategy : out.strategies) {
            for (const auto& symbol : strategy.symbols) {
                const bool seen = std::any_of(out.books.begin(), out.books.end(),
                                              [&](const BookView& view) { return view.symbol == symbol; });
                if (seen) continue;
                if (auto view = env_.book(symbol)) out.books.push_back(std::move(*view));
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
