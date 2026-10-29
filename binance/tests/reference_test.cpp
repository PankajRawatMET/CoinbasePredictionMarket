// The reference feed (B2): the parsing on its own, and the feed against a
// WebSocket server on loopback. No network, and no Binance.
#include "cpm/binance/reference.hpp"

#include "cpm/common/fixed.hpp"
#include "cpm/common/websocket.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <sstream>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;
using cpm::binance::BookTicker;
using cpm::binance::kScale;
using cpm::binance::parse_book_ticker;
using cpm::binance::Price;
using cpm::binance::ReferenceFeed;

Price at(const char* decimal) {
    const auto value = cpm::parse_scaled(decimal, kScale);
    EXPECT_TRUE(value.has_value()) << decimal;
    return value.value_or(0);
}

// ---- the payload ----------------------------------------------------------

TEST(BookTicker, ReadsTheStreamBinanceDocuments) {
    // The example from Binance's own documentation, read 2026-10-29.
    const auto tick = parse_book_ticker(
        R"({"u":400900217,"s":"BNBUSDT","b":"25.35190000","B":"31.21000000",)"
        R"("a":"25.36520000","A":"40.66000000"})");
    ASSERT_TRUE(tick.has_value());
    EXPECT_EQ(tick->symbol, "BNBUSDT");
    EXPECT_EQ(tick->bid, at("25.35190000"));
    EXPECT_EQ(tick->ask, at("25.36520000"));
    EXPECT_EQ(tick->mid(), at("25.35855"));
}

TEST(BookTicker, ReadsTheCombinedStreamEnvelopeToo) {
    // Several symbols on one connection arrive wrapped. The feed uses this
    // shape; the bare one is accepted so the path can change without this.
    const auto tick = parse_book_ticker(
        R"({"stream":"btcusdt@bookTicker","data":{"u":1,"s":"BTCUSDT",)"
        R"("b":"67000.00","B":"1","a":"67000.02","A":"1"}})");
    ASSERT_TRUE(tick.has_value());
    EXPECT_EQ(tick->symbol, "BTCUSDT");
    EXPECT_EQ(tick->mid(), at("67000.01"));
}

TEST(BookTicker, KeepsEveryDecimalBinanceSends) {
    // Eight decimals is Binance's limit and int64 at 1e-8 holds it with room
    // to spare: nothing here goes near a double (B-Q12).
    const auto tick = parse_book_ticker(
        R"({"s":"BTCUSDT","b":"67890.12345678","a":"67890.12345680"})");
    ASSERT_TRUE(tick.has_value());
    EXPECT_EQ(tick->bid, 6789012345678LL);
    EXPECT_EQ(tick->mid(), 6789012345679LL);
}

TEST(BookTicker, RefusesWhatItCannotTrust) {
    // Not JSON, no prices, a crossed touch, an empty side: all of them are
    // "no reference", which the guard reads as "do not quote".
    EXPECT_FALSE(parse_book_ticker("not json at all").has_value());
    EXPECT_FALSE(parse_book_ticker(R"({"s":"BTCUSDT"})").has_value());
    EXPECT_FALSE(parse_book_ticker(R"({"s":"X","b":"2.00","a":"1.00"})").has_value());
    EXPECT_FALSE(parse_book_ticker(R"({"s":"X","b":"0","a":"1.00"})").has_value());
    EXPECT_FALSE(parse_book_ticker(R"({"s":"X","b":"abc","a":"1.00"})").has_value());
}

// ---- the feed against a socket --------------------------------------------

class FeedTest : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_EQ(server.start(0), ""); }

    void TearDown() override {
        if (feed) feed->stop();
        server.stop();
    }

    // Starts the feed against the loopback server and waits for it to
    // attach. exchangeInfo is not reachable here, so the tick size is given.
    //
    // `pinned` is what --binance asks for: subscribed for the whole run
    // whatever the strategies do. The tests about subscribing and
    // unsubscribing pass none, because that is the daemon's normal state -
    // a stream exists only while a strategy wants it (B-Q14).
    void run(std::chrono::milliseconds stale_after = 5s,
             std::vector<std::string> pinned = {"BTCUSDT"}) {
        ReferenceFeed::Options options;
        options.host = "127.0.0.1";
        options.port = server.port();
        options.tls = false;
        options.symbols = std::move(pinned);
        options.stale_after = stale_after;
        options.reconnect_after = 50ms;
        options.tick_size["BTCUSDT"] = at("0.01");
        feed = std::make_unique<ReferenceFeed>(std::move(options), logger);
        ASSERT_EQ(feed->start(), "");

        std::string why;
        ASSERT_TRUE(server.accept(3s, why)) << why;
    }

    bool wait_until(const std::function<bool()>& done, std::chrono::milliseconds limit = 3s) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < deadline) {
            if (done()) return true;
            server.pump(20ms, [this](std::string_view text) { control.emplace_back(text); });
        }
        return done();
    }

    // Everything the client sent us: the SUBSCRIBE and UNSUBSCRIBE frames.
    std::vector<std::string> control;

    bool saw_control(const std::string& method, const std::string& stream) {
        return std::any_of(control.begin(), control.end(), [&](const std::string& message) {
            return message.find("\"" + method + "\"") != std::string::npos &&
                   message.find(stream) != std::string::npos;
        });
    }

    std::string tick_json(const char* bid, const char* ask) {
        return std::string(R"({"stream":"btcusdt@bookTicker","data":{"u":1,"s":"BTCUSDT","b":")") +
               bid + R"(","B":"1","a":")" + ask + R"(","A":"1"}})";
    }

    std::ostringstream log;
    cpm::Logger logger{log, cpm::LogLevel::Info};
    cpm::ws::Server server;
    std::unique_ptr<ReferenceFeed> feed;
};

TEST_F(FeedTest, HasNoPriceUntilOneArrives) {
    run();
    if (::testing::Test::HasFatalFailure()) return;
    // Connected is not the same as knowing a price, and the difference
    // matters: a strategy must not quote off a reference it has not got.
    const auto view = feed->latest("BTCUSDT", cpm::binance::Clock::now());
    EXPECT_FALSE(view.present);
    EXPECT_FALSE(view.usable());
    EXPECT_EQ(view.tick_size, at("0.01"));
}

TEST_F(FeedTest, KeepsTheLatestMid) {
    run();
    if (::testing::Test::HasFatalFailure()) return;

    ASSERT_EQ(server.send_text(tick_json("67000.00", "67000.02")), "");
    ASSERT_TRUE(wait_until([&] {
        return feed->latest("BTCUSDT", cpm::binance::Clock::now()).present;
    }));
    EXPECT_EQ(feed->latest("BTCUSDT", cpm::binance::Clock::now()).mid, at("67000.01"));

    // The latest, not the first: a reference that lagged would be worse
    // than none.
    ASSERT_EQ(server.send_text(tick_json("67100.00", "67100.02")), "");
    EXPECT_TRUE(wait_until([&] {
        return feed->latest("BTCUSDT", cpm::binance::Clock::now()).mid == at("67100.01");
    }));
}

TEST_F(FeedTest, WakesWhoeverIsWaitingOnEveryUpdate) {
    ReferenceFeed::Options options;
    options.host = "127.0.0.1";
    options.port = server.port();
    options.tls = false;
    options.symbols = {"BTCUSDT"};
    options.tick_size["BTCUSDT"] = at("0.01");
    feed = std::make_unique<ReferenceFeed>(std::move(options), logger);

    // The whole point of the waker: the daemon must not wait for its next
    // 250 ms pass to notice that the world moved (B-Q5).
    std::atomic<int> wakes{0};
    feed->on_update([&wakes] { wakes.fetch_add(1); });
    ASSERT_EQ(feed->start(), "");
    std::string why;
    ASSERT_TRUE(server.accept(3s, why)) << why;

    for (int i = 0; i < 3; ++i) {
        ASSERT_EQ(server.send_text(tick_json("67000.00", "67000.02")), "");
    }
    EXPECT_TRUE(wait_until([&] { return wakes.load() >= 3; }));
}

TEST_F(FeedTest, GoesStaleWhenTheFeedGoesQuiet) {
    run(200ms);
    if (::testing::Test::HasFatalFailure()) return;

    ASSERT_EQ(server.send_text(tick_json("67000.00", "67000.02")), "");
    ASSERT_TRUE(wait_until([&] {
        return feed->latest("BTCUSDT", cpm::binance::Clock::now()).usable();
    }));

    // Still connected, saying nothing. The price is still the last one, but
    // it is no longer to be relied on, and that is what a strategy asks.
    std::this_thread::sleep_for(400ms);
    const auto view = feed->latest("BTCUSDT", cpm::binance::Clock::now());
    EXPECT_TRUE(view.present);
    EXPECT_TRUE(view.stale);
    EXPECT_FALSE(view.usable());
    EXPECT_GE(view.age, 300ms);
}

TEST_F(FeedTest, ReconnectsAfterTheServerDropsIt) {
    run(5s);
    if (::testing::Test::HasFatalFailure()) return;

    ASSERT_EQ(server.send_text(tick_json("67000.00", "67000.02")), "");
    ASSERT_TRUE(wait_until([&] {
        return feed->latest("BTCUSDT", cpm::binance::Clock::now()).present;
    }));

    // Binance closes every connection at 24 hours, so this is not an
    // exception, it is Tuesday (B-Q13).
    server.drop();
    std::string why;
    ASSERT_TRUE(server.accept(5s, why)) << why;
    ASSERT_EQ(server.send_text(tick_json("68000.00", "68000.02")), "");
    EXPECT_TRUE(wait_until([&] {
        return feed->latest("BTCUSDT", cpm::binance::Clock::now()).mid == at("68000.01");
    }, 5s));
}

TEST_F(FeedTest, IsStaleRatherThanBrokenWhileNothingIsListening) {
    // Nothing on the port at all: the feed retries, the daemon stays up and
    // every strategy that needs a reference simply does not quote (B-Q4).
    ReferenceFeed::Options options;
    options.host = "127.0.0.1";
    options.port = 1;  // nothing listens here
    options.tls = false;
    options.symbols = {"BTCUSDT"};
    options.reconnect_after = 20ms;
    options.tick_size["BTCUSDT"] = at("0.01");
    feed = std::make_unique<ReferenceFeed>(std::move(options), logger);
    EXPECT_EQ(feed->start(), "") << "a feed that cannot connect yet is not a failure to start";
    std::this_thread::sleep_for(150ms);
    EXPECT_FALSE(feed->connected());
    EXPECT_FALSE(feed->latest("BTCUSDT", cpm::binance::Clock::now()).usable());
}

// --- one connection, streams on and off it (B-Q14) ------------------------

TEST_F(FeedTest, SubscribesNothingUntilSomethingIsWanted) {
    run(5s, {});
    if (::testing::Test::HasFatalFailure()) return;
    // Connected and carrying nothing: a daemon with no strategy running
    // should not be reading a price it has no use for.
    wait_until([] { return false; }, 300ms);
    EXPECT_TRUE(control.empty()) << "it asked for a stream nobody wanted";
    EXPECT_TRUE(feed->subscribed().empty());
}

TEST_F(FeedTest, SubscribesWhatIsWantedAndDropsWhatIsNot) {
    run(5s, {});
    if (::testing::Test::HasFatalFailure()) return;

    feed->want({"BTCUSDT"});
    ASSERT_TRUE(wait_until([&] { return saw_control("SUBSCRIBE", "btcusdt@bookTicker"); }));
    EXPECT_EQ(feed->subscribed(), std::vector<std::string>{"BTCUSDT"});

    // A price arrives and is kept, as it would be on the real stream.
    ASSERT_EQ(server.send_text(tick_json("67000.00", "67000.02")), "");
    ASSERT_TRUE(wait_until([&] {
        return feed->latest("BTCUSDT", cpm::binance::Clock::now()).present;
    }));

    // Nobody wants it any more. The stream goes, and so does its price: a
    // mid from a stream nobody is listening to is not stale, it is gone.
    feed->want({});
    ASSERT_TRUE(wait_until([&] { return saw_control("UNSUBSCRIBE", "btcusdt@bookTicker"); }));
    EXPECT_TRUE(feed->subscribed().empty());
    EXPECT_FALSE(feed->latest("BTCUSDT", cpm::binance::Clock::now()).present);
}

TEST_F(FeedTest, IgnoresAPriceForAStreamItHasDropped) {
    run(5s, {});
    if (::testing::Test::HasFatalFailure()) return;
    feed->want({"BTCUSDT"});
    ASSERT_TRUE(wait_until([&] { return !feed->subscribed().empty(); }));
    feed->want({});
    ASSERT_TRUE(wait_until([&] { return feed->subscribed().empty(); }));

    // An UNSUBSCRIBE and a message already on the wire cross all the time.
    // The message is not a reason to resurrect the stream.
    ASSERT_EQ(server.send_text(tick_json("99000.00", "99000.02")), "");
    wait_until([] { return false; }, 300ms);
    EXPECT_FALSE(feed->latest("BTCUSDT", cpm::binance::Clock::now()).present);
}

TEST_F(FeedTest, AsksForSeveralStreamsInOneMessage) {
    run(5s, {});
    if (::testing::Test::HasFatalFailure()) return;
    // The venue allows five incoming messages a second and counts our
    // pongs among them, so N symbols must cost one message, not N.
    feed->want({"BTCUSDT", "ETHUSDT", "SOLUSDT"});
    ASSERT_TRUE(wait_until([&] { return feed->subscribed().size() == 3u; }));
    ASSERT_EQ(control.size(), 1u) << "one SUBSCRIBE, not three";
    EXPECT_NE(control.front().find("btcusdt@bookTicker"), std::string::npos);
    EXPECT_NE(control.front().find("ethusdt@bookTicker"), std::string::npos);
    EXPECT_NE(control.front().find("solusdt@bookTicker"), std::string::npos);
}

TEST_F(FeedTest, KeepsInsideTheVenuesMessageBudget) {
    run(5s, {});
    if (::testing::Test::HasFatalFailure()) return;
    // An operator mashing start and stop must not get the IP banned. Two
    // control messages a second is the cap; ten changes in a tenth of a
    // second must not become ten messages.
    for (int i = 0; i < 10; ++i) {
        feed->want(i % 2 == 0 ? std::vector<std::string>{"BTCUSDT"} : std::vector<std::string>{});
        wait_until([] { return false; }, 10ms);
    }
    wait_until([] { return false; }, 300ms);
    EXPECT_LE(control.size(), 2u) << "it sent " << control.size() << " control messages";
}

TEST_F(FeedTest, SubscribesAgainAfterAReconnect) {
    run(5s, {});
    if (::testing::Test::HasFatalFailure()) return;
    feed->want({"BTCUSDT"});
    ASSERT_TRUE(wait_until([&] { return !feed->subscribed().empty(); }));

    // A new connection carries nothing, so everything wanted is wanted
    // again - which is why want() is declarative and not a count.
    control.clear();
    server.drop();
    std::string why;
    ASSERT_TRUE(server.accept(5s, why)) << why;
    EXPECT_TRUE(wait_until([&] { return saw_control("SUBSCRIBE", "btcusdt@bookTicker"); }, 5s));
    EXPECT_EQ(feed->subscribed(), std::vector<std::string>{"BTCUSDT"});
}

TEST_F(FeedTest, KeepsWhatWasAskedForAtStartupWhateverStrategiesWant) {
    ReferenceFeed::Options options;
    options.host = "127.0.0.1";
    options.port = server.port();
    options.tls = false;
    options.symbols = {"BTCUSDT"};  // --binance BTCUSDT: keep this one warm
    options.tick_size["BTCUSDT"] = at("0.01");
    feed = std::make_unique<ReferenceFeed>(std::move(options), logger);
    ASSERT_EQ(feed->start(), "");
    std::string why;
    ASSERT_TRUE(server.accept(3s, why)) << why;

    ASSERT_TRUE(wait_until([&] { return !feed->subscribed().empty(); }));
    feed->want({});  // no strategy wants it
    wait_until([] { return false; }, 300ms);
    EXPECT_EQ(feed->subscribed(), std::vector<std::string>{"BTCUSDT"})
        << "a stream asked for on the command line stays up";
}

TEST_F(FeedTest, DoesNotReadAControlAnswerAsAPrice) {
    run(5s, {});
    if (::testing::Test::HasFatalFailure()) return;
    feed->want({"BTCUSDT"});
    ASSERT_TRUE(wait_until([&] { return !feed->subscribed().empty(); }));

    ASSERT_EQ(server.send_text(R"({"result":null,"id":1})"), "");
    ASSERT_EQ(server.send_text(R"({"code":2,"msg":"Invalid request"})"), "");
    wait_until([] { return false; }, 300ms);
    EXPECT_FALSE(feed->latest("BTCUSDT", cpm::binance::Clock::now()).present)
        << "an answer to a subscription is not a price";
}

}  // namespace
