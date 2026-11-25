#pragma once

#include "cpm/md/ticks.hpp"
#include "cpm/strategy/strategy.hpp"

namespace cpm::strategy {

struct Quote {
    Price price = 0;
    Qty quantity = 0;
};

struct DesiredQuotes {
    std::optional<Quote> bid;
    std::optional<Quote> ask;
    bool stop = false;
    std::string reason;
    bool rounded_up = false;
    std::string round_reason;
};

struct QuoteSettings {
    std::string symbol;
    int requote_threshold_ticks = 1;  // 0 replaces on any grid-price change
};

// Order lifecycle only. Pricing belongs to the calling strategy. Extracted
// from CryptoMM so every quoting strategy shares cancel/replace and report rules.
class QuoteReconciler {
public:
    using Hooks = StrategyHooks;
    QuoteReconciler(QuoteSettings settings, Hooks hooks);
    void update(const DesiredQuotes& desired);
    void on_report(const ExecutionReport& report);
    void withdraw(const std::string& why);
    void abandon(const std::string& why);
    bool quiescent() const { return !bid_ && !ask_; }
    std::optional<Quote> live_bid() const;
    std::optional<Quote> live_ask() const;
    std::vector<LiveLeg> legs() const;
    std::size_t quotes_placed() const { return placed_; }
    std::size_t quotes_cancelled() const { return cancelled_; }
    std::size_t fills() const { return fills_; }
    std::size_t passes() const { return passes_; }
    std::size_t passes_without_quotes() const { return passes_without_quotes_; }
    const std::string& last_reason() const { return last_reason_; }
    StrategyStatus status() const;

protected:
    struct LiveQuote {
        std::string cl_ord_id;
        Price requested = 0;
        Price price = 0;
        Qty quantity = 0;
        bool cancel_pending = false;
        bool acknowledged = false;
        bool slid = false;
    };
    void log_quote_availability(bool quoting_bid, bool quoting_ask, const std::string& reason);
    bool worth_replacing(const LiveQuote& live, const Quote& desired) const;
    void reconcile_side(cpm::oms::Side side, const std::optional<Quote>& desired,
                        std::optional<LiveQuote>& live, const std::string& symbol);
    QuoteSettings settings_;
    Hooks hooks_;
    std::optional<LiveQuote> bid_;
    std::optional<LiveQuote> ask_;
    std::size_t placed_ = 0;
    std::size_t cancelled_ = 0;
    std::size_t fills_ = 0;
    std::size_t passes_ = 0;
    std::size_t passes_without_quotes_ = 0;
    std::string last_reason_;

private:
    bool bid_paused_ = false;
    bool ask_paused_ = false;
};

}  // namespace cpm::strategy
