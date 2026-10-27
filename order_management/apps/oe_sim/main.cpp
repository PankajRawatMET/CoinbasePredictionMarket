// oe_sim: a local stand-in for the CPM order entry gateway. Accepts a signed
// Logon, validates and matches orders, and answers with the layouts observed on
// the sandbox where they are known (OE-O2) and the spec's where they are not.
//
//   terminal 1:  source config/oe-sim.env && ./build/order_management/oe_sim
//   terminal 2:  source config/oe-sim.env && ./build/order_management/oe_client \
//                  --allow-orders --test-order SIMOE-26NOV01HOMAWY-HOM --price 0.5
#include "cpm/common/config.hpp"
#include "cpm/common/logging.hpp"
#include "cpm/oms/sim/simulator.hpp"

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

struct Args {
    cpm::oms::sim::SimOptions sim;
    bool verbose = false;
    bool replaced_instruments = false;
};

void usage() {
    std::cerr << "usage: oe_sim [--port N] [--dict-dir DIR] [--taker-bps N] [--maker-bps N]\n"
                 "              [--instrument SYMBOL]... [--no-seed-book] [--verbose]\n"
                 "env: CPM_OE_API_KEY CPM_OE_PASSPHRASE CPM_OE_SECRET\n"
                 "     [CPM_OE_PORT] [CPM_OE_TARGET_COMP_ID=CPMOE]\n";
}

int to_int(std::string_view flag, const char* value) {
    char* end = nullptr;
    const long v = std::strtol(value, &end, 10);
    if (*value == '\0' || *end != '\0') {
        throw std::invalid_argument(std::string(flag) + " needs an integer");
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
        if (flag == "--port") a.sim.port = static_cast<std::uint16_t>(to_int(flag, value()));
        else if (flag == "--dict-dir") a.sim.dict_dir = value();
        else if (flag == "--taker-bps") a.sim.fees.taker_bps = to_int(flag, value());
        else if (flag == "--maker-bps") a.sim.fees.maker_bps = to_int(flag, value());
        else if (flag == "--no-seed-book") a.sim.seed_book = false;
        else if (flag == "--instrument") {
            // Replaces the defaults, so the simulator can serve the same
            // symbols as md_sim when a strategy spans both services.
            if (!a.replaced_instruments) {
                a.sim.instruments.clear();
                a.replaced_instruments = true;
            }
            a.sim.instruments.push_back({value(), true, std::chrono::milliseconds{0}});
        }
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
        std::cerr << "oe_sim: " << e.what() << '\n';
        usage();
        return 64;
    }

    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);

    cpm::Logger logger(std::cout, args.verbose ? cpm::LogLevel::Debug : cpm::LogLevel::Info);
    try {
        const auto env = cpm::process_env();
        args.sim.credentials = cpm::load_credentials(env, "CPM_OE_");
        if (auto port = env("CPM_OE_PORT"); port && !port->empty() && args.sim.port == 6111) {
            args.sim.port = static_cast<std::uint16_t>(std::stoi(*port));
        }
        if (auto id = env("CPM_OE_TARGET_COMP_ID"); id && !id->empty()) {
            cpm::validate_comp_id("CPM_OE_TARGET_COMP_ID", *id);
            args.sim.target_comp_id = *id;
        }

        cpm::oms::sim::Simulator sim(std::move(args.sim), logger);
        sim.start();
        int sig = 0;
        while (sig != SIGINT && sig != SIGTERM) sig = sigwaitinfo(&signals, nullptr);
        logger.info("oe_sim: stopping");
        sim.stop();
        return 0;
    } catch (const std::exception& e) {
        logger.error(std::string("fatal: ") + e.what());
        return 1;
    }
}
