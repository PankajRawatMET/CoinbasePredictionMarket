#pragma once

#include "cpm/md/ticker.hpp"
#include "cpm/strategy/quote_reconciler.hpp"
#include "cpm/strategy/quoting_model.hpp"
#include "cpm/strategy/registry.hpp"

namespace cpm::strategy {

struct QuotingMmParams {
    std::string symbol;
    int spread_ticks = 4;
    Qty quantity = 100;
    Qty max_inventory = 500;
    cpm::md::TickStructure ticks = cpm::md::TickStructure::TaperedDeciCent;
    int vol_lookback_seconds = 1200;
    double correction = 0;  // required calibration, never silently guessed
    TScale t_scale = TScale::Raw;
    int fair_move_ticks = 1;
    std::int64_t sharp_move_ticks = 2500;  // reference venue's ticks
    std::chrono::milliseconds resume_quiet{1000};
    std::chrono::seconds withdraw_before{25};
    // 0 captures the mapped reference's first observation in the opening
    // second. A supplied official opening applies to this window only.
    cpm::binance::Price opening_price = 0;
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
    QuotingMmParams params_;
    QuoteReconciler orders_;
    std::optional<cpm::md::Ticker> ticker_;
    cpm::binance::ReferenceView reference_;
    std::optional<cpm::binance::Price> opening_;
    std::optional<double> sigma_;
    std::optional<double> fair_;
    std::optional<double> target_fair_;
    std::optional<cpm::binance::Price> previous_reference_;
    Clock::time_point previous_reference_time_{};
    // Each leg retains its own pricing reference. Replenishing a filled bid
    // must not reset the guard baseline of an older ask that is still resting.
    std::map<std::string, std::pair<cpm::binance::Price, Clock::time_point>> reference_anchors_;
    Clock::time_point hold_until_{};
    Clock::time_point last_now_{};
    bool held_ = false;
    std::size_t passes_ = 0;
    std::size_t passes_without_target_ = 0;
    std::string reason_;
    std::string last_log_;
    std::function<void(const std::string&)> log_;
};

ParamSchema quoting_mm_schema();
QuotingMmParams quoting_mm_params(const ParamValues& values);
void register_quoting_mm(StrategyRegistry& registry);

}  // namespace cpm::strategy
