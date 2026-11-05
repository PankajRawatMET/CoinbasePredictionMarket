#include "cpm/oms/sim/simulator.hpp"

#include "cpm/fix_common/raw_log.hpp"
#include "cpm/fix_common/session_settings.hpp"
#include "fix/sim_application.hpp"

#include <quickfix/MessageStore.h>
#include <quickfix/ThreadedSocketAcceptor.h>

#include <atomic>
#include <thread>

namespace cpm::oms::sim {

std::vector<Instrument> default_instruments() {
    // SIM prefix: never a real ticker. One instrument carries a speed bump, so
    // the PendingNew path can be exercised.
    return {
        {"SIMOE-26NOV01HOMAWY-HOM", true, std::chrono::milliseconds{0}},
        {"SIMOE-26NOV01HOMAWY-AWY", true, std::chrono::milliseconds{0}},
        {"SIMOE-BUMP-26NOV01-YES", true, std::chrono::milliseconds{300}},
    };
}

struct Simulator::Impl {
    Impl(SimOptions opts, Logger& log)
        : options(std::move(opts)),
          logger(log),
          settings(cpm::fix::make_oe_acceptor_settings(options.credentials.api_key, options.port,
                                                       options.dict_dir, options.target_comp_id)),
          app(options, logger),
          raw_log(logger, options.credentials.api_key) {}

    SimOptions options;
    Logger& logger;
    FIX::SessionSettings settings;
    fix::SimApplication app;
    FIX::MemoryStoreFactory store;
    cpm::fix::RawLogFactory raw_log;
    std::unique_ptr<FIX::ThreadedSocketAcceptor> acceptor;
    // Releases speed-bumped orders; the only thing the simulator does on a timer.
    std::atomic<bool> running{false};
    std::thread timer;
};

Simulator::Simulator(SimOptions options, Logger& logger)
    : impl_(std::make_unique<Impl>(std::move(options), logger)) {}

Simulator::~Simulator() { stop(); }

void Simulator::start() {
    if (impl_->acceptor) return;
    impl_->acceptor = std::make_unique<FIX::ThreadedSocketAcceptor>(impl_->app, impl_->store,
                                                                    impl_->settings, impl_->raw_log);
    impl_->acceptor->start();
    impl_->running.store(true);
    impl_->timer = std::thread([this] {
        while (impl_->running.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (!impl_->running.load()) return;
            impl_->app.tick(Clock::now());
        }
    });
    impl_->logger.info("oe_sim: listening on port " + std::to_string(impl_->options.port) +
                       " as " + impl_->options.target_comp_id);
}

void Simulator::stop() {
    if (impl_->running.exchange(false) && impl_->timer.joinable()) impl_->timer.join();
    if (!impl_->acceptor) return;
    impl_->acceptor->stop();
    impl_->acceptor.reset();
}

bool Simulator::wait_logged_on(std::chrono::milliseconds timeout) const {
    return impl_->app.wait_logged_on(timeout);
}

int Simulator::logons() const { return impl_->app.logons(); }

std::vector<RestingOrder> Simulator::book(const std::string& symbol, Side side) const {
    return impl_->app.book(symbol, side);
}

void Simulator::settle(const std::string& symbol, SettlementReport::Outcome outcome, Price price,
                       Qty long_quantity, Qty short_quantity) {
    impl_->app.settle(symbol, outcome, price, long_quantity, short_quantity);
}

Qty Simulator::trade(const std::string& symbol, Side side, Qty quantity, Price price) {
    return impl_->app.trade(symbol, side, quantity, price);
}

void Simulator::rest(const std::string& symbol, Side side, Qty quantity, Price price) {
    impl_->app.rest(symbol, side, quantity, price);
}

}  // namespace cpm::oms::sim
