#pragma once

#include "cpm/binance/config.hpp"
#include "cpm/md/ticker.hpp"
#include "cpm/strategy/quote_reconciler.hpp"
#include "cpm/strategy/quoting_model.hpp"
#include "cpm/strategy/quoting_warmup.hpp"
#include "cpm/strategy/registry.hpp"

#include <map>
#include <memory>

namespace cpm::strategy {

struct QuotingMmParams {
    std::string symbol;
    int spread_ticks = 4;
    Qty quantity = 100;
    Qty max_inventory = 500;
    // The volatility lookback, in minutes, ending at the window's opening.
    // Read once from REST klines when the instance is built; never
    // recomputed while the window runs.
    int vol_lookback_minutes = 20;
    // Sigma per sqrt(second), and the window's opening price, both from that
    // one warmup. The factory fills them in; an instance is never built
    // without a sigma, which is why there is no "warming" state any more.
    double sigma = 0;
    // The strike: the open of the window's own kline. There is no operator
    // override any more - the venue the model prices against supplies it.
    cpm::binance::Price warmup_opening = 0;
    std::string warmup_note;  // where they came from, for the operator's row
    double correction = 0;  // required calibration, never silently guessed
    int fair_move_ticks = 1;
    // PricingMidMoveThreshold, in the reference venue's own ticks: a move
    // of this much in one update - or inside one second - pulls the quotes.
    // 0 is off. BTCUSDT on the futures venue ticks at 0.1.
    std::int64_t mid_move_threshold_ticks = 2500;
    // PricingMidBreachResumeSec: how long to stand aside after such a move.
    // The cancels must also have come back before quoting resumes.
    std::chrono::seconds mid_breach_resume{1};
    std::chrono::seconds withdraw_before{25};
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
};

ParamSchema quoting_mm_schema();
QuotingMmParams quoting_mm_params(const ParamValues& values);

// What the factory needs to reach Binance: the same CPM-instrument-to-symbol
// mapping the reference feed is configured with (config/binance.conf), and
// where REST is.
//
// The fetch is injected so every test of the factory runs without a socket,
// and the cache is shared across instances so a roll that is retried on each
// daemon tick cannot turn into one blocking HTTPS call per tick.
struct QuotingMmVenue {
    std::map<std::string, cpm::binance::Mapping> symbols;
    std::string host = "fapi.binance.com";
    int port = 443;
    std::string ca_file;
    std::chrono::seconds timeout{15};
    HttpsFetch fetch;                    // empty: cpm::https_get
    std::shared_ptr<WarmupCache> cache;  // null: one of its own
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
