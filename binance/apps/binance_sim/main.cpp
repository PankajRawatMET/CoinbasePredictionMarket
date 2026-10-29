// binance_sim: the reference feed's other end, on loopback.
//
// It exists for the same reason md_sim and oe_sim do. Every test of the
// speed bump needs a price that moves exactly when the test says so, and the
// sandbox cannot provide one: CPM's sandbox BTCD15M is simulated and does
// not track real BTC, so pointing the real Binance feed at it would make the
// guard fire constantly and prove nothing (B-Q6).
//
//   binance_sim --port 9443 --symbol BTCUSDT --mid 67000 --tick 0.01
//               [--interval-ms 100] [--drift-ticks 1] [--jump 2500@50]
//               [--stall-after N] [--drop-after N] [--verbose]
#include "cpm/binance/reference.hpp"
#include "cpm/common/json.hpp"
#include "cpm/common/logging.hpp"
#include "cpm/common/fixed.hpp"
#include "cpm/common/websocket.hpp"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

std::atomic<bool> stopping{false};

void on_signal(int) { stopping.store(true); }

struct Args {
    int port = 0;
    std::string symbol = "BTCUSDT";
    std::int64_t mid = 6700000000000;  // 67,000 at 1e-8
    std::int64_t tick = 1000000;       // 0.01 at 1e-8
    int interval_ms = 100;
    std::int64_t drift_ticks = 1;
    std::int64_t jump_ticks = 0;
    int jump_at = 0;
    int stall_after = 0;
    int drop_after = 0;
    bool verbose = false;
};

void usage() {
    std::cerr << "usage: binance_sim [--port N] [--symbol SYM] [--mid PRICE] [--tick SIZE]\n"
                 "                   [--interval-ms MS] [--drift-ticks N] [--jump TICKS@AT]\n"
                 "                   [--stall-after N] [--drop-after N] [--verbose]\n"
                 "Serves <symbol>@bookTicker over a plain WebSocket on loopback.\n"
                 "--jump 2500@50 moves the mid 2500 ticks at the 50th message, which is\n"
                 "what a test of the speed bump needs and a real feed will not do on cue.\n";
}

std::int64_t to_scaled(const char* text, const char* flag) {
    const auto value = cpm::parse_scaled(text, cpm::binance::kScale);
    if (!value) throw std::invalid_argument(std::string(flag) + " needs a decimal");
    return *value;
}

int to_int(const char* text, const char* flag) {
    char* end = nullptr;
    const long parsed = std::strtol(text, &end, 10);
    if (*text == '\0' || *end != '\0') throw std::invalid_argument(std::string(flag) + " needs an integer");
    return static_cast<int>(parsed);
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string flag = argv[i];
            auto value = [&]() -> const char* {
                if (i + 1 >= argc) throw std::invalid_argument(flag + " needs a value");
                return argv[++i];
            };
            if (flag == "--port") args.port = to_int(value(), "--port");
            else if (flag == "--symbol") args.symbol = value();
            else if (flag == "--mid") args.mid = to_scaled(value(), "--mid");
            else if (flag == "--tick") args.tick = to_scaled(value(), "--tick");
            else if (flag == "--interval-ms") args.interval_ms = to_int(value(), "--interval-ms");
            else if (flag == "--drift-ticks") args.drift_ticks = to_int(value(), "--drift-ticks");
            else if (flag == "--stall-after") args.stall_after = to_int(value(), "--stall-after");
            else if (flag == "--drop-after") args.drop_after = to_int(value(), "--drop-after");
            else if (flag == "--verbose") args.verbose = true;
            else if (flag == "--jump") {
                const std::string spec = value();
                const auto at = spec.find('@');
                if (at == std::string::npos) throw std::invalid_argument("--jump needs TICKS@AT");
                args.jump_ticks = std::strtoll(spec.substr(0, at).c_str(), nullptr, 10);
                args.jump_at = to_int(spec.substr(at + 1).c_str(), "--jump");
            } else {
                throw std::invalid_argument("unknown flag " + flag);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "binance_sim: " << e.what() << '\n';
        usage();
        return 64;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    cpm::Logger logger(std::cout, args.verbose ? cpm::LogLevel::Debug : cpm::LogLevel::Info);
    cpm::ws::Server server;
    if (const auto failed = server.start(args.port); !failed.empty()) {
        logger.error("binance_sim: " + failed);
        return 73;
    }
    logger.info("binance_sim listening on 127.0.0.1:" + std::to_string(server.port()) + " as " +
                args.symbol + ", mid " + cpm::format_scaled(args.mid, cpm::binance::kScale) +
                ", tick " + cpm::format_scaled(args.tick, cpm::binance::kScale));

    std::int64_t mid = args.mid;
    std::int64_t update_id = 1;
    int sent = 0;
    auto last_ping = std::chrono::steady_clock::now();
    // Nothing is sent until it is asked for, as the venue behaves: a
    // connection with no SUBSCRIBE on it is a connection that says nothing.
    bool subscribed = false;
    const std::string stream = [&args] {
        std::string lower = args.symbol;
        for (char& c : lower) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        return lower + "@bookTicker";
    }();

    // SUBSCRIBE and UNSUBSCRIBE, answered the way the venue answers them:
    // {"result":null,"id":N}.
    const auto on_control = [&](std::string_view text) {
        const auto parsed = cpm::json_parse(text);
        if (!parsed.ok() || !parsed.value.is_object()) return;
        const auto* method = parsed.value.find("method");
        const auto* id = parsed.value.find("id");
        if (method == nullptr) return;
        const bool wants = method->text == "SUBSCRIBE";
        const bool drops = method->text == "UNSUBSCRIBE";
        if (!wants && !drops) return;

        bool mentions_us = false;
        if (const auto* params = parsed.value.find("params"); params != nullptr) {
            for (const auto& item : params->items) {
                if (item.text == stream) mentions_us = true;
            }
        }
        if (mentions_us) {
            subscribed = wants;
            logger.info(std::string("binance_sim: ") + (wants ? "SUBSCRIBE " : "UNSUBSCRIBE ") +
                        stream);
        }
        server.send_text(R"({"result":null,"id":)" +
                         (id != nullptr ? id->text : std::string("0")) + "}");
    };

    while (!stopping.load()) {
        std::string why;
        if (!server.attached() && !server.accept(500ms, why)) continue;
        logger.info("binance_sim: a client attached");
        sent = 0;
        subscribed = false;

        while (!stopping.load() && server.attached()) {
            if (!subscribed) {
                // Connected and listening, with nothing asked for yet.
                server.pump(100ms, on_control);
                continue;
            }
            if (args.stall_after > 0 && sent >= args.stall_after) {
                // Still connected, saying nothing: what a feed looks like
                // when it has quietly died, which must read as stale.
                server.pump(200ms, on_control);
                continue;
            }
            if (args.drop_after > 0 && sent >= args.drop_after) {
                logger.info("binance_sim: dropping the connection after " + std::to_string(sent) +
                            " messages");
                server.drop();
                break;
            }

            ++sent;
            if (args.jump_ticks != 0 && sent == args.jump_at) {
                mid += args.jump_ticks * args.tick;
                logger.info("binance_sim: jumping " + std::to_string(args.jump_ticks) +
                            " ticks to " + cpm::format_scaled(mid, cpm::binance::kScale));
            } else {
                // A walk that goes nowhere, so a test that is not about a
                // jump sees a feed that is alive and uninteresting.
                mid += ((sent % 2 == 0) ? 1 : -1) * args.drift_ticks * args.tick;
            }

            const auto half = args.tick / 2 > 0 ? args.tick / 2 : 1;
            std::string message = R"({"u":)" + std::to_string(update_id++) + R"(,"s":")" +
                                  args.symbol + R"(","b":")" +
                                  cpm::format_scaled(mid - half, cpm::binance::kScale) +
                                  R"(","B":"1.00000000","a":")" +
                                  cpm::format_scaled(mid + half, cpm::binance::kScale) +
                                  R"(","A":"1.00000000"})";
            if (const auto failed = server.send_text(message); !failed.empty()) {
                logger.warn("binance_sim: " + failed);
                server.drop();
                break;
            }

            const auto now = std::chrono::steady_clock::now();
            if (now - last_ping >= 20s) {
                last_ping = now;
                server.send_ping("keepalive");  // as Binance does, every 20 s
            }
            server.pump(std::chrono::milliseconds(args.interval_ms), on_control);
        }
        logger.info("binance_sim: the client went away");
    }
    server.stop();
    return 0;
}
