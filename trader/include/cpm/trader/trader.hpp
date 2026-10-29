#pragma once

#include "cpm/binance/config.hpp"
#include "cpm/md/feed_monitor.hpp"
#include "cpm/md/market_state.hpp"
#include "cpm/md/instruments.hpp"
#include "cpm/md/series_watcher.hpp"
#include "cpm/md/ticks.hpp"
#include "cpm/md/md_client.hpp"
#include "cpm/oms/oe_client.hpp"
#include "cpm/oms/order_state.hpp"
#include "cpm/oms/reconciler.hpp"
#include "cpm/trader/control_api.hpp"

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace cpm::trader {

struct TraderOptions {
    cpm::MdConfig md;
    cpm::OeConfig oe;
    std::string dict_dir;
    // The one durable file, as OE-Q7 allows: the daemon's, not any
    // strategy's (GUI-Q5).
    std::string bookmark{"cpm-trader-bookmark.txt"};
    // The daemon-wide key. Nothing can be armed without it.
    bool allow_orders = false;
    // Shared by every strategy, because they belong to the session (GUI-Q10).
    cpm::oms::OrderLimits limits;
    int book_depth = 10;  // 1 or 10, per the spec
    // The price grid every instrument here is assumed to trade on. One
    // setting for the daemon, because the structure is not on the FIX feed
    // and there is nowhere else to learn it from (O5).
    cpm::md::TickStructure ticks = cpm::md::TickStructure::TaperedDeciCent;
    // The REST instrument catalog. Optional: without it a series cannot be
    // resolved, and everything that names a full ticker still works.
    cpm::md::RestConfig rest;
    std::chrono::milliseconds stale_after{std::chrono::seconds(30)};
    // A daemon meant to stay up for days must drop the instruments it will
    // never see again (GUI-N3): the sandbox lists ~100 rolling windows a day.
    std::chrono::seconds evict_finished_after{std::chrono::minutes(15)};
    std::chrono::seconds evict_every{std::chrono::seconds(60)};
    // The reference venue, for the pricing speed bump
    // (docs/design-binance.md). No symbols means no feed and no guard: the
    // daemon is then exactly what it was before B3.
    cpm::binance::ReferenceFeed::Options reference;
    // Whether to hold the reference connection open at all. False is a
    // daemon with no Binance in it, which is every test that does not
    // name one and every run that does not want the speed bump.
    bool reference_enabled = false;
    std::map<std::string, cpm::binance::Mapping> reference_symbols;
    // Which Binance symbol a CPM series - or asset - is priced against,
    // and that symbol's tick size (B-Q7, B-Q17). Both are read off the
    // instrument's own ticker, so nothing is typed per strategy. Empty by
    // default: the mappings live in config/binance.conf, not here.
};

// The daemon: one market data session, one order entry session, one of every
// piece of state the venue counts, and a host running N strategies over them.
//
// It owns exactly one of each because the gateway allows one session per API
// key and displaces the older one without firing cancel-on-disconnect, which
// would orphan a running strategy's quotes (GUI-Q1, spec OE:197-201).
//
// Threading: everything the strategies see happens on the thread that calls
// tick(). The sessions' own callbacks only update state or queue work.
class Trader {
public:
    Trader(TraderOptions options, const cpm::strategy::StrategyRegistry& registry, Logger& logger);
    ~Trader();

    Trader(const Trader&) = delete;
    Trader& operator=(const Trader&) = delete;

    // Logs both sessions on and reconciles. Returns an empty string on
    // success, or why it could not start. A market data failure is fatal; an
    // order entry failure is fatal only with --allow-orders, so a read-only
    // run still works when order entry is down (as crypto_mm's dry run did).
    std::string start(std::chrono::milliseconds timeout = std::chrono::seconds(20));

    // Anything that happens between passes and cannot wait for the next
    // one writes a byte here. Today that is the reference feed: a Binance
    // tick must reach the guard now, not at the end of a 250 ms sleep
    // (B-Q5). -1 when there is no feed.
    int wake_fd() const;
    // Empties the pipe. The loop calls it after poll() says there is
    // something, because the byte is a flag and not a queue: a burst of
    // updates is one pass at the newest price.
    void drain_wake();

    // One pass: applies a pending halt, drives the strategies, and evicts
    // finished instruments on its timer. Call it about four times a second.
    void tick(Clock::time_point now = Clock::now());

    // Clean shutdown: stop every strategy, wait for its cancels, save the
    // bookmark, log out. Idempotent.
    void stop();

    ControlApi& api() { return *api_; }
    const ControlApi& api() const { return *api_; }
    bool halted() const { return host_.halted(); }

private:
    // Both halt paths funnel through here. The reconciler's handler can run on
    // the session's own callback thread, where stopping a client would
    // deadlock, so nothing is stopped here: the reason is recorded and tick()
    // acts on it.
    void request_halt(std::string reason);
    void apply_halt(Clock::time_point now);
    std::string subscribe(const std::string& symbol);
    // Moves every strategy that follows a series onto the window the
    // lifecycle feed says is open. Called from tick(), on the tick thread,
    // because that is where strategies may be touched.
    void roll_series(Clock::time_point now);
    void unsubscribe(const std::string& symbol);
    BookView view_of(const std::string& symbol) const;
    // The reference venue's view of a CPM symbol: its asset, mapped. An
    // asset with no mapping has no reference, which a guard that is on
    // reads as "do not quote" and one that is off ignores.
    cpm::binance::ReferenceView reference_for(const std::string& symbol) const;
    // Which Binance symbol prices this CPM one, or empty when nothing does.
    std::string reference_symbol_for(const std::string& symbol) const;
    // Tells the reference feed which streams the running strategies need.
    // Called every pass, from tick(), on the tick thread.
    void follow_reference_demand();

    std::unique_ptr<cpm::binance::ReferenceFeed> reference_;
    // A self-pipe, so the feed's thread can wake the loop without sharing
    // anything else with it.
    int wake_read_ = -1;
    int wake_write_ = -1;

    const TraderOptions options_;
    Logger& logger_;

    cpm::md::MarketState book_;
    // Which window of each series is trading, from the all-instrument
    // lifecycle subscription this daemon opens at startup.
    cpm::md::SeriesWatcher series_;
    std::string series_md_req_id_;
    cpm::md::MdClient md_;
    std::unique_ptr<cpm::md::FeedMonitor> feed_;

    cpm::oms::OrderState orders_;
    cpm::oms::OeClient oe_;
    std::unique_ptr<cpm::oms::Reconciler> reconciler_;

    cpm::strategy::StrategyHost host_;
    std::unique_ptr<ControlApi> api_;

    std::atomic<std::uint64_t> cl_ord_counter_{0};

    // The halt, in two phases: withdraw everything, then once the cancels are
    // accounted for, stop the sessions. The process itself stays up so the
    // reason can still be read (GUI-Q4).
    mutable std::mutex mutex_;
    std::string pending_halt_;
    bool halt_requested_ = false;
    bool halt_applied_ = false;
    bool sessions_stopped_ = false;
    Clock::time_point halt_applied_at_{};

    // symbol -> the MDReqIDs opened for it (book, then lifecycle).
    std::map<std::string, std::vector<std::string>> subscriptions_;
    Clock::time_point last_evict_{};
    bool stopped_ = false;
};

}  // namespace cpm::trader
