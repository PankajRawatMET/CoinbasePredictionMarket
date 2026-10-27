#include "cpm/oms/sim/engine.hpp"

#include <algorithm>

namespace cpm::oms::sim {
namespace {

// A single sort key per side: a bid wants the highest price first, an ask the
// lowest, and both want the oldest first at equal prices. Encoding the price
// into the key lets one multimap serve both.
std::int64_t sort_key(Side side, Price price, std::uint64_t sequence) {
    const std::int64_t by_price = side == Side::Buy ? -price : price;
    // Prices are 1..999 and sequences are small, so this packs without overlap.
    return by_price * 1'000'000'000LL + static_cast<std::int64_t>(sequence);
}

Amount notional_of(Qty quantity, Price price) {
    // quantity is hundredths and price thousandths, so their product is 10^-5;
    // amounts are 10^-6.
    return quantity * price * 10;
}

bool crosses(Side taker_side, Price taker_price, Price maker_price) {
    return taker_side == Side::Buy ? taker_price >= maker_price : taker_price <= maker_price;
}

}  // namespace

Engine::Engine(std::vector<Instrument> instruments, Fees fees)
    : instruments_(std::move(instruments)), fees_(fees) {}

const Instrument* Engine::find(const std::string& symbol) const {
    const auto it = std::find_if(instruments_.begin(), instruments_.end(),
                                 [&](const Instrument& i) { return i.symbol == symbol; });
    return it == instruments_.end() ? nullptr : &*it;
}

std::string Engine::next_order_id() { return std::to_string(next_order_ += 2); }
std::string Engine::next_trade_id() { return std::to_string(++next_trade_); }

Price Engine::worst_admissible(Side side) const {
    // A market order is forced to BUY 0.999 / SELL 0.001.
    return side == Side::Buy ? kPriceOne - 1 : 1;
}

Event Engine::event_for(const RestingOrder& order, Event::Kind kind, Clock::time_point now) const {
    Event event;
    event.kind = kind;
    event.owner = order.owner;
    event.cl_ord_id = order.cl_ord_id;
    event.order_id = order.order_id;
    event.symbol = order.symbol;
    event.subaccount = order.subaccount;
    event.side = order.side;
    event.order_quantity = order.quantity;
    event.leaves = order.leaves;
    event.cumulative = order.cumulative;
    event.price = order.price;
    event.type = order.type;
    event.time_in_force = order.time_in_force;
    event.post_only = order.post_only;
    // VWAP of the fills so far; zero before the first one, as the gateway emits.
    event.average_price = order.cumulative > 0
                              ? static_cast<Price>(order.filled_value / order.cumulative)
                              : 0;
    event.time = now;
    return event;
}

void Engine::rest(RestingOrder order) {
    const auto key = sort_key(order.side, order.price, ++sequence_);
    levels_[{order.symbol, order.side}].emplace(key, order.order_id);
    orders_.emplace(order.order_id, std::move(order));
}

void Engine::remove(const std::string& order_id) {
    const auto it = orders_.find(order_id);
    if (it == orders_.end()) return;
    auto& level = levels_[{it->second.symbol, it->second.side}];
    for (auto entry = level.begin(); entry != level.end(); ++entry) {
        if (entry->second == order_id) {
            level.erase(entry);
            break;
        }
    }
    orders_.erase(it);
}

std::vector<RestingOrder> Engine::book(const std::string& symbol, Side side) const {
    std::vector<RestingOrder> out;
    const auto level = levels_.find({symbol, side});
    if (level == levels_.end()) return out;
    for (const auto& [key, order_id] : level->second) {
        const auto order = orders_.find(order_id);
        if (order != orders_.end() && order->second.matchable) out.push_back(order->second);
    }
    return out;
}

RestingOrder* Engine::resolve(const std::string& order_id, const std::string& cl_ord_id,
                              const std::string& owner) {
    // OrderID wins when both are given, as the gateway does. Another session's
    // order is simply not found, which is reported as UNKNOWN_ORDER.
    if (!order_id.empty()) {
        const auto it = orders_.find(order_id);
        if (it == orders_.end() || it->second.owner != owner) return nullptr;
        return &it->second;
    }
    for (auto& [id, order] : orders_) {
        if (order.cl_ord_id == cl_ord_id && order.owner == owner) return &order;
    }
    return nullptr;
}

std::pair<Qty, Amount> Engine::available(const RestingOrder& taker) const {
    const Side other = taker.side == Side::Buy ? Side::Sell : Side::Buy;
    Qty quantity = 0;
    Amount cost = 0;
    for (const auto& maker : book(taker.symbol, other)) {
        if (!crosses(taker.side, taker.price, maker.price)) break;
        const Qty take = std::min(taker.leaves - quantity, maker.leaves);
        if (take <= 0) break;
        quantity += take;
        // All-in: the notional plus the taker fee, which is what the cap covers.
        const Amount notional = notional_of(take, maker.price);
        cost += notional + notional * fees_.taker_bps / 10000;
        if (quantity >= taker.leaves) break;
    }
    return {quantity, cost};
}

std::optional<Price> Engine::slide_price(const RestingOrder& order) const {
    // Post-only slide: step just inside the opposite best so it cannot cross.
    const Side other = order.side == Side::Buy ? Side::Sell : Side::Buy;
    const auto opposite = book(order.symbol, other);
    if (opposite.empty()) return order.price;
    const Price best = opposite.front().price;
    const Price slid = order.side == Side::Buy ? best - 1 : best + 1;
    if (slid <= 0 || slid >= kPriceOne) return std::nullopt;
    return slid;
}

Qty Engine::match(RestingOrder& taker, std::vector<Event>& out, Clock::time_point now) {
    const Side other = taker.side == Side::Buy ? Side::Sell : Side::Buy;
    Qty filled = 0;
    for (;;) {
        if (taker.leaves <= 0) break;
        const auto resting = book(taker.symbol, other);
        if (resting.empty()) break;
        const auto& front = resting.front();
        if (!crosses(taker.side, taker.price, front.price)) break;

        const Qty take = std::min(taker.leaves, front.leaves);
        const Price price = front.price;  // the resting order's price wins
        const auto trade_id = next_trade_id();
        const Amount notional = notional_of(take, price);

        // The maker first: its fill, then the taker's.
        RestingOrder& maker = orders_.at(front.order_id);
        maker.leaves -= take;
        maker.cumulative += take;
        maker.filled_value += take * price;
        Event maker_fill = event_for(maker, Event::Kind::Fill, now);
        maker_fill.last_quantity = take;
        maker_fill.last_price = price;
        maker_fill.trade_match_id = trade_id;
        maker_fill.aggressor = false;
        maker_fill.fee = notional * fees_.maker_bps / 10000;

        taker.leaves -= take;
        taker.cumulative += take;
        taker.filled_value += take * price;
        Event taker_fill = event_for(taker, Event::Kind::Fill, now);
        taker_fill.last_quantity = take;
        taker_fill.last_price = price;
        taker_fill.trade_match_id = trade_id;
        taker_fill.aggressor = true;
        taker_fill.fee = notional * fees_.taker_bps / 10000;

        out.push_back(std::move(taker_fill));
        out.push_back(std::move(maker_fill));
        filled += take;
        if (maker.leaves == 0) remove(front.order_id);
    }
    return filled;
}

std::vector<Event> Engine::submit(const NewOrder& order, const std::string& subaccount,
                                  Clock::time_point now, const std::string& owner) {
    std::vector<Event> out;
    const auto reject = [&](Event::Kind kind, int reason, std::string text) {
        Event event;
        event.kind = kind;
        event.owner = owner;
        event.cl_ord_id = order.cl_ord_id;
        event.symbol = order.symbol;
        event.subaccount = subaccount;
        event.side = order.side;
        event.order_quantity = order.quantity;
        event.price = order.price;
        event.type = order.type;
        event.time_in_force = order.time_in_force;
        event.reject_reason = reason;
        event.text = std::move(text);
        event.time = now;
        out.push_back(std::move(event));
        return out;
    };

    // Detectable on receipt, so answered with BusinessMessageReject(j) and no
    // ExecutionReport at all.
    if (subaccount.empty()) {
        return reject(Event::Kind::BusinessRejected, 0,
                      "INVALID_SUBACCOUNT: Parties with PartyRole=24 (CUSTOMER_ACCOUNT) is required");
    }
    if (order.type == OrderType::Limit && (order.price <= 0 || order.price >= kPriceOne)) {
        return reject(Event::Kind::BusinessRejected, 0,
                      "INVALID_FIELD_VALUE: Price(44) is required for a limit order");
    }
    if (order.type == OrderType::Market && order.price != 0) {
        return reject(Event::Kind::BusinessRejected, 0,
                      "INVALID_FIELD_VALUE: a market order takes no price");
    }
    if (order.type == OrderType::Market && order.post_only != PostOnly::None) {
        return reject(Event::Kind::BusinessRejected, 11, "POST_ONLY_WOULD_CROSS");
    }
    if (order.max_execution_cost && order.time_in_force != TimeInForce::Fok) {
        return reject(Event::Kind::BusinessRejected, 0,
                      "INVALID_TIME_IN_FORCE: MaxExecutionCost(21009) requires TimeInForce(59)=FOK");
    }
    if (order.max_execution_cost && *order.max_execution_cost <= 0) {
        return reject(Event::Kind::BusinessRejected, 0,
                      "INVALID_FIELD_VALUE: MaxExecutionCost(21009) must be positive");
    }
    if (order.cl_ord_id.size() > 16) {
        return reject(Event::Kind::BusinessRejected, 0, "INVALID_FIELD_VALUE: ClOrdID(11) too long");
    }

    // Everything else is answered with an ExecutionReport.
    const Instrument* instrument = find(order.symbol);
    if (instrument == nullptr) return reject(Event::Kind::Rejected, 1, "INVALID_INSTRUMENT");
    if (!instrument->active) return reject(Event::Kind::Rejected, 44, "INSTRUMENT_NOT_ACTIVE");
    if (order.quantity <= 0) return reject(Event::Kind::Rejected, 13, "INVALID_QUANTITY");
    // ClOrdID must be unique among the user's own live orders.
    for (const auto& [id, existing] : orders_) {
        if (existing.owner == owner && existing.cl_ord_id == order.cl_ord_id) {
            return reject(Event::Kind::Rejected, 6, "DUPLICATE_CLIENT_ORDER_ID");
        }
    }

    RestingOrder resting;
    resting.order_id = next_order_id();
    resting.cl_ord_id = order.cl_ord_id;
    resting.owner = owner;
    resting.subaccount = subaccount;
    resting.symbol = order.symbol;
    resting.side = order.side;
    resting.quantity = order.quantity;
    resting.leaves = order.quantity;
    resting.type = order.type;
    resting.post_only = order.post_only;
    resting.accepted_at = now;
    // A market order is forced to the worst admissible price and to IOC.
    resting.price = order.type == OrderType::Market ? worst_admissible(order.side) : order.price;
    resting.time_in_force = order.type == OrderType::Market ? TimeInForce::Ioc : order.time_in_force;

    // Post-only, before anything can trade.
    if (resting.post_only != PostOnly::None) {
        const auto [quantity, cost] = available(resting);
        (void)cost;
        if (quantity > 0) {
            if (resting.post_only == PostOnly::Reject) {
                return reject(Event::Kind::Rejected, 11, "POST_ONLY_WOULD_CROSS");
            }
            const auto slid = slide_price(resting);
            if (!slid) return reject(Event::Kind::Rejected, 11, "POST_ONLY_WOULD_CROSS");
            resting.price = *slid;
        }
    }

    // A speed bump holds the order before it can match.
    if (instrument->speed_bump.count() > 0) {
        resting.matchable = false;
        auto pending = event_for(resting, Event::Kind::PendingNew, now);
        rest(std::move(resting));
        out.push_back(std::move(pending));
        return out;
    }

    // FOK and the spend cap are both decided before anything executes.
    if (resting.time_in_force == TimeInForce::Fok) {
        const auto [quantity, cost] = available(resting);
        if (quantity < resting.leaves) {
            auto event = event_for(resting, Event::Kind::Expired, now);
            event.leaves = 0;
            event.text = "FOK_NOT_FILLED";
            out.push_back(std::move(event));
            return out;
        }
        if (order.max_execution_cost && cost > *order.max_execution_cost) {
            auto event = event_for(resting, Event::Kind::Rejected, now);
            event.leaves = 0;
            event.reject_reason = 3;
            event.text = "MAX_EXECUTION_COST_EXCEEDED - insufficient liquidity";
            out.push_back(std::move(event));
            return out;
        }
    }

    const auto order_id = resting.order_id;
    rest(std::move(resting));
    RestingOrder& live = orders_.at(order_id);
    match(live, out, now);

    if (live.leaves == 0) {
        remove(order_id);
        return out;
    }
    if (live.time_in_force == TimeInForce::Ioc) {
        // The unfilled remainder of an IOC expires; it is never Canceled.
        auto event = event_for(live, Event::Kind::Expired, now);
        event.leaves = 0;
        event.text = "TIME_IN_FORCE";
        remove(order_id);
        out.push_back(std::move(event));
        return out;
    }
    // Resting. The New report comes after any fills the order already took.
    out.push_back(event_for(live, Event::Kind::New, now));
    return out;
}

std::vector<Event> Engine::cancel(const CancelOrder& request, Clock::time_point now,
                                  const std::string& owner) {
    std::vector<Event> out;
    RestingOrder* order = resolve(request.order_id, request.orig_cl_ord_id, owner);
    if (order == nullptr) {
        Event event;
        event.kind = Event::Kind::CancelRejected;
        event.owner = owner;
        event.cl_ord_id = request.cl_ord_id;
        event.orig_cl_ord_id = request.orig_cl_ord_id;
        event.order_id = request.order_id.empty() ? "0" : request.order_id;
        event.cancel_reject_reason = 1;
        event.text = "UNKNOWN_ORDER";
        event.time = now;
        out.push_back(std::move(event));
        return out;
    }
    auto event = event_for(*order, Event::Kind::Canceled, now);
    // The report names the cancel as ClOrdID and the order as OrigClOrdID
    // (OE-O2), and a cancel we asked for carries no ExecRestatementReason.
    event.cl_ord_id = request.cl_ord_id;
    event.orig_cl_ord_id = order->cl_ord_id;
    event.leaves = 0;
    event.text = "USER_REQUEST";
    const auto order_id = order->order_id;
    out.push_back(std::move(event));
    remove(order_id);
    return out;
}

std::vector<Event> Engine::mass_cancel(const MassCancel& request, const std::string& subaccount,
                                       Clock::time_point now, const std::string& owner) {
    std::vector<Event> out;
    std::vector<std::string> doomed;
    for (const auto& [order_id, order] : orders_) {
        // Only the requesting session's own orders.
        if (order.owner != owner) continue;
        // Filters AND together. Scopes 6 and 7 are the same: an API key has one
        // session, so "this session's orders" and "all my orders" coincide.
        if (request.scope == MassCancel::Scope::Security && order.symbol != request.symbol) continue;
        if (request.side && order.side != *request.side) continue;
        if (request.filter_by_subaccount && !subaccount.empty() && order.subaccount != subaccount) continue;
        doomed.push_back(order_id);
    }
    for (const auto& order_id : doomed) {
        auto event = event_for(orders_.at(order_id), Event::Kind::Canceled, now);
        event.cl_ord_id = orders_.at(order_id).cl_ord_id;
        event.leaves = 0;
        // Per-order reports precede the summary, and carry no 378.
        event.text = "USER_MASS_CANCEL";
        out.push_back(std::move(event));
        remove(order_id);
    }
    return out;
}

std::vector<Event> Engine::release_speed_bumped(Clock::time_point now) {
    std::vector<Event> out;
    std::vector<std::string> ready;
    for (const auto& [order_id, order] : orders_) {
        if (order.matchable) continue;
        const Instrument* instrument = find(order.symbol);
        if (instrument == nullptr) continue;
        if (now - order.accepted_at >= instrument->speed_bump) ready.push_back(order_id);
    }
    for (const auto& order_id : ready) {
        RestingOrder& order = orders_.at(order_id);
        order.matchable = true;
        match(order, out, now);
        if (order.leaves == 0) {
            remove(order_id);
            continue;
        }
        if (order.time_in_force == TimeInForce::Ioc) {
            auto event = event_for(order, Event::Kind::Expired, now);
            event.leaves = 0;
            event.text = "TIME_IN_FORCE";
            remove(order_id);
            out.push_back(std::move(event));
            continue;
        }
        out.push_back(event_for(order, Event::Kind::New, now));
    }
    return out;
}

}  // namespace cpm::oms::sim
