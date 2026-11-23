#include "cpm/strategy/quoting_mm.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <thread>
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

double prediction_z(double probability) {
    double low = -20, high = 20;
    for (int i = 0; i < 44; ++i) {
        const double mid = (low + high) / 2;
        if (student_t4_cdf(mid) < probability) low = mid;
        else high = mid;
    }
    return (low + high) / 2;
}

std::optional<Price> other_touch(const std::vector<cpm::md::BookLevel>& levels,
                                 const std::vector<LiveLeg>& own, cpm::oms::Side side) {
    for (const auto& level : levels) {
        const auto price = side == cpm::oms::Side::Buy ? level.price : kPriceOne - level.price;
        auto quantity = level.qty;
        for (const auto& order : own) {
            if (order.side == side && order.price == price) quantity -= order.quantity;
        }
        if (quantity > 0) return price;
    }
    return std::nullopt;
}

std::optional<double> boundary_mid(const cpm::binance::HistoryView& history,
                                   PricingSecond second, std::chrono::duration<double> max_age) {
    auto it = std::lower_bound(history.begin(), history.end(), second,
        [](const auto& observation, auto at) { return observation.second < at; });
    cpm::binance::Price price = 0;
    if (it != history.end() && it->second == second) price = it->boundary_mid;
    if (price <= 0 && it != history.begin()) {
        --it;
        if (it->close_at != Clock::time_point{} && second - it->close_at <= max_age)
            price = it->close;
    }
    if (price <= 0) return std::nullopt;
    return static_cast<double>(price) / 100000000.0;
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

void PricingCalibration::expire(PricingSecond now) {
    while (!corrections.empty() && corrections.front().first <= now - std::chrono::hours(2)) {
        ordered.erase(ordered.find(corrections.front().second));
        corrections.pop_front();
    }
    if (ordered.size() < 200) {
        correction = 0.95;
    } else {
        auto middle = std::next(ordered.begin(), ordered.size() / 2);
        correction = ordered.size() % 2 == 0 ? (*middle + *std::prev(middle)) / 2 : *middle;
    }
}

void PricingCalibration::update(PricingSecond now, double x, double scale, double market_mid) {
    if (now <= last_second) return;
    last_second = now;
    const double z = prediction_z(market_mid);
    if (std::abs(z) > 0.3 && x * z > 0) {
        const double sample = std::clamp(x / (scale * z), 0.5, 3.0);
        corrections.emplace_back(now, sample);
        ordered.insert(sample);
        expire(now);
    }
    const double implied = z * correction * scale - x;
    offset += 0.129 * (std::clamp(implied, -0.002, 0.002) - offset);
}

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
      orders_({params_.symbol, params_.history_fetch ? 0 : 1}, hooks),
      ticker_(cpm::md::parse_ticker(params_.symbol)),
      log_(std::move(hooks.log)),
      resting_orders_(std::move(hooks.resting_orders)) {
    last_now_ = Clock::now();
    if (!ticker_ || !ticker_->series.ends_with("15M")) {
        throw std::invalid_argument("QuotingMM requires a dated 15-minute instrument");
    }
    if (params_.history_fetch) return;
    if (!std::isfinite(params_.correction) || params_.correction <= 0) {
        throw std::invalid_argument("QuotingMM needs a positive calibrated correction-factor");
    }
    if (!std::isfinite(params_.sigma) || params_.sigma <= 0) {
        throw std::invalid_argument("QuotingMM needs a positive sigma from the window's warmup");
    }
    opening_ = params_.warmup_opening;
    if (!opening_ || *opening_ <= 0) {
        throw std::invalid_argument("QuotingMM needs this window's opening price");
    }
}

bool QuotingMm::update_history(PricingSecond now) {
    using namespace std::chrono;
    const auto final_start = floor<seconds>(ticker_->window_end) - minutes(1);
    if (pending_history_.valid() && pending_history_.wait_for(seconds(0)) == std::future_status::ready) {
        auto result = pending_history_.get();
        if (!result.error.empty()) {
            retry_after_ = steady_clock::now() + seconds(2);
            stand_aside("historical pricing data: " + result.error);
            return false;
        }
        history_ = std::move(result.state);
    }
    if (history_) {
        if (now < history_->last_second) {
            history_.reset();
            history_hold_ = true;
            stand_aside("trader clock moved behind pricing history; reloading history");
            return false;
        }
        while (history_->last_second < now) {
            const auto next = history_->last_second + seconds(1);
            const auto mid = reference_.history
                ? boundary_mid(*reference_.history, next, history_->max_age) : std::nullopt;
            if (!mid) {
                history_.reset();
                retry_after_ = steady_clock::now() + seconds(2);
                history_hold_ = true;
                stand_aside("missing Binance midpoint at a required second; reloading history");
                return false;
            }
            history_->add(next, *mid, final_start);
        }
        if (history_hold_ && !orders_.quiescent()) {
            stand_aside("waiting for pricing-data cancellations");
            return false;
        }
        if (reference_.age > history_->max_age) {
            history_hold_ = true;
            stand_aside("Binance midpoint is older than the historical sampling limit");
            return false;
        }
        history_hold_ = false;
        return true;
    }
    if (!pending_history_.valid() && steady_clock::now() >= retry_after_) {
        const auto window_open = floor<seconds>(ticker_->window_end) - minutes(15);
        const PricingSecond as_of = floor<minutes>(now);
        const auto& live = reference_.history;
        if (!live || live->empty() || live->front().second > as_of) {
            stand_aside("waiting for live Binance history to reach a published minute boundary");
            return false;
        }
        const auto previous = as_of - minutes(1);
        const bool can_fallback = previous >= window_open && live->front().second <= previous;
        auto task = std::packaged_task<HistoryResult()>(
            [request = params_.history_request, fetch = params_.history_fetch,
             symbol = params_.history_symbol, as_of, window_open, final_start, previous, can_fallback] {
                auto result = fetch_pricing_history(request, fetch, symbol, as_of, window_open, final_start);
                if (result.incomplete && can_fallback)
                    return fetch_pricing_history(request, fetch, symbol, previous, window_open, final_start);
                return result;
            });
        auto result = task.get_future();
        try {
            std::thread(std::move(task)).detach();
            pending_history_ = std::move(result);
        } catch (const std::system_error& e) {
            retry_after_ = steady_clock::now() + seconds(2);
            stand_aside(std::string("could not start historical pricing request: ") + e.what());
            return false;
        }
        stand_aside("loading historical pricing data");
    }
    return false;
}

void QuotingMm::calibrate(const InstrumentView& book, PricingSecond now, double remaining) {
    if (remaining <= 60 || !book.book_valid || book.crossed_or_locked ||
        params_.calibration->last_second == now) return;
    const auto own = resting_orders_ ? resting_orders_(params_.symbol) : orders_.legs();
    const auto bid = other_touch(book.yes, own, cpm::oms::Side::Buy);
    const auto ask = other_touch(book.no, own, cpm::oms::Side::Sell);
    if (!bid || !ask || cpm::md::price_above(*bid) != *ask) return;
    const double midpoint = (static_cast<double>(*bid) + *ask) / (2 * kPriceOne);
    if (midpoint < 0.10 || midpoint > 0.90) return;
    const double spot = static_cast<double>(reference_.mid) / 100000000.0;
    params_.calibration->update(now, std::log(spot / history_->strike),
                                params_.sigma * std::sqrt(remaining - 40), midpoint);
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
        if (params_.history_fetch) {
            history_hold_ = true;
            history_.reset();
        }
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
    const auto second = std::chrono::floor<std::chrono::seconds>(now);
    double strike = 0;
    const double spot = static_cast<double>(reference_.mid) / 100000000.0;
    if (params_.history_fetch) {
        if (!update_history(second)) {
            fair_.reset();
            z_.reset();
            return;
        }
        if (second != pricing_second_) {
            pricing_second_ = second;
            params_.calibration->expire(second);
            correction_ = params_.calibration->correction;
            offset_ = params_.calibration->offset;
        }
        strike = history_->strike;
        params_.sigma = history_->sigma();
        const auto elapsed = second - std::chrono::floor<std::chrono::seconds>(ticker_->window_end)
                             + std::chrono::seconds(60);
        z_ = history_->z(spot, remaining, static_cast<int>(elapsed.count()), correction_, offset_);
    } else {
        strike = static_cast<double>(*opening_) / 100000000.0;
        z_ = quoting_z(reference_.mid, *opening_, params_.sigma, remaining, params_.correction);
    }
    fair_ = z_ ? std::optional<double>(student_t4_cdf(*z_)) : std::nullopt;
    if (!fair_) {
        stand_aside("model inputs do not produce a valid probability");
        return;
    }
    if (orders_.quiescent()) target_fair_.reset();
    if (params_.history_fetch || !target_fair_ || std::abs(*fair_ - *target_fair_) * kPriceOne >=
                             params_.fair_move_ticks * cpm::md::tick_at(static_cast<Price>(*target_fair_ * kPriceOne))) {
        target_fair_ = fair_;
    }
    const auto desired = quoting_mm_quotes(*target_fair_, book, position, params_);
    if (desired.bid || desired.ask) --passes_without_target_;
    reason_ = desired.reason;
    orders_.update(desired);
    if (params_.history_fetch) calibrate(book, second, remaining);
    const double travelled = std::log(spot / strike);
    const auto message =
        "fair " + decimal(*fair_, 6) + ", z " + decimal(*z_, 6) + ", ln(S/K) " +
        decimal(travelled, 8) + ", spot " + format_scaled(reference_.mid, cpm::binance::kScale) +
        ", open " + decimal(strike, cpm::binance::kScale) + ", left " +
        decimal(remaining, 1) + "s, sigma " + decimal(params_.sigma, 8) +
        (params_.history_fetch ? ", c " + decimal(correction_, 6) + ", delta " + decimal(offset_, 8) +
            ", settlement samples " + std::to_string(history_->settlement_count) : "") + " -> " +
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
    if (history_) out.diagnostics["Strike proxy"] = decimal(history_->strike, cpm::binance::kScale);
    out.diagnostics["Underlying price"] = reference_.usable() && reference_.mid > 0
                                              ? format_scaled(reference_.mid, cpm::binance::kScale) : "unavailable";
    out.diagnostics["Correction factor"] = decimal(params_.history_fetch ? correction_ : params_.correction, 6);
    if (params_.history_fetch) out.diagnostics["Price offset"] = decimal(offset_, 8);
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
    add("width-ticks", Type::Int, "4", "1", "500", "spread in prediction-market grid steps");
    add("qty", Type::Decimal, "1", "0.01", "10000", "contracts per side, capped by remaining inventory capacity", cpm::kQtyScale);
    add("max-inventory", Type::Decimal, "5", "0", "1000000", "stop at this absolute net position; 0 disables", cpm::kQtyScale);
    add("vol-lookback-minutes", Type::Int, "20", "2", "1440", "legacy setting accepted for saved strategies, model uses 20 and 120 minutes");
    add("correction-factor", Type::Decimal, "1", "0.000001", "1000", "legacy setting accepted for saved strategies, model calibrates automatically", 6);
    add("fair-value-move-threshold", Type::Int, "1", "1", "500", "legacy setting accepted for saved strategies, model reprices on grid changes");
    add("pricing-mid-move-threshold", Type::Int, "2500", "0", "1000000000",
        "pull the quotes when the reference venue's mid moves this many of its own ticks in a "
        "single update, or over any one second; 0 is off");
    schema.back().label = "PricingMidMoveThreshold";
    add("pricing-mid-breach-resume-sec", Type::Int, "1", "0", "900",
        "seconds to stand aside after such a move; the previous cancels must also be confirmed "
        "before quoting resumes");
    schema.back().label = "PricingMidBreachResumeSec";
    add("withdraw-before", Type::Int, "25", "0", "899", "seconds before close to withdraw; default 25");
    return schema;
}

QuotingMmParams quoting_mm_params(const ParamValues& values) {
    QuotingMmParams p;
    p.symbol = param_text(values, "symbol");
    p.spread_ticks = static_cast<int>(param_int(values, "width-ticks"));
    p.quantity = param_decimal(values, "qty", cpm::kQtyScale);
    p.max_inventory = param_decimal(values, "max-inventory", cpm::kQtyScale);
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
    if (!venue.fetch) venue.fetch = cpm::https_get;
    using Calibrations = std::map<std::pair<std::string, std::string>, std::shared_ptr<PricingCalibration>>;
    auto calibrations = std::make_shared<Calibrations>();
    registry.add("QuotingMM", "quotes the midpoint-based 15-minute model",
                 quoting_mm_schema(), [venue, calibrations](const ParamValues& values, StrategyHooks hooks) {
                     auto params = quoting_mm_params(values);
                     const auto ticker = cpm::md::parse_ticker(params.symbol);
                     if (!ticker || !ticker->series.ends_with("15M"))
                         throw std::invalid_argument("QuotingMM requires a dated 15-minute instrument");
                     const auto symbol = reference_symbol_for(venue.symbols, params.symbol);
                     if (symbol.empty())
                         throw std::invalid_argument("no Binance symbol mapped for " + params.symbol +
                                                     " in config/binance.conf");
                     if (!venue.configuration_error.empty())
                         throw std::invalid_argument(venue.configuration_error);
                     if (venue.history.host.empty())
                         throw std::invalid_argument("set CPM_QUOTING_HISTORY_URL to the historical API");
                     auto& calibration = (*calibrations)[{ticker->series, symbol}];
                     if (!calibration) calibration = std::make_shared<PricingCalibration>();
                     params.calibration = calibration;
                     params.history_request = venue.history;
                     params.history_fetch = venue.fetch;
                     params.history_symbol = symbol;
                     params.warmup_note = "historical API and live midpoint samples";
                     return std::make_unique<QuotingMm>(std::move(params), std::move(hooks));
                 });
}

}  // namespace cpm::strategy
