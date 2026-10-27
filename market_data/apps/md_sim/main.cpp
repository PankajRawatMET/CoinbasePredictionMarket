// md_sim: local stand-in for the CPM FIX market data gateway (see docs/design.md §12).
// Accepts one client, identified by the same environment variables md_client uses:
//   source config/sim.env && ./md_sim [--port N] [--rate N] [--roll-secs N] ...
#include "cpm/common/config.hpp"
#include "cpm/common/logging.hpp"
#include "cpm/md/sim/simulator.hpp"

#include <csignal>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <string>
#include <string_view>

namespace {

struct Args {
    cpm::md::sim::SimOptions sim;
    int duration_secs = 0;  // 0 = until signal
    bool verbose = false;
};

void usage() {
    std::cerr << "usage: md_sim [--port N] [--seed N] [--rate EVENTS_PER_SEC] [--roll-secs N]\n"
                 "              [--drop-seq N] [--disconnect-after N] [--stale-after SECS]\n"
                 "              [--duration SECS] [--dict-dir DIR] [--verbose]\n"
                 "env: CPM_API_KEY CPM_PASSPHRASE CPM_SECRET (the client allowed to log on)\n";
}

int to_int(std::string_view flag, const char* value, long lo, long hi) {
    char* end = nullptr;
    const long v = std::strtol(value, &end, 10);
    if (*value == '\0' || *end != '\0' || v < lo || v > hi) {
        throw std::invalid_argument(std::string(flag) + " needs an integer in " + std::to_string(lo) + ".." +
                                    std::to_string(hi));
    }
    return static_cast<int>(v);
}

Args parse_args(int argc, char** argv) {
    Args a;
    a.sim.dict_dir = CPM_DEFAULT_DICT_DIR;
    for (int i = 1; i < argc; ++i) {
        const std::string_view flag = argv[i];
        auto value = [&]() -> const char* {
            if (i + 1 >= argc) throw std::invalid_argument(std::string(flag) + " needs a value");
            return argv[++i];
        };
        if (flag == "--port") a.sim.port = static_cast<std::uint16_t>(to_int(flag, value(), 1, 65535));
        else if (flag == "--seed") a.sim.seed = static_cast<std::uint64_t>(to_int(flag, value(), 0, 2147483647));
        else if (flag == "--rate") a.sim.events_per_sec = to_int(flag, value(), 1, 10000);
        else if (flag == "--roll-secs") a.sim.roll_secs = to_int(flag, value(), 0, 86400);
        else if (flag == "--drop-seq") a.sim.drop_seq = to_int(flag, value(), 2, 1000000000);
        else if (flag == "--disconnect-after") a.sim.disconnect_after = to_int(flag, value(), 1, 1000000000);
        else if (flag == "--stale-after") a.sim.stale_after_secs = to_int(flag, value(), 1, 86400);
        else if (flag == "--duration") a.duration_secs = to_int(flag, value(), 0, 86400);
        else if (flag == "--dict-dir") a.sim.dict_dir = value();
        else if (flag == "--verbose") a.verbose = true;
        else if (flag == "--help" || flag == "-h") { usage(); std::exit(0); }
        else throw std::invalid_argument("unknown option " + std::string(flag));
    }
    return a;
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    try {
        args = parse_args(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "md_sim: " << e.what() << '\n';
        usage();
        return 64;
    }

    // Block SIGINT/SIGTERM before any thread starts; collect them synchronously.
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);

    cpm::Logger logger(std::cout, args.verbose ? cpm::LogLevel::Debug : cpm::LogLevel::Info);
    try {
        args.sim.credentials = cpm::load_credentials(cpm::process_env());
        // Play the same comp id the client will target.
        if (auto id = cpm::process_env()("CPM_FIX_TARGET_COMP_ID"); id && !id->empty()) {
            cpm::validate_comp_id("CPM_FIX_TARGET_COMP_ID", *id);
            args.sim.target_comp_id = *id;
        }
        cpm::md::sim::Simulator sim(args.sim, logger);
        sim.start();

        int elapsed = 0;
        while (args.duration_secs == 0 || elapsed < args.duration_secs) {
            const timespec one_second{1, 0};
            const int sig = sigtimedwait(&signals, nullptr, &one_second);
            if (sig == SIGINT || sig == SIGTERM) {
                logger.info(std::string("received ") + (sig == SIGINT ? "SIGINT" : "SIGTERM") + ", stopping");
                break;
            }
            ++elapsed;
        }
        sim.stop();
        logger.info("sim stopped");
        return 0;
    } catch (const cpm::ConfigError& e) {
        logger.error(std::string("configuration: ") + e.what());
        return 78;
    } catch (const std::exception& e) {
        logger.error(std::string("fatal: ") + e.what());
        return 1;
    }
}
