#include "cpm/md/sim/market.hpp"

#include "cpm/common/fixed.hpp"

#include <algorithm>
#include <stdexcept>

namespace cpm::md::sim {
namespace {

constexpr int kCent = 10;  // 0.01 in thousandths

int index(Side s) { return s == Side::Yes ? 0 : 1; }
Side other(Side s) { return s == Side::Yes ? Side::No : Side::Yes; }

// TAPERED_DECI_CENT (spec REST §6.4): 0.001 ticks up to 0.100 and from 0.900,
// 0.01 ticks in between. Order prices exclude 0 and 1.
bool valid_price(Price p) {
    if (p <= 0 || p >= kPriceOne) return false;
    if (p > 100 && p < 900) return p % kCent == 0;
    return true;
}

// Largest valid price <= p (0 if none).
Price floor_valid(Price p) {
    for (; p > 0; --p) {
        if (valid_price(p)) return p;
    }
    return 0;
}

std::optional<Level> best(const std::vector<Level>& side) {
    if (side.empty()) return std::nullopt;
    return side.front();
}

int position(const std::vector<Level>& side, Price price) {
    for (std::size_t i = 0; i < side.size(); ++i) {
        if (side[i].price == price) return static_cast<int>(i) + 1;
    }
    return 0;
}

}  // namespace

void apply_delta(std::vector<Level>& side, const BookDelta& delta) {
    const auto it = std::find_if(side.begin(), side.end(), [&](const Level& l) { return l.price == delta.price; });
    if (delta.action == BookDelta::Action::Delete || delta.qty == 0) {
        if (it != side.end()) side.erase(it);
        return;
    }
    if (it != side.end()) {
        it->qty = delta.qty;
        return;
    }
    const auto pos = std::find_if(side.begin(), side.end(), [&](const Level& l) { return l.price < delta.price; });
    side.insert(pos, Level{delta.price, delta.qty});
}

// Same wire formatting the client parses back (cpm/common/fixed.hpp).
std::string format_price(Price p) { return cpm::format_price(p); }
std::string format_qty(Qty q) { return cpm::format_qty(q); }

Market::Market(std::vector<InstrumentSpec> instruments, std::uint64_t seed, Clock::time_point now) : rng_(seed) {
    for (const auto& spec : instruments) instruments_.emplace(spec.symbol, make(spec, now));
}

int Market::uniform(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng_); }

Instrument Market::make(const InstrumentSpec& spec, Clock::time_point now) {
    Instrument inst;
    inst.symbol = spec.symbol;
    inst.security_id = spec.security_id;
    inst.fair = spec.fair;
    inst.last_book_change = now;
    // YES bids below fair, NO bids below (1 - fair): best YES + best NO = 0.98.
    const int levels = std::clamp(spec.levels, 0, static_cast<int>(kMaxLevels));
    for (int k = 1; k <= levels; ++k) {
        const Price yes = floor_valid(spec.fair - k * kCent);
        const Price no = floor_valid(kPriceOne - spec.fair - k * kCent);
        if (yes > 0) inst.yes.push_back({yes, uniform(1, 10) * 100});
        if (no > 0) inst.no.push_back({no, uniform(1, 10) * 100});
    }
    return inst;
}

const Instrument* Market::find(const std::string& symbol) const {
    const auto it = instruments_.find(symbol);
    return it == instruments_.end() ? nullptr : &it->second;
}

std::vector<std::string> Market::symbols() const {
    std::vector<std::string> out;
    for (const auto& [symbol, inst] : instruments_) out.push_back(symbol);
    return out;
}

Instrument& Market::pick_open() {
    std::vector<Instrument*> open;
    for (auto& [symbol, inst] : instruments_) {
        if (inst.status == Status::Open) open.push_back(&inst);
    }
    if (open.empty()) throw std::logic_error("no open instrument");
    return *open[static_cast<std::size_t>(uniform(0, static_cast<int>(open.size()) - 1))];
}

Step Market::step(Clock::time_point now) {
    Step out;
    out.time = now;
    const bool any_open = std::any_of(instruments_.begin(), instruments_.end(),
                                      [](const auto& kv) { return kv.second.status == Status::Open; });
    if (!any_open) return out;

    Instrument& inst = pick_open();
    out.symbol = inst.symbol;
    out.security_id = inst.security_id;
    for (Side s : {Side::Yes, Side::No}) out.best_before[index(s)] = best(inst.side(s));

    // Fair value drifts by at most one cent per event, within 0.15-0.85.
    inst.fair = std::clamp<Price>(inst.fair + uniform(-1, 1) * kCent, 150, 850);

    const int dice = uniform(0, 99);
    if (dice < 50) {
        add_order(inst, out);
    } else if (dice < 75) {
        cancel_order(inst, out);
    } else {
        trade(inst, out);
    }
    if (!out.book.empty()) inst.last_book_change = now;
    for (Side s : {Side::Yes, Side::No}) out.best_after[index(s)] = best(inst.side(s));
    out.volume = inst.volume;
    out.open_interest = inst.open_interest;
    return out;
}

void Market::add_order(Instrument& inst, Step& out) {
    const Side side = uniform(0, 1) == 0 ? Side::Yes : Side::No;
    auto& levels = side == Side::Yes ? inst.yes : inst.no;
    const auto opposite = best(inst.side(other(side)));
    // Strictly below 1 - best opposite bid, so the book never locks or crosses.
    const Price limit = kPriceOne - (opposite ? opposite->price : 0) - 1;
    const Price centre = side == Side::Yes ? inst.fair : kPriceOne - inst.fair;
    Price price = floor_valid(std::min(limit, centre - uniform(1, 6) * kCent));
    if (price <= 0) return;

    const Qty add = uniform(1, 10) * 100 + (uniform(0, 3) == 0 ? 50 : 0);  // sometimes fractional
    int pos = position(levels, price);
    if (pos == 0 && levels.size() >= kMaxLevels) {
        // Side is full: add to an existing level instead.
        pos = uniform(1, static_cast<int>(levels.size()));
        price = levels[static_cast<std::size_t>(pos) - 1].price;
    }
    BookDelta delta{side, BookDelta::Action::New, price, add, 0};
    if (pos != 0) {
        delta.action = BookDelta::Action::Change;
        delta.qty = levels[static_cast<std::size_t>(pos) - 1].qty + add;
    }
    apply_delta(levels, delta);
    delta.level = position(levels, price);
    out.book.push_back(delta);
}

void Market::cancel_order(Instrument& inst, Step& out) {
    Side side = uniform(0, 1) == 0 ? Side::Yes : Side::No;
    if (inst.side(side).empty()) side = other(side);
    auto& levels = side == Side::Yes ? inst.yes : inst.no;
    if (levels.empty()) return add_order(inst, out);

    const auto& level = levels[static_cast<std::size_t>(uniform(0, static_cast<int>(levels.size()) - 1))];
    BookDelta delta{side, BookDelta::Action::Delete, level.price, 0, 0};
    if (level.qty > 100 && uniform(0, 1) == 0) {
        // Partial cancel: half the level stays.
        delta.action = BookDelta::Action::Change;
        delta.qty = level.qty / 2;
    }
    apply_delta(levels, delta);
    if (delta.action == BookDelta::Action::Change) delta.level = position(levels, delta.price);
    out.book.push_back(delta);
}

void Market::trade(Instrument& inst, Step& out) {
    Side side = uniform(0, 1) == 0 ? Side::Yes : Side::No;
    if (inst.side(side).empty()) side = other(side);
    auto& levels = side == Side::Yes ? inst.yes : inst.no;
    if (levels.empty()) return add_order(inst, out);

    const Level hit = levels.front();
    const Qty qty = std::min<Qty>(hit.qty, uniform(1, 5) * 100);
    Trade t;
    t.qty = qty;
    t.entry_id = std::to_string(next_entry_id_++);
    if (side == Side::No) {
        t.yes_price = kPriceOne - hit.price;  // bought YES against a NO bid
        t.aggressor = 1;
    } else {
        t.yes_price = hit.price;  // sold YES into a YES bid
        t.aggressor = 2;
    }
    out.trade = t;

    BookDelta delta{side, BookDelta::Action::Change, hit.price, hit.qty - qty, 1};
    if (delta.qty == 0) {
        delta.action = BookDelta::Action::Delete;
        delta.level = 0;
    }
    apply_delta(levels, delta);
    out.book.push_back(delta);

    inst.volume += qty;
    if (uniform(0, 1) == 0 || inst.open_interest < qty) {
        inst.open_interest += qty;
    } else {
        inst.open_interest -= qty;
    }
    out.stats_changed = true;
}

Roll Market::roll(const std::string& symbol, InstrumentSpec next, Clock::time_point now) {
    const auto it = instruments_.find(symbol);
    if (it == instruments_.end()) throw std::invalid_argument("roll: unknown instrument " + symbol);
    Instrument& inst = it->second;

    Roll out;
    out.closing.push_back({inst.symbol, inst.security_id, Status::Closed, now});
    inst.status = Status::Closed;

    out.cleared.symbol = inst.symbol;
    out.cleared.security_id = inst.security_id;
    out.cleared.time = now;
    for (Side s : {Side::Yes, Side::No}) out.cleared.best_before[index(s)] = best(inst.side(s));
    for (Side s : {Side::Yes, Side::No}) {
        for (const auto& level : inst.side(s)) {
            out.cleared.book.push_back({s, BookDelta::Action::Delete, level.price, 0, 0});
        }
    }
    inst.yes.clear();
    inst.no.clear();
    if (!out.cleared.book.empty()) inst.last_book_change = now;

    inst.status = Status::Determined;
    out.after.push_back({inst.symbol, inst.security_id, Status::Determined, now});

    Instrument fresh = make(next, now);
    out.after.push_back({fresh.symbol, fresh.security_id, Status::Listed, now});
    out.after.push_back({fresh.symbol, fresh.security_id, Status::Open, now});
    fresh.status = Status::Open;
    instruments_.insert_or_assign(fresh.symbol, std::move(fresh));
    return out;
}

StatusEvent Market::settle(const std::string& symbol, Clock::time_point now) {
    const auto it = instruments_.find(symbol);
    if (it == instruments_.end()) throw std::invalid_argument("settle: unknown instrument " + symbol);
    StatusEvent event{it->second.symbol, it->second.security_id, Status::Settled, now};
    instruments_.erase(it);
    return event;
}

}  // namespace cpm::md::sim
