#include "cpm/strategy/quoting_mm.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace cpm::strategy {
namespace {

std::int64_t distance(std::int64_t a, std::int64_t b) { return a > b ? a - b : b - a; }

std::string decimal(double value, int precision) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(precision) << value;
    return out.str();
}

bool inventory_limit(const Position& position, const QuotingMmParams& p) {
    return p.max_inventory > 0 &&
           (position.net >= p.max_inventory || position.net <= -p.max_inventory);
}

// `steps` grid prices above or below `price`, or nullopt when the grid runs
// out before then. Stepped rather than multiplied: a tick is not one size
// everywhere, and arithmetic on tick_at() lands off the grid wherever the
// taper changes it (0.100 and 0.900 on the venue's own structure).
std::optional<Price> step_up(Price price, int steps) {
    const Price maximum = cpm::md::price_below(kPriceOne);
    for (int i = 0; i < steps; ++i) {
        if (price >= maximum) return std::nullopt;
        price = cpm::md::price_above(price);
    }
    return price;
}

std::optional<Price> step_down(Price price, int steps) {
    const Price minimum = cpm::md::price_above(0);
    for (int i = 0; i < steps; ++i) {
        if (price <= minimum) return std::nullopt;
        price = cpm::md::price_below(price);
    }
    return price;
}

// The pair of grid prices exactly `width` steps apart that straddles fair
// most evenly.
//
// The width is a HARD CAP, not a minimum: a quoting obligation says the
// market we show may not be wider than the one we promised, so neither leg
// is ever rounded away from the other to reach the grid. Both legs come off
// the grid by construction instead, and the pair is slid as a pair until it
// sits as near centred on fair as the grid allows - at most half a step of
// skew either way.
//
// Nullopt when the grid itself has no such pair straddling fair, which is
// only near 0 and 1 where it runs out.
std::optional<std::pair<Price, Price>> straddling_pair(double center, int width) {
    if (width < 1) return std::nullopt;
    const auto start = static_cast<Price>(std::floor(center));
    // 0 and 1.000 are on the grid but are settlement values, not prices an
    // order may rest at (O5), so the market lives strictly between them.
    const Price minimum = cpm::md::price_above(0);
    std::optional<std::pair<Price, Price>> best;
    double best_skew = 0;
    // Every bid within `width` steps below fair is a candidate, and nothing
    // else can straddle it.
    std::optional<Price> bid = cpm::md::snap_down(std::clamp<Price>(start, 0, kPriceOne));
    for (int i = 0; bid && i <= width; ++i, bid = step_down(*bid, 1)) {
        if (*bid < minimum) break;
        const auto ask = step_up(*bid, width);
        if (!ask) continue;
        if (static_cast<double>(*bid) >= center || static_cast<double>(*ask) <= center) continue;
        const double skew = std::abs((static_cast<double>(*bid) + static_cast<double>(*ask)) / 2.0 - center);
        // Ties go to the lower pair: of two equally skewed markets, the one
        // that buys cheaper is the one to show.
        if (!best || skew < best_skew) {
            best = std::make_pair(*bid, *ask);
            best_skew = skew;
        }
    }
    return best;
}

}  // namespace

DesiredQuotes quoting_mm_quotes(double fair, const InstrumentView& book,
                                 const Position& position, const QuotingMmParams& p) {
    DesiredQuotes out;
    if (inventory_limit(position, p)) {
        out.stop = true;
        out.reason = "inventory limit reached: net " + cpm::format_signed(position.net, cpm::kQtyScale);
        return out;
    }
    if (!std::isfinite(fair) || fair < 0 || fair > 1) {
        out.reason = "invalid model probability";
        return out;
    }
    // Empty and one-sided VALID snapshots are intentional markets to make.
    // A missing/invalid snapshot still means we cannot trust crossing checks.
    if (!book.book_valid) {
        out.reason = "waiting for a valid prediction-market snapshot";
        return out;
    }
    if (!book.yes.empty() && !book.no.empty() &&
        book.yes.front().price >= kPriceOne - book.no.front().price) {
        out.reason = "crossed or locked prediction market";
        return out;
    }
    const double center = fair * kPriceOne;
    const auto pair = straddling_pair(center, p.spread_ticks);
    if (!pair) {
        out.reason = "no room for model quotes on the price grid";
        return out;
    }
    const Price bid = pair->first;
    const Price ask = pair->second;
    // Neither leg may rest at or inside the opposing touch - that is not
    // making a market, it is taking one. The old code moved the offending
    // leg clear, which widened the quote beyond what was asked for. Under a
    // quoting obligation that is not available, and neither is sliding the
    // pair, which would post prices the model does not believe. So it stands
    // aside and says so until the book leaves room.
    if (!book.no.empty() && bid >= kPriceOne - book.no.front().price) {
        out.reason = "a " + std::to_string(p.spread_ticks) +
                     "-tick market would bid at or through the best offer (" +
                     format_price(kPriceOne - book.no.front().price) + ")";
        return out;
    }
    if (!book.yes.empty() && ask <= book.yes.front().price) {
        out.reason = "a " + std::to_string(p.spread_ticks) +
                     "-tick market would offer at or through the best bid (" +
                     format_price(book.yes.front().price) + ")";
        return out;
    }
    Qty buy_qty = p.quantity, sell_qty = p.quantity;
    if (p.max_inventory > 0) {
        // The reconciler keeps at most one live order per side and waits for
        // cancel confirmation. Cap that order's leaves by worst-case fills
        // on its side; do not assume the opposing order fills first.
        buy_qty = std::min(buy_qty, p.max_inventory - position.net);
        sell_qty = std::min(sell_qty, p.max_inventory + position.net);
    }
    if (buy_qty > 0) out.bid = Quote{bid, buy_qty};
    if (sell_qty > 0) out.ask = Quote{ask, sell_qty};
    return out;
}

QuotingMm::QuotingMm(QuotingMmParams params, StrategyHooks hooks)
    : params_(std::move(params)),
      orders_({params_.symbol, 1}, hooks),
      ticker_(cpm::md::parse_ticker(params_.symbol)),
      log_(std::move(hooks.log)) {
    last_now_ = Clock::now();
    if (!ticker_ || !ticker_->series.ends_with("15M")) {
        throw std::invalid_argument("QuotingMM requires a dated 15-minute instrument");
    }
    if (!std::isfinite(params_.correction) || params_.correction <= 0) {
        throw std::invalid_argument("QuotingMM needs a positive calibrated correction-factor");
    }
    // Both come from the warmup the factory did, so an instance that exists
    // can price. There is no state in which this strategy is waiting for a
    // volatility to arrive.
    if (!std::isfinite(params_.sigma) || params_.sigma <= 0) {
        throw std::invalid_argument("QuotingMM needs a positive sigma from the window's warmup");
    }
    opening_ = params_.warmup_opening;
    if (!opening_ || *opening_ <= 0) {
        throw std::invalid_argument("QuotingMM needs this window's opening price");
    }
}

void QuotingMm::stand_aside(const std::string& why) {
    reason_ = why;
    if (last_log_ != why) {
        if (log_) log_(why);
        last_log_ = why;
    }
    const auto legs = orders_.legs();
    if (std::any_of(legs.begin(), legs.end(), [](const auto& leg) { return !leg.cancel_pending; })) {
        orders_.withdraw(why);
    }
    target_fair_.reset();
}

bool QuotingMm::before_close(Clock::time_point now) {
    if (now >= ticker_->window_end) {
        abandon("the trading window has ended");
        return true;
    }
    if (now < ticker_->window_end - std::chrono::minutes(15)) {
        stand_aside("the trading window has not opened");
        return true;
    }
    if (now >= ticker_->window_end - params_.withdraw_before) {
        stand_aside("withdrawing before the window closes");
        return true;
    }
    return false;
}

void QuotingMm::on_reference(const cpm::binance::ReferenceView& reference, Clock::time_point now) {
    last_now_ = now;
    reference_ = reference;
    if (before_close(now)) return;
    if (!reference.usable() || reference.mid <= 0) {
        fair_.reset();
        z_.reset();
        previous_reference_.reset();
        previous_reference_time_ = {};
        stand_aside("waiting for a fresh underlying reference and tick size");
        return;
    }
    // PricingMidMoveThreshold: how far the reference venue's mid may move
    // in ONE update before the quotes standing behind it are stale. Two
    // tests, one threshold:
    //
    //   - this update against the last one, which is the move as it happens
    //   - the high-low range of any one second just passed, because the feed
    //     hands over only the LATEST touch at each wake, so a spike that
    //     reverts before the next one is invisible to the first test
    //
    // It is deliberately not the drift of the mid away from where a resting
    // order was priced. That is a different question - it asks whether a
    // quote has aged, not whether the market just jumped - and it is what
    // CryptoMM's parameter of the same name measures.
    const auto exceeds = [&](std::int64_t a, std::int64_t b) {
        // Division avoids overflowing ticks * tick_size for large mappings.
        return distance(a, b) / reference.tick_size >= params_.mid_move_threshold_ticks;
    };
    bool breach = false;
    if (params_.mid_move_threshold_ticks > 0) {
        breach = previous_reference_ && exceeds(reference.mid, *previous_reference_);
        if (!breach && reference.history) {
            const auto recent = previous_reference_time_ == Clock::time_point{}
                                    ? std::chrono::floor<std::chrono::seconds>(now) - std::chrono::seconds(1)
                                    : std::chrono::floor<std::chrono::seconds>(previous_reference_time_);
            for (auto it = reference.history->rbegin();
                 it != reference.history->rend() && it->second >= recent; ++it) {
                if (it->second > now) continue;
                if (exceeds(it->high, it->low)) breach = true;
            }
        }
    }
    previous_reference_ = reference.mid;
    previous_reference_time_ = now;
    if (breach) {
        held_ = true;
        hold_until_ = now + params_.mid_breach_resume;
        stand_aside("the pricing mid moved more than PricingMidMoveThreshold in one update: "
                    "waiting for a quiet reference and confirmed cancellations");
    }
}

void QuotingMm::on_market(const InstrumentView& book, const Position& position, Clock::time_point now) {
    ++passes_;
    ++passes_without_target_;
    last_now_ = now;
    // Inventory is checked even when pricing inputs or the book are unavailable.
    if (inventory_limit(position, params_)) {
        auto desired = quoting_mm_quotes(0.5, book, position, params_);
        reason_ = desired.reason;
        orders_.update(desired);
        return;
    }
    if (before_close(now)) return;
    if (!reference_.usable() || reference_.mid <= 0) {
        stand_aside("waiting for a fresh underlying reference and tick size");
        return;
    }
    if (held_) {
        if (now < hold_until_ || !orders_.quiescent()) {
            stand_aside("sharp underlying move: waiting for a quiet reference and confirmed cancellations");
            return;
        }
        held_ = false;
    }
    const double remaining = std::chrono::duration<double>(ticker_->window_end - now).count();
    // z first, then the probability that follows from it: the log line below
    // has to show the model's working, not just its answer. Fair alone
    // cannot say whether it moved because the underlying did or because the
    // window ran down, and a correction factor is fitted against z.
    z_ = quoting_z(reference_.mid, *opening_, params_.sigma, remaining, params_.correction);
    fair_ = z_ ? std::optional<double>(student_t4_cdf(*z_)) : std::nullopt;
    if (!fair_) {
        stand_aside("model inputs do not produce a valid probability");
        return;
    }
    if (orders_.quiescent()) target_fair_.reset();
    if (!target_fair_ || std::abs(*fair_ - *target_fair_) * kPriceOne >=
                             params_.fair_move_ticks * cpm::md::tick_at(static_cast<Price>(*target_fair_ * kPriceOne))) {
        target_fair_ = fair_;
    }
    const auto desired = quoting_mm_quotes(*target_fair_, book, position, params_);
    if (desired.bid || desired.ask) --passes_without_target_;
    reason_ = desired.reason;
    orders_.update(desired);
    // Everything the model used, so a finished run can be re-priced from its
    // own log: the strike, where the underlying was against it, how much of
    // the window was left, and the volatility that was frozen at the open.
    // ln(S/K) is written out beside z because it is the quantity the model
    // is defined on, and z is it divided by correction * sigma * sqrt(tau).
    const double travelled =
        std::log(static_cast<double>(reference_.mid) / static_cast<double>(*opening_));
    const auto message =
        "fair " + decimal(*fair_, 6) + ", z " + decimal(*z_, 6) + ", ln(S/K) " +
        decimal(travelled, 8) + ", spot " + format_scaled(reference_.mid, cpm::binance::kScale) +
        ", open " + format_scaled(*opening_, cpm::binance::kScale) + ", left " +
        decimal(remaining, 1) + "s, sigma " + decimal(params_.sigma, 8) + " -> " +
        (desired.bid ? format_price(desired.bid->price) : "-") + " / " +
        (desired.ask ? format_price(desired.ask->price) : "-");
    if (message != last_log_) {
        if (log_) log_(message);
        last_log_ = message;
    }
}

void QuotingMm::on_report(const ExecutionReport& report) { orders_.on_report(report); }
void QuotingMm::withdraw(const std::string& why) { stand_aside(why); }
void QuotingMm::abandon(const std::string& why) {
    orders_.abandon(why);
    reason_ = why;
    target_fair_.reset();
}

StrategyStatus QuotingMm::status() const {
    auto out = orders_.status();
    out.passes = passes_;
    out.passes_without_target = passes_without_target_;
    out.reason = reason_;
    out.diagnostics["Fair YES (%)"] = fair_ ? decimal(*fair_ * 100, 3) : "unavailable";
    out.diagnostics["Sigma / sqrt(second)"] = decimal(params_.sigma, 8);
    out.diagnostics["Sigma source"] =
        params_.warmup_note.empty() ? "supplied" : params_.warmup_note;
    // The opening price is not here any more: it comes from Binance like
    // everything else the model prices with, and it is written on every
    // strategy.log line instead of being a number to read off a row.
    out.diagnostics["Underlying price"] = reference_.usable() && reference_.mid > 0
                                              ? format_scaled(reference_.mid, cpm::binance::kScale) : "unavailable";
    out.diagnostics["Correction factor"] = decimal(params_.correction, 6);
    out.diagnostics["Seconds remaining"] = std::to_string(std::max<std::int64_t>(0,
        std::chrono::duration_cast<std::chrono::seconds>(ticker_->window_end - last_now_).count()));
    out.diagnostics["Model state"] = held_ ? "held after sharp move" : (reason_.empty() && fair_ ? "ready" : "waiting");
    return out;
}

ParamSchema quoting_mm_schema() {
    ParamSchema schema;
    const auto add = [&](const std::string& name, ParamSpec::Type type, const std::string& value,
                         const std::string& minimum, const std::string& maximum, const std::string& help,
                         int scale = 0) {
        ParamSpec p;
        p.name = name; p.type = type; p.default_value = value;
        p.min = minimum; p.max = maximum; p.help = help; p.scale = scale;
        schema.push_back(std::move(p));
    };
    using Type = ParamSpec::Type;
    add("symbol", Type::Text, "", "", "", "dated 15-minute instrument or series");
    add("width-ticks", Type::Int, "4", "1", "500", "minimum whole spread around model fair value, in prediction-market ticks");
    add("qty", Type::Decimal, "1", "0.01", "10000", "contracts per side, capped by remaining inventory capacity", cpm::kQtyScale);
    add("max-inventory", Type::Decimal, "5", "0", "1000000", "stop at this absolute net position; 0 disables", cpm::kQtyScale);
    add("vol-lookback-minutes", Type::Int, "20", "2", "1440", "minutes of one-minute Binance klines, ending at the window's opening, that sigma is taken over; read once when the instance is built");
    add("correction-factor", Type::Decimal, "1", "0.000001", "1000", "calibrated multiplier of sigma; 1 prices straight off the measured volatility, which is a starting point and not a calibration. Kept fixed until explicitly edited", 6);
    add("fair-value-move-threshold", Type::Int, "1", "1", "500", "reprice once fair value moves this many prediction ticks from the last target");
    add("pricing-mid-move-threshold", Type::Int, "2500", "0", "1000000000",
        "pull the quotes when the reference venue's mid moves this many of its own ticks in a "
        "single update, or over any one second; 0 is off. BTCUSDT on the USD-M futures venue "
        "ticks at 0.1, so 2500 is a $250 move");
    schema.back().label = "PricingMidMoveThreshold";
    add("pricing-mid-breach-resume-sec", Type::Int, "1", "0", "900",
        "seconds to stand aside after such a move; the previous cancels must also be confirmed "
        "before quoting resumes");
    schema.back().label = "PricingMidBreachResumeSec";
    add("withdraw-before", Type::Int, "25", "0", "899", "seconds before close to withdraw; default 25");
    // No opening-price here any more. The strike comes from the venue this
    // model prices against, like the spot and the volatility do: the open of
    // the window's own Binance kline, fetched when the instance is built. A
    // typed-in opening was a way to correct a strike the daemon could not
    // read, and there is nothing left for it to correct.
    return schema;
}

QuotingMmParams quoting_mm_params(const ParamValues& values) {
    QuotingMmParams p;
    p.symbol = param_text(values, "symbol");
    p.spread_ticks = static_cast<int>(param_int(values, "width-ticks"));
    p.quantity = param_decimal(values, "qty", cpm::kQtyScale);
    p.max_inventory = param_decimal(values, "max-inventory", cpm::kQtyScale);
    p.vol_lookback_minutes = static_cast<int>(param_int(values, "vol-lookback-minutes"));
    p.correction = static_cast<double>(param_decimal(values, "correction-factor", 6)) / 1000000;
    p.fair_move_ticks = static_cast<int>(param_int(values, "fair-value-move-threshold"));
    p.mid_move_threshold_ticks = param_int(values, "pricing-mid-move-threshold");
    p.mid_breach_resume = std::chrono::seconds(param_int(values, "pricing-mid-breach-resume-sec"));
    p.withdraw_before = std::chrono::seconds(param_int(values, "withdraw-before"));
    return p;
}

std::string reference_symbol_for(const std::map<std::string, cpm::binance::Mapping>& symbols,
                                 const std::string& instrument) {
    // The same two keys the daemon maps a reference price with: the ticker's
    // asset (BTC), and whatever precedes the first dash (BTCD15M), which is
    // what makes an instrument with no readable asset mappable at all.
    std::vector<std::string> keys;
    if (const auto parsed = cpm::md::parse_ticker(instrument)) keys.push_back(parsed->asset);
    if (const auto dash = instrument.find('-'); dash != std::string::npos) {
        keys.push_back(instrument.substr(0, dash));
    }
    for (const auto& key : keys) {
        const auto found = symbols.find(key);
        if (found != symbols.end()) return found->second.symbol;
    }
    return {};
}

void register_quoting_mm(StrategyRegistry& registry, QuotingMmVenue venue) {
    if (!venue.cache) venue.cache = std::make_shared<WarmupCache>();
    if (!venue.fetch) venue.fetch = cpm::https_get;
    registry.add("QuotingMM", "quotes Student t4 model fair value, including on an empty prediction book",
                 quoting_mm_schema(), [venue](const ParamValues& values, StrategyHooks hooks) {
                     auto params = quoting_mm_params(values);
                     const auto ticker = cpm::md::parse_ticker(params.symbol);
                     // Let the constructor refuse an instrument this kind
                     // does not quote: it says so better, and there is no
                     // point asking Binance about a window that is not one.
                     if (ticker && ticker->series.ends_with("15M")) {
                         const auto symbol = reference_symbol_for(venue.symbols, params.symbol);
                         if (symbol.empty()) {
                             throw std::invalid_argument(
                                 "no Binance symbol is mapped for " + params.symbol +
                                 ": add it to config/binance.conf before quoting this model");
                         }
                         WarmupRequest request;
                         request.symbol = symbol;
                         // The window this instrument is: the one that opened
                         // fifteen minutes before it closes, whether that was
                         // a moment ago or twelve minutes ago.
                         request.window_start = ticker->window_end - std::chrono::minutes(15);
                         request.lookback_minutes = params.vol_lookback_minutes;
                         request.host = venue.host;
                         request.port = venue.port;
                         request.ca_file = venue.ca_file;
                         request.timeout = venue.timeout;
                         const auto warmed = venue.cache->get(request, venue.fetch);
                         if (!warmed.ok()) {
                             // Refused rather than quoted on a guess. On a
                             // create the operator sees this; on a roll the
                             // host keeps the old instance and the daemon's
                             // next tick tries again (host.cpp, trader.cpp).
                             throw std::runtime_error("cannot price " + params.symbol + " yet: " +
                                                      warmed.error);
                         }
                         params.sigma = warmed.warmup.sigma;
                         params.warmup_opening = warmed.warmup.opening;
                         params.warmup_note = warmed.warmup.describe();
                     }
                     return std::make_unique<QuotingMm>(std::move(params), std::move(hooks));
                 });
}

}  // namespace cpm::strategy
