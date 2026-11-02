#include "cpm/gui/views.hpp"

#include "cpm/md/ticker.hpp"
#include "cpm/md/ticks.hpp"
#include "cpm/oms/orders.hpp"
#include "cpm/strategy/host.hpp"

namespace cpm::gui {
namespace {

using cpm::trader::BookView;
using cpm::trader::KindView;
using cpm::trader::LadderLevel;
using cpm::trader::PnlView;
using cpm::trader::PositionView;
using cpm::trader::SessionView;
using cpm::trader::Snapshot;
using cpm::strategy::ParamSpec;
using cpm::strategy::StrategyInfo;

const char* level_name(cpm::LogLevel level) {
    switch (level) {
        case cpm::LogLevel::Debug: return "DEBUG";
        case cpm::LogLevel::Info: return "INFO";
        case cpm::LogLevel::Warn: return "WARN";
        case cpm::LogLevel::Error: return "ERROR";
    }
    return "?";
}

// What the operator calls the thing a strategy is on: BTCD15M, not the
// quarter-hour window it happens to be quoting (GUI-Q26). A strategy that
// follows a series already carries it; one pinned to a ticker has it as the
// first field of that ticker, which is what parse_ticker reads.
std::string series_of(const StrategyInfo& strategy) {
    if (!strategy.target.empty()) return strategy.target;
    if (strategy.symbols.empty()) return {};
    const auto& symbol = strategy.symbols.front();
    if (const auto parsed = cpm::md::parse_ticker(symbol)) return parsed->series;
    // A ticker this build cannot take apart - the simulator's, or a shape
    // the venue has not shown us - still carries its series first, because
    // every instrument name on this venue begins with one.
    const auto dash = symbol.find('-');
    return dash == std::string::npos ? symbol : symbol.substr(0, dash);
}

void write_session(JsonWriter& out, std::string_view name, const SessionView& session) {
    out.key(name).object_begin();
    out.field("logged_on", session.logged_on);
    out.field("last_reason", session.last_reason);
    out.object_end();
}

void write_strategy(JsonWriter& out, const StrategyInfo& strategy) {
    out.object_begin();
    out.field("id", strategy.id);
    out.field("kind", strategy.kind);
    // The series it follows, empty when it is pinned to one instrument. The
    // symbol below changes under it at every roll; this does not.
    out.field("target", strategy.target);
    // Series whether or not it follows one, so the page can show the name
    // the operator types rather than the window it resolved to.
    out.field("series", series_of(strategy));
    out.field("state", to_string(strategy.state));
    out.field("armed", strategy.armed);
    // Whether its instrument is subscribed. A row whose feed is stopped
    // keeps its place, its symbol and its parameters, and Start Feed is the
    // only thing left that can be done to it.
    out.field("feed", strategy.feed);
    out.field("note", strategy.note);
    out.field("reason", strategy.status.reason);

    out.key("params").object_begin();
    for (const auto& param : strategy.params) out.field(param.first, param.second);
    out.object_end();

    out.key("symbols").array_begin();
    for (const auto& symbol : strategy.symbols) out.string(symbol);
    out.array_end();

    out.key("legs").array_begin();
    for (const auto& leg : strategy.status.legs) {
        out.object_begin();
        out.field("side", to_string(leg.side));
        out.field("cl_ord_id", leg.cl_ord_id);
        out.field_percent("price", leg.price);
        // The same price as an exact integer in ticks. The page never does
        // arithmetic on a price - that is what the string above is for - but
        // it has to match an order to a rung and order the rungs, and doing
        // either on "0.585" would mean parsing a decimal in JavaScript.
        out.field("price_ticks", leg.price);
        out.field_qty("quantity", leg.quantity);
        out.field("cancel_pending", leg.cancel_pending);
        out.object_end();
    }
    out.array_end();

    out.key("counts").object_begin();
    out.field("passes", strategy.status.passes);
    out.field("passes_without_target", strategy.status.passes_without_target);
    out.field("placed", strategy.status.placed);
    out.field("cancelled", strategy.status.cancelled);
    out.field("fills", strategy.status.fills);
    out.object_end();

    out.object_end();
}

// One side of the ladder. Every rung carries its price twice: as the text the
// page prints, and as the exact tick count it sorts and matches on.
void write_ladder(JsonWriter& out, std::string_view name,
                  const std::vector<cpm::trader::LadderLevel>& levels) {
    out.key(name).array_begin();
    for (const auto& level : levels) {
        out.object_begin();
        out.field_percent("price", level.price);
        out.field("price_ticks", level.price);
        out.field_qty("qty", level.qty);
        // What the venue is actually holding at this rung, which on the ask
        // side is a NO bid at one minus the price (Q9).
        out.field_percent("venue_price", level.venue_price);
        out.object_end();
    }
    out.array_end();
}

void write_book(JsonWriter& out, const BookView& book) {
    out.object_begin();
    out.field("symbol", book.symbol);
    out.field("valid", book.valid);
    out.field("status", book.status);
    out.field("crossed_or_locked", book.crossed_or_locked);
    out.field_percent("best_yes_bid", book.best_yes_bid);
    out.field_qty("best_yes_bid_qty", book.best_yes_bid_qty);
    out.field_percent("implied_yes_ask", book.implied_yes_ask);
    out.field_qty("best_no_bid_qty", book.best_no_bid_qty);
    out.field_percent("mid", book.mid);
    out.field_percent("spread", book.spread);
    out.field_percent("last_trade", book.last_trade);
    out.field_qty("volume", book.volume);
    out.field_qty("open_interest", book.open_interest);
    write_ladder(out, "bids", book.bids);
    write_ladder(out, "asks", book.asks);
    out.object_end();
}

void write_position(JsonWriter& out, const PositionView& position) {
    out.object_begin();
    out.field("symbol", position.symbol);
    out.field_qty("net", position.net);
    out.field_qty("bought", position.bought);
    out.field_qty("sold", position.sold);
    out.field_scaled("cash", position.cash, kAmtScale);
    out.field_scaled("bought_value", position.bought_value, kAmtScale);
    out.field_scaled("sold_value", position.sold_value, kAmtScale);
    out.field_scaled("fees", position.fees, kAmtScale);
    out.field("fills", position.fills);
    out.field_scaled("settlement_payout", position.settlement_payout, kAmtScale);
    out.object_end();
}

// P&L (GUI-Q15). The unrealized half and the total are null, not zero, when
// there is no mark for an open position: the page shows a dash for them,
// which is the difference between "flat" and "not known".
void write_pnl(JsonWriter& out, const PnlView& pnl) {
    out.object_begin();
    out.field("symbol", pnl.symbol);
    // The mark as a percentage, the same way every other price reaches the
    // page.
    out.field_percent("mark", pnl.mark);
    out.field_scaled("realized", pnl.realized, kAmtScale);
    out.field_scaled("unrealized", pnl.unrealized, kAmtScale);
    out.field_scaled("total", pnl.total, kAmtScale);
    out.object_end();
}

}  // namespace

std::string snapshot_json(const Snapshot& snapshot) {
    JsonWriter out;
    out.object_begin();
    out.field("state", to_string(snapshot.state));
    // Every price in this document is a percentage chance: "59.9", not
    // "0.599". The daemon's own logs and the stdin console still speak in the
    // wire's units, which is the form the venue and the FIX messages use.
    out.field("price_unit", "percent");
    out.field("allow_orders", snapshot.allow_orders);
    out.field("halt_reason", snapshot.halt_reason);
    out.field("recovery", snapshot.recovery);
    out.field("reference", snapshot.reference);
    out.field("revision", snapshot.revision);
    write_session(out, "market_data", snapshot.market_data);
    write_session(out, "order_entry", snapshot.order_entry);

    out.key("limits").object_begin();
    out.field("allow_orders", snapshot.limits.allow_orders);
    out.field_scaled("max_quantity", snapshot.limits.max_quantity, kQtyScale);
    out.field_scaled("max_notional", snapshot.limits.max_notional, kAmtScale);
    // A rate, not a price: the one number here that is genuinely fractional
    // and never reaches an order, so it goes as the text of a rounded rate
    // rather than pretending to a scale it does not have.
    out.field("max_orders_per_sec",
              format_signed(static_cast<std::int64_t>(snapshot.limits.max_orders_per_sec * 100), 2));
    out.object_end();

    out.key("strategies").array_begin();
    for (const auto& strategy : snapshot.strategies) write_strategy(out, strategy);
    out.array_end();

    out.key("books").array_begin();
    for (const auto& book : snapshot.books) write_book(out, book);
    out.array_end();

    out.key("positions").array_begin();
    for (const auto& position : snapshot.positions) write_position(out, position);
    out.array_end();

    out.key("pnl").array_begin();
    for (const auto& pnl : snapshot.pnl) write_pnl(out, pnl);
    out.array_end();
    out.key("pnl_total").object_begin();
    out.field_scaled("realized", snapshot.pnl_total.realized, kAmtScale);
    out.field_scaled("unrealized", snapshot.pnl_total.unrealized, kAmtScale);
    out.field_scaled("total", snapshot.pnl_total.total, kAmtScale);
    out.object_end();

    out.object_end();
    return out.take();
}

std::string schema_json(const std::vector<KindView>& kinds) {
    JsonWriter out;
    out.object_begin();
    out.key("kinds").array_begin();
    for (const auto& kind : kinds) {
        out.object_begin();
        out.field("name", kind.name);
        out.field("help", kind.help);
        out.key("params").array_begin();
        for (const auto& spec : kind.schema) {
            out.object_begin();
            out.field("name", spec.name);
            out.field("label", spec.label);
            out.field("type", to_string(spec.type));
            out.field("default", spec.default_value);
            out.field("required", spec.required());
            out.field("help", spec.help);
            out.field("min", spec.min);
            out.field("max", spec.max);
            out.field("scale", spec.scale);
            out.key("choices").array_begin();
            for (const auto& choice : spec.choices) out.string(choice);
            out.array_end();
            out.object_end();
        }
        out.array_end();
        out.object_end();
    }
    out.array_end();
    out.object_end();
    return out.take();
}

std::string axis_json(const cpm::trader::AxisView& axis) {
    JsonWriter out;
    out.object_begin();
    out.field("symbol", axis.symbol);
    out.field("structure", axis.structure);
    out.key("rungs").array_begin();
    for (const auto price : axis.prices) {
        out.object_begin();
        // The same two forms as everywhere else: the text to print, and the
        // exact tick count to order and match on.
        out.field_percent("price", price);
        out.field("price_ticks", price);
        // 0 and 100 are on the grid but are not order prices (O5).
        out.field("quotable", cpm::md::is_quotable(price));
        out.object_end();
    }
    out.array_end();
    out.object_end();
    return out.take();
}

std::string result_json(const cpm::trader::Result& result, std::uint64_t revision) {
    JsonWriter out;
    out.object_begin();
    out.field("ok", result.ok());
    if (!result.id.empty()) out.field("id", result.id);
    if (!result.ok()) out.field("error", result.error);
    out.field("revision", revision);
    out.object_end();
    return out.take();
}

std::string logs_json(const std::vector<cpm::LogRing::Line>& lines) {
    JsonWriter out;
    out.object_begin();
    out.key("lines").array_begin();
    for (const auto& line : lines) {
        out.object_begin();
        out.field("seq", static_cast<std::int64_t>(line.seq));
        out.field("at", line.stamp);
        out.field("level", level_name(line.level));
        out.field("text", line.text);
        out.object_end();
    }
    out.array_end();
    out.object_end();
    return out.take();
}

std::string error_json(std::string_view message) {
    JsonWriter out;
    out.object_begin();
    out.field("ok", false);
    out.field("error", message);
    out.object_end();
    return out.take();
}

}  // namespace cpm::gui
