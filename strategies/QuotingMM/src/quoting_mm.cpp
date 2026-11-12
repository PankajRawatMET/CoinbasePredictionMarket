#include "cpm/strategy/quoting_mm.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

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
    const Price tick = cpm::md::tick_at(static_cast<Price>(center), p.ticks);
    const double half = static_cast<double>(tick * p.spread_ticks) / 2.0;
    // Round outwards: the configured spread is a minimum before touch/bounds
    // adjustments, and neither side improves through the model's fair value.
    Price bid = cpm::md::snap_down(static_cast<Price>(std::floor(center - half)), p.ticks);
    Price ask = cpm::md::snap_up(static_cast<Price>(std::ceil(center + half)), p.ticks);
    const Price minimum = cpm::md::price_above(0, p.ticks);
    const Price maximum = cpm::md::price_below(kPriceOne, p.ticks);
    bid = std::clamp(bid, minimum, maximum);
    ask = std::clamp(ask, minimum, maximum);
    if (!book.no.empty() && bid >= kPriceOne - book.no.front().price) {
        bid = cpm::md::price_below(kPriceOne - book.no.front().price, p.ticks);
    }
    if (!book.yes.empty() && ask <= book.yes.front().price) {
        ask = cpm::md::price_above(book.yes.front().price, p.ticks);
    }
    if (bid < minimum || ask > maximum || bid >= ask) {
        out.reason = "no room for model quotes on the price grid";
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
      orders_({params_.symbol, params_.ticks, 1}, hooks),
      ticker_(cpm::md::parse_ticker(params_.symbol)),
      log_(std::move(hooks.log)) {
    last_now_ = Clock::now();
    if (!ticker_ || !ticker_->series.ends_with("15M")) {
        throw std::invalid_argument("QuotingMM requires a dated 15-minute instrument");
    }
    if (!std::isfinite(params_.correction) || params_.correction <= 0) {
        throw std::invalid_argument("QuotingMM needs a positive calibrated correction-factor");
    }
    if (params_.opening_price > 0) opening_ = params_.opening_price;
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
        sigma_.reset();
        volatility_returns_ = 0;
        previous_reference_.reset();
        previous_reference_time_ = {};
        stand_aside("waiting for a fresh underlying reference and tick size");
        return;
    }
    // Compare both to the price behind live quotes and to observed ranges.
    // The latter catches a jump and reversal hidden by latest-price delivery.
    const auto exceeds = [&](std::int64_t a, std::int64_t b) {
        // Division avoids overflowing ticks * tick_size for large mappings.
        return distance(a, b) / reference.tick_size >= params_.sharp_move_ticks;
    };
    bool breach = false;
    if (params_.sharp_move_ticks > 0) {
        breach = previous_reference_ && exceeds(reference.mid, *previous_reference_);
        const auto legs = orders_.legs();
        if (!held_) {
            for (const auto& leg : legs) {
                const auto anchor = reference_anchors_.find(leg.cl_ord_id);
                if (anchor != reference_anchors_.end() && exceeds(reference.mid, anchor->second.first)) breach = true;
            }
        }
        if (reference.history) {
            const auto recent = previous_reference_time_ == Clock::time_point{}
                                    ? std::chrono::floor<std::chrono::seconds>(now) - std::chrono::seconds(1)
                                    : std::chrono::floor<std::chrono::seconds>(previous_reference_time_);
            for (auto it = reference.history->rbegin(); it != reference.history->rend() && it->second >= recent; ++it) {
                if (it->second > now) continue;
                if (exceeds(it->high, it->low)) breach = true;
                if (!held_) {
                    for (const auto& leg : legs) {
                        const auto anchor = reference_anchors_.find(leg.cl_ord_id);
                        if (anchor != reference_anchors_.end() &&
                            it->second >= std::chrono::floor<std::chrono::seconds>(anchor->second.second) &&
                            (exceeds(it->high, anchor->second.first) || exceeds(it->low, anchor->second.first))) breach = true;
                    }
                }
            }
        }
    }
    previous_reference_ = reference.mid;
    previous_reference_time_ = now;
    if (breach) {
        held_ = true;
        hold_until_ = now + params_.resume_quiet;
        stand_aside("sharp underlying move: waiting for a quiet reference and confirmed cancellations");
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
        reference_anchors_.clear();
    }
    if (!opening_ && reference_.history) {
        opening_ = reference_opening(*reference_.history, ticker_->window_end - std::chrono::minutes(15));
    }
    if (!opening_) {
        stand_aside("opening price unavailable: supply this window's opening-price or wait for a captured window");
        return;
    }
    volatility_returns_ = reference_.history
                              ? contiguous_reference_returns(*reference_.history, params_.vol_lookback_seconds, now)
                              : 0;
    sigma_ = reference_.history
                 ? reference_volatility(*reference_.history, params_.vol_lookback_seconds, now)
                 : std::nullopt;
    if (!sigma_) {
        fair_.reset();
        stand_aside("warming volatility: need " + std::to_string(params_.vol_lookback_seconds) +
                    " contiguous completed one-second returns with nonzero variance");
        return;
    }
    const double remaining = std::chrono::duration<double>(ticker_->window_end - now).count();
    fair_ = quoting_probability(reference_.mid, *opening_, *sigma_, remaining, params_.correction, params_.t_scale);
    if (!fair_) {
        stand_aside("model inputs do not produce a valid probability");
        return;
    }
    if (orders_.quiescent()) target_fair_.reset();
    if (!target_fair_ || std::abs(*fair_ - *target_fair_) * kPriceOne >=
                             params_.fair_move_ticks * cpm::md::tick_at(
                                 static_cast<Price>(*target_fair_ * kPriceOne), params_.ticks)) {
        target_fair_ = fair_;
    }
    const auto desired = quoting_mm_quotes(*target_fair_, book, position, params_);
    if (desired.bid || desired.ask) --passes_without_target_;
    reason_ = desired.reason;
    orders_.update(desired);
    std::map<std::string, std::pair<cpm::binance::Price, Clock::time_point>> next_anchors;
    for (const auto& leg : orders_.legs()) {
        const auto anchor = reference_anchors_.find(leg.cl_ord_id);
        next_anchors[leg.cl_ord_id] = anchor == reference_anchors_.end()
                                          ? std::make_pair(reference_.mid, now) : anchor->second;
    }
    reference_anchors_ = std::move(next_anchors);
    const auto message = "fair " + decimal(*fair_, 6) + ", sigma " + decimal(*sigma_, 8) +
                         " -> " + (desired.bid ? format_price(desired.bid->price) : "-") + " / " +
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
    reference_anchors_.clear();
}

StrategyStatus QuotingMm::status() const {
    auto out = orders_.status();
    out.passes = passes_;
    out.passes_without_target = passes_without_target_;
    out.reason = reason_;
    out.diagnostics["Fair YES (%)"] = fair_ ? decimal(*fair_ * 100, 3) : "unavailable";
    out.diagnostics["Sigma / sqrt(second)"] = sigma_ ? decimal(*sigma_, 8) : "warming";
    out.diagnostics["Volatility returns"] = std::to_string(volatility_returns_) + " / " +
                                          std::to_string(params_.vol_lookback_seconds);
    out.diagnostics["Warmup remaining (minimum)"] =
        volatility_returns_ < params_.vol_lookback_seconds
            ? std::to_string(params_.vol_lookback_seconds - volatility_returns_) + " seconds of uninterrupted observations"
            : sigma_ ? "ready" : "waiting for nonzero variance";
    out.diagnostics["Opening price"] = opening_ ? format_scaled(*opening_, cpm::binance::kScale) : "unavailable";
    out.diagnostics["Opening source"] = params_.opening_price > 0 ? "window override" : "mapped reference opening second";
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
    add("vol-lookback-seconds", Type::Int, "1200", "2", "3600", "volatility history length: 1200 is 20 minutes; 60 gives a one-minute simulator warmup but changes the estimate");
    add("correction-factor", Type::Decimal, "", "0.000001", "1000", "required calibrated multiplier of sigma; kept fixed until explicitly edited", 6);
    add("t-scale", Type::Enum, "raw", "", "", "raw matches the PDF formula; unit-variance rescales Student t4 to variance one");
    schema.back().choices = {"raw", "unit-variance"};
    add("fair-value-move-threshold", Type::Int, "1", "1", "500", "reprice once fair value moves this many prediction ticks from the last target");
    add("sharp-move-threshold", Type::Int, "2500", "0", "1000000000", "withdraw on this many reference ticks of movement or a one-second range; 0 disables");
    add("resume-quiet-ms", Type::Int, "1000", "1", "60000", "quiet time after the last detected sharp move; previous cancels must also be confirmed");
    add("withdraw-before", Type::Int, "25", "0", "899", "seconds before close to withdraw; default 25");
    add("opening-price", Type::Decimal, "0", "0", "1000000000", "this window's underlying opening/strike; 0 uses the captured reference opening second; reset on series roll", cpm::binance::kScale);
    schema.back().reset_on_roll = true;
    add("ticks", Type::Enum, "tapered", "", "", "prediction-market price grid");
    schema.back().choices = {"tapered", "tapered-cent", "cent", "deci"};
    return schema;
}

QuotingMmParams quoting_mm_params(const ParamValues& values) {
    QuotingMmParams p;
    p.symbol = param_text(values, "symbol");
    p.spread_ticks = static_cast<int>(param_int(values, "width-ticks"));
    p.quantity = param_decimal(values, "qty", cpm::kQtyScale);
    p.max_inventory = param_decimal(values, "max-inventory", cpm::kQtyScale);
    p.vol_lookback_seconds = static_cast<int>(param_int(values, "vol-lookback-seconds"));
    p.correction = static_cast<double>(param_decimal(values, "correction-factor", 6)) / 1000000;
    p.t_scale = param_text(values, "t-scale") == "raw" ? TScale::Raw : TScale::UnitVariance;
    p.fair_move_ticks = static_cast<int>(param_int(values, "fair-value-move-threshold"));
    p.sharp_move_ticks = param_int(values, "sharp-move-threshold");
    p.resume_quiet = std::chrono::milliseconds(param_int(values, "resume-quiet-ms"));
    p.withdraw_before = std::chrono::seconds(param_int(values, "withdraw-before"));
    p.opening_price = param_decimal(values, "opening-price", cpm::binance::kScale);
    p.ticks = cpm::md::tick_structure_from(param_text(values, "ticks")).value();
    return p;
}

void register_quoting_mm(StrategyRegistry& registry) {
    registry.add("QuotingMM", "quotes Student t4 model fair value, including on an empty prediction book",
                 quoting_mm_schema(), [](const ParamValues& values, StrategyHooks hooks) {
                     return std::make_unique<QuotingMm>(quoting_mm_params(values), std::move(hooks));
                 });
}

}  // namespace cpm::strategy
