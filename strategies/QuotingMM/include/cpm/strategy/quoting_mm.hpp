#pragma once

#include "cpm/binance/config.hpp"
#include "cpm/md/ticker.hpp"
#include "cpm/strategy/quote_reconciler.hpp"
#include "cpm/strategy/quoting_model.hpp"
#include "cpm/strategy/quoting_history.hpp"
#include "cpm/strategy/registry.hpp"

#include <map>
#include <future>
#include <memory>
#include <set>

namespace cpm::strategy {

struct PricingCalibration {
    std::deque<std::pair<PricingSecond, double>> corrections;
    std::multiset<double> ordered;
    double correction = 0.95;
    double offset = 0;
    PricingSecond last_second{};
    void expire(PricingSecond now);
    void update(PricingSecond now, double x, double scale, double market_mid);
};

struct QuotingMmParams {
    std::string symbol;
    int spread_ticks = 4;
    Qty quantity = 100;
    Qty max_inventory = 500;
    // Explicit static inputs for direct callers. The registry uses live pricing.
    double sigma = 0;
    cpm::binance::Price warmup_opening = 0;
    std::string warmup_note;
    double correction = 0;
    int fair_move_ticks = 1;
    // PricingMidMoveThreshold, in the reference venue's own ticks: a move
    // of this much in one update - or inside one second - pulls the quotes.
    // 0 disables the guard.
    std::int64_t mid_move_threshold_ticks = 2500;
    // PricingMidBreachResumeSec: how long to stand aside after such a move.
    // The cancels must also have come back before quoting resumes.
    std::chrono::seconds mid_breach_resume{1};
    std::chrono::seconds withdraw_before{25};
    cpm::HttpsRequest history_request;
    HttpsFetch history_fetch;
    std::string history_symbol;
    std::shared_ptr<PricingCalibration> calibration;
};

DesiredQuotes quoting_mm_quotes(double fair, const InstrumentView& book,
                                 const Position& position, const QuotingMmParams& params);

class QuotingMm final : public IStrategy {
public:
    QuotingMm(QuotingMmParams params, StrategyHooks hooks);
    void on_market(const InstrumentView&, const Position&, Clock::time_point now) override;
    void on_reference(const cpm::binance::ReferenceView&, Clock::time_point now) override;
    void on_report(const ExecutionReport& report) override;
    void withdraw(const std::string& why) override;
    void abandon(const std::string& why) override;
    bool quiescent() const override { return orders_.quiescent(); }
    std::vector<std::string> symbols() const override { return {params_.symbol}; }
    bool requires_book_updates() const override { return false; }
    StrategyStatus status() const override;

private:
    bool before_close(Clock::time_point now);
    void stand_aside(const std::string& why);
    bool update_history(PricingSecond now);
    void calibrate(const InstrumentView& book, PricingSecond now, double remaining);
    QuotingMmParams params_;
    QuoteReconciler orders_;
    std::optional<cpm::md::Ticker> ticker_;
    cpm::binance::ReferenceView reference_;
    std::optional<cpm::binance::Price> opening_;
    std::optional<double> z_;
    std::optional<double> fair_;
    std::optional<double> target_fair_;
    std::optional<cpm::binance::Price> previous_reference_;
    Clock::time_point previous_reference_time_{};
    Clock::time_point hold_until_{};
    Clock::time_point last_now_{};
    bool held_ = false;
    std::size_t passes_ = 0;
    std::size_t passes_without_target_ = 0;
    std::string reason_;
    std::string last_log_;
    std::function<void(const std::string&)> log_;
    decltype(StrategyHooks::resting_orders) resting_orders_;
    std::optional<PricingHistory> history_;
    std::future<HistoryResult> pending_history_;
    std::chrono::steady_clock::time_point retry_after_{};
    bool history_hold_ = false;
    PricingSecond pricing_second_{};
    double correction_ = 0.95;
    double offset_ = 0;
};

ParamSchema quoting_mm_schema();
QuotingMmParams quoting_mm_params(const ParamValues& values);

// The reference feed's mappings and the historical API endpoint.
struct QuotingMmVenue {
    std::map<std::string, cpm::binance::Mapping> symbols;
    cpm::HttpsRequest history;
    HttpsFetch fetch;
    std::string configuration_error;
};

// The Binance symbol that prices a CPM instrument, by the rule the feed
// uses: the ticker's asset first, then whatever precedes the first dash.
// Empty when nothing maps it.
std::string reference_symbol_for(const std::map<std::string, cpm::binance::Mapping>& symbols,
                                 const std::string& instrument);

// A kind that cannot be built without the venue it prices against. The
// default leaves the mapping empty, which refuses every create with a
// reason - a build with no binance.conf has no business quoting this model.
void register_quoting_mm(StrategyRegistry& registry, QuotingMmVenue venue = {});

}  // namespace cpm::strategy
