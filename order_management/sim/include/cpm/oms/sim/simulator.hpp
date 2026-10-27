#pragma once

#include "cpm/common/config.hpp"
#include "cpm/common/logging.hpp"
#include "cpm/oms/sim/engine.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cpm::oms::sim {

// A few instruments to trade against, with a resting book on both sides.
std::vector<Instrument> default_instruments();

struct SimOptions {
    Credentials credentials;  // the one client allowed to log on (its API key is the TargetCompID's peer)
    std::uint16_t port = 6111;
    std::string dict_dir;
    std::string target_comp_id{kOeTargetCompId};
    std::vector<Instrument> instruments = default_instruments();
    Fees fees;

    // The firm identity the gateway stamps on every report's Parties group,
    // in the shape observed on the sandbox (OE-O2).
    std::string clearing_firm = "MET";
    std::string executing_firm = "MTP";
    std::string trader = "Met Traders 1";

    // Orders placed on the book before the first client connects, so there is
    // something to trade against: price, quantity and side per entry.
    bool seed_book = true;

    int clock_window_secs = 30;  // Logon SendingTime tolerance
};

// oe_sim: a QuickFIX acceptor playing the CPM order entry gateway (plain TCP,
// loopback use). Accepts a signed Logon, validates and matches orders, and
// answers with the layouts observed on the sandbox where they are known and the
// spec's where they are not. RAII: the destructor stops it.
class Simulator {
public:
    Simulator(SimOptions options, Logger& logger);
    ~Simulator();

    Simulator(const Simulator&) = delete;
    Simulator& operator=(const Simulator&) = delete;

    void start();
    void stop();

    bool wait_logged_on(std::chrono::milliseconds timeout) const;
    int logons() const;
    // Resting orders, for tests that want to see what the book holds.
    std::vector<RestingOrder> book(const std::string& symbol, Side side) const;

    // Pushes a MarketSettlementReport(UMS), as the gateway does when an
    // instrument settles and the session opted in with 20127=Y. The payout is
    // computed from the identity the report must satisfy.
    void settle(const std::string& symbol, SettlementReport::Outcome outcome, Price price,
                Qty long_quantity, Qty short_quantity);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cpm::oms::sim
