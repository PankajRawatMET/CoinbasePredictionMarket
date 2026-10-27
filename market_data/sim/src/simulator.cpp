#include "cpm/md/sim/simulator.hpp"

#include "cpm/fix_common/raw_log.hpp"
#include "cpm/fix_common/session_settings.hpp"
#include "fix/sim_application.hpp"

#include <quickfix/MessageStore.h>
#include <quickfix/ThreadedSocketAcceptor.h>

#include <thread>

namespace cpm::md::sim {

std::vector<InstrumentSpec> default_instruments() {
    // Long-lived game-style instruments (like the sandbox's NFLGAME series [OBS O6]),
    // one event with two outcomes plus a second event. SIM prefix: never real tickers.
    return {
        {"SIMGAME-26NOV01HOMAWY-HOM", "700000000000000001", 550, 6},
        {"SIMGAME-26NOV01HOMAWY-AWY", "700000000000000002", 450, 6},
        {"SIMGAME-26NOV02REDBLU-RED", "700000000000000003", 300, 4},
    };
}

struct Simulator::Impl {
    Impl(SimOptions opts, Logger& log)
        : options(std::move(opts)),
          logger(log),
          settings(cpm::fix::make_acceptor_settings(options.credentials.api_key, options.port, options.dict_dir,
                                                    options.target_comp_id)),
          app(options, logger, Clock::now()),
          raw_log(logger, options.credentials.api_key) {}

    SimOptions options;
    Logger& logger;
    FIX::SessionSettings settings;
    fix::SimApplication app;
    FIX::MemoryStoreFactory store;
    cpm::fix::RawLogFactory raw_log;
    std::unique_ptr<FIX::ThreadedSocketAcceptor> acceptor;
    std::jthread events;
};

Simulator::Simulator(SimOptions options, Logger& logger)
    : impl_(std::make_unique<Impl>(std::move(options), logger)) {}

Simulator::~Simulator() { stop(); }

void Simulator::start() {
    if (impl_->acceptor) return;
    impl_->acceptor = std::make_unique<FIX::ThreadedSocketAcceptor>(impl_->app, impl_->store, impl_->settings,
                                                                    impl_->raw_log);
    impl_->acceptor->start();
    impl_->logger.info("sim: listening on port " + std::to_string(impl_->options.port) +
                       " (plain TCP, all interfaces: QuickFIX's acceptor has no bind-address option)");
    impl_->events = std::jthread([this](std::stop_token stop) {
        while (!stop.stop_requested()) {
            impl_->app.tick(Clock::now());
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
}

void Simulator::stop() {
    if (!impl_->acceptor) return;
    impl_->events.request_stop();
    if (impl_->events.joinable()) impl_->events.join();
    impl_->acceptor->stop();
    impl_->acceptor.reset();
}

bool Simulator::wait_logged_on(std::chrono::milliseconds timeout) const { return impl_->app.wait_logged_on(timeout); }

int Simulator::logons() const { return impl_->app.logons(); }

}  // namespace cpm::md::sim
