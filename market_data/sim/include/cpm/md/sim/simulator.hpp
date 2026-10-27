#pragma once

#include "cpm/common/config.hpp"
#include "cpm/common/logging.hpp"
#include "cpm/md/sim/market.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cpm::md::sim {

// A few long-lived instruments with books a few levels deep on both sides.
std::vector<InstrumentSpec> default_instruments();

struct SimOptions {
    Credentials credentials;  // the one client allowed to log on (its API key is the TargetCompID)
    std::uint16_t port = 6121;
    std::string dict_dir;
    // The comp id the simulator plays, i.e. the client's TargetCompID.
    std::string target_comp_id{kMdTargetCompId};
    std::uint64_t seed = 1;
    double events_per_sec = 5.0;
    std::vector<InstrumentSpec> instruments = default_instruments();

    // Rolling instrument (like the sandbox's 5/15-minute series [OBS O6]):
    // every roll_secs it closes, is determined and replaced by the next window,
    // and is settled settle_delay later. 0 disables it.
    int roll_secs = 0;
    std::chrono::milliseconds settle_delay{1000};

    // Logon: SendingTime must be within this many seconds of the simulator's
    // clock (the spec's Order Entry ±30 s rule, design §3).
    int clock_window_secs = 30;

    // Faults. Each fires once per simulator run, so recovery can be observed.
    //  drop_seq N:         the first market data message whose MsgSeqNum is >= N
    //                      is not sent, leaving a sequence gap. ResendRequests are
    //                      answered with a gap fill (nothing is persisted).
    //  disconnect_after N: after the Nth market data message of a session, drop
    //                      the TCP connection without a Logout.
    //  stale_after N:      N seconds after logon, every active subscription goes
    //                      silent; heartbeats continue. Subscriptions made after
    //                      that receive data normally.
    std::optional<int> drop_seq;
    std::optional<int> disconnect_after;
    std::optional<int> stale_after_secs;
};

// md_sim: a QuickFIX acceptor playing the CPM MD gateway (plain TCP, loopback
// use). Accepts a signed Logon, validates MarketDataRequests (spec §4.3), and
// sends snapshots then incrementals in the layouts observed on the sandbox.
// RAII: the destructor stops it.
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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cpm::md::sim
