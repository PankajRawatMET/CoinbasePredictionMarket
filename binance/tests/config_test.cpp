// config/binance.conf: the WebSocket URL and the symbol mapping, as data
// rather than as literals in the source (B-Q15, B-Q17). The parser takes
// text, so all of this runs without a filesystem.
#include "cpm/binance/config.hpp"

#include "cpm/common/fixed.hpp"

#include <gtest/gtest.h>

#include <string>

namespace {

using cpm::binance::Config;
using cpm::binance::kScale;
using cpm::binance::parse_config;
using cpm::binance::parse_ws_url;
using cpm::binance::ReferenceFeed;

cpm::binance::Price at(const char* decimal) {
    return cpm::parse_scaled(decimal, kScale).value_or(0);
}

// --- the url ---------------------------------------------------------------

TEST(BinanceUrl, TakesTheFuturesStreamApart) {
    ReferenceFeed::Options options;
    ASSERT_EQ(parse_ws_url("wss://fstream.binance.com/ws", options), "");
    EXPECT_TRUE(options.tls);
    EXPECT_EQ(options.host, "fstream.binance.com");
    EXPECT_EQ(options.port, 443) << "wss without a port is 443";
    EXPECT_EQ(options.path, "/ws");
}

TEST(BinanceUrl, TakesAPlainOneForTheSimulator) {
    ReferenceFeed::Options options;
    ASSERT_EQ(parse_ws_url("ws://127.0.0.1:9459/stream", options), "");
    EXPECT_FALSE(options.tls);
    EXPECT_EQ(options.host, "127.0.0.1");
    EXPECT_EQ(options.port, 9459);
    EXPECT_EQ(options.path, "/stream");
}

TEST(BinanceUrl, DefaultsThePathAndThePort) {
    ReferenceFeed::Options options;
    ASSERT_EQ(parse_ws_url("wss://stream.binance.com:9443", options), "");
    EXPECT_EQ(options.port, 9443);
    EXPECT_EQ(options.path, "/ws");

    ReferenceFeed::Options plain;
    ASSERT_EQ(parse_ws_url("ws://localhost", plain), "");
    EXPECT_EQ(plain.port, 80) << "ws without a port is 80";
}

TEST(BinanceUrl, RefusesWhatIsNotAWebsocketUrl) {
    ReferenceFeed::Options options;
    EXPECT_EQ(parse_ws_url("https://fstream.binance.com/ws", options),
              "a websocket url must start with wss:// or ws://");
    EXPECT_EQ(parse_ws_url("fstream.binance.com/ws", options),
              "a websocket url must start with wss:// or ws://");
    EXPECT_EQ(parse_ws_url("wss:///ws", options), "a websocket url needs a host");
    EXPECT_EQ(parse_ws_url("wss://host:https/ws", options),
              "the port in a websocket url must be a port number");
}

// --- the file --------------------------------------------------------------

TEST(BinanceConfig, DefaultsToTheFuturesStream) {
    Config config;
    ASSERT_EQ(parse_config("", config), "");
    EXPECT_EQ(config.feed.host, "fstream.binance.com");
    EXPECT_EQ(config.feed.port, 443);
    EXPECT_TRUE(config.feed.tls);
    EXPECT_EQ(config.feed.path, "/ws");
}

TEST(BinanceConfig, MovesTheVenueWithOneLine) {
    Config config;
    ASSERT_EQ(parse_config("url = ws://127.0.0.1:9459/stream\n", config), "");
    EXPECT_EQ(config.feed.host, "127.0.0.1");
    EXPECT_EQ(config.feed.port, 9459);
    EXPECT_FALSE(config.feed.tls);
    EXPECT_EQ(config.feed.path, "/stream");
}

TEST(BinanceConfig, ReadsAMappingWithItsTickSize) {
    Config config;
    ASSERT_EQ(parse_config("map.BTCD15M = { BTCUSDT, tickSize: 0.1 }\n", config), "");
    ASSERT_EQ(config.symbols.count("BTCD15M"), 1u);
    EXPECT_EQ(config.symbols.at("BTCD15M").symbol, "BTCUSDT");
    EXPECT_EQ(config.symbols.at("BTCD15M").tick_size, at("0.1"));
    // The feed is given the tick size by Binance symbol, which is what it
    // asks about.
    EXPECT_EQ(config.feed.tick_size.at("BTCUSDT"), at("0.1"));
}

TEST(BinanceConfig, TakesTheMappingHoweverItIsWritten) {
    // Quotes, braces and `:` versus `=` are all optional: an operator
    // should not have to guess which of them this wants.
    for (const std::string form : {R"(map.X = {BTCUSDT, tickSize: 0.1})",
                                   R"(map.X = { BTCUSDT , "tickSize" : 0.1 })",
                                   R"(map.X = {"BTCUSDT", tickSize=0.1})",
                                   R"(map.X = BTCUSDT, tickSize: 0.1)"}) {
        Config config;
        ASSERT_EQ(parse_config(form + "\n", config), "") << form;
        EXPECT_EQ(config.symbols.at("X").symbol, "BTCUSDT") << form;
        EXPECT_EQ(config.symbols.at("X").tick_size, at("0.1")) << form;
    }
}

TEST(BinanceConfig, UpperCasesBothEnds) {
    Config config;
    ASSERT_EQ(parse_config("map.btcd15m = {btcusdt, tickSize: 0.1}\n", config), "");
    EXPECT_EQ(config.symbols.at("BTCD15M").symbol, "BTCUSDT");
}

TEST(BinanceConfig, InsistsOnATickSize) {
    Config config;
    // There is no REST lookup to fall back on any more (B-Q17), so a
    // mapping without one would silently disable the guard for that
    // instrument - the exact failure that was found and fixed in B-Q16.
    EXPECT_EQ(parse_config("map.BTCD15M = BTCUSDT\n", config),
              "map.BTCD15M needs a tickSize, as in {BTCUSDT, tickSize: 0.1} (line 1)");
    EXPECT_EQ(parse_config("map.BTCD15M = {BTCUSDT, tickSize: 0}\n", config),
              "map.BTCD15M's tickSize must be a positive decimal (line 1)");
    EXPECT_EQ(parse_config("map.BTCD15M = {tickSize: 0.1}\n", config),
              "map.BTCD15M needs a Binance symbol (line 1)");
    EXPECT_EQ(parse_config("map.BTCD15M = {BTCUSDT, tick: 0.1}\n", config),
              "map.BTCD15M does not know the setting tick (line 1)");
}

TEST(BinanceConfig, RefusesTwoTickSizesForOneSymbol) {
    Config config;
    // A tick size belongs to the Binance symbol, so two instruments that
    // disagree about one are a contradiction rather than an override.
    EXPECT_EQ(parse_config(R"(
        map.BTCD15M = {BTCUSDT, tickSize: 0.1}
        map.BTCD1H  = {BTCUSDT, tickSize: 0.01}
    )", config), "BTCUSDT is given two different tick sizes (line 3)");

    Config agreeing;
    EXPECT_EQ(parse_config(R"(
        map.BTCD15M = {BTCUSDT, tickSize: 0.1}
        map.BTCD1H  = {BTCUSDT, tickSize: 0.1}
    )", agreeing), "") << "two instruments on one symbol is fine when they agree";
}

TEST(BinanceConfig, ReadsTheTimingsAndThePins) {
    Config config;
    ASSERT_EQ(parse_config(R"(
        stale_after_ms = 2500
        reconnect_after_ms = 250
        max_reconnect_after_ms = 60000
        control_per_sec = 1
        pin = btcusdt
    )", config), "");
    EXPECT_EQ(config.feed.stale_after, std::chrono::milliseconds(2500));
    EXPECT_EQ(config.feed.reconnect_after, std::chrono::milliseconds(250));
    EXPECT_EQ(config.feed.max_reconnect_after, std::chrono::milliseconds(60000));
    EXPECT_EQ(config.feed.control_per_sec, 1);
    ASSERT_EQ(config.feed.symbols.size(), 1u);
    EXPECT_EQ(config.feed.symbols.front(), "BTCUSDT");
}

TEST(BinanceConfig, IgnoresBlankLinesAndComments) {
    Config config;
    ASSERT_EQ(parse_config(R"(
# a whole-line comment

   url = ws://localhost:1234/ws   # and one after a value

    )", config), "");
    EXPECT_EQ(config.feed.port, 1234);
}

TEST(BinanceConfig, RefusesASettingNobodyReads) {
    Config config;
    // The one that matters. A key that is quietly ignored is a setting
    // that quietly does not apply, and the operator believes the daemon is
    // pointed somewhere it is not - which is how the creds-rest.env that
    // was never sourced went unnoticed for a whole session.
    EXPECT_EQ(parse_config("websocket_url = wss://fstream.binance.com/ws\n", config),
              "unknown setting websocket_url (line 1)");
    // Including the keys this file used to have.
    EXPECT_EQ(parse_config("rest.host = api.binance.com\n", config),
              "unknown setting rest.host (line 1)");
    EXPECT_EQ(parse_config("stream.host = fstream.binance.com\n", config),
              "unknown setting stream.host (line 1)");
}

TEST(BinanceConfig, RefusesAValueItCannotUse) {
    Config config;
    EXPECT_EQ(parse_config("url = http://fstream.binance.com/ws\n", config),
              "a websocket url must start with wss:// or ws:// (line 1)");
    EXPECT_EQ(parse_config("stale_after_ms = -1\n", config),
              "stale_after_ms must be a whole number of milliseconds (line 1)");
    EXPECT_EQ(parse_config("just a line\n", config), "expected key = value (line 1): just a line");
    EXPECT_EQ(parse_config("url =\n", config), "the key url has no value (line 1)");
}

TEST(BinanceConfig, TheFileShippedWithTheProjectParses) {
    // The one in config/ is the documentation of these settings, and a
    // comment in it that was not a comment would be found here first.
    Config config;
    bool found = false;
    const auto failed = cpm::binance::load_config(std::string(CPM_DICT_DIR) + "/binance.conf",
                                                  config, found);
    EXPECT_EQ(failed, "");
    EXPECT_TRUE(found) << "config/binance.conf is missing";
    EXPECT_EQ(config.feed.host, "fstream.binance.com");
    EXPECT_EQ(config.feed.path, "/ws");
    EXPECT_EQ(config.symbols.at("BTCD15M").symbol, "BTCUSDT");
    // Futures BTCUSDT quotes to two decimals - 80162.60 / 80162.70 - so a
    // tick is 0.1, and PricingMidMoveThreshold counts those.
    EXPECT_EQ(config.symbols.at("BTCD15M").tick_size, at("0.1"));
    EXPECT_EQ(config.symbols.at("ETHD15M").tick_size, at("0.01"));
}

TEST(BinanceConfig, AMissingFileIsNotAnError) {
    // At the default path it means the built-in defaults. The caller is
    // what decides that a file named on the command line must exist.
    Config config;
    bool found = true;
    EXPECT_EQ(cpm::binance::load_config("/nowhere/binance.conf", config, found), "");
    EXPECT_FALSE(found);
    EXPECT_EQ(config.feed.host, "fstream.binance.com");
}

TEST(BinanceConfig, ReferenceHistoryCacheCanUseAnExplicitLocalPath) {
    Config config;
    EXPECT_EQ(cpm::binance::parse_config("history_file = '/tmp/reference history'\n", config), "");
    EXPECT_EQ(config.feed.history_file, "/tmp/reference history");
}

TEST(BinanceConfig, RestIsAHostAndNothingElse) {
    // Where QuotingMM reads a window's sigma and opening price from. The
    // default is production USD-M futures, so a file that says nothing
    // about REST still works (B-Q19).
    Config defaults;
    EXPECT_EQ(cpm::binance::parse_config("url = wss://fstream.binance.com/ws\n", defaults), "");
    EXPECT_EQ(defaults.rest.host, "fapi.binance.com");
    EXPECT_EQ(defaults.rest.port, 443);

    Config testnet;
    EXPECT_EQ(cpm::binance::parse_config("rest_url = https://testnet.binancefuture.com\n", testnet), "");
    EXPECT_EQ(testnet.rest.host, "testnet.binancefuture.com");

    Config ported;
    EXPECT_EQ(cpm::binance::parse_config("rest_url = https://localhost:8443\n", ported), "");
    EXPECT_EQ(ported.rest.host, "localhost");
    EXPECT_EQ(ported.rest.port, 8443);

    // A trailing slash is a host; a path is the venue's to choose, not an
    // operator's, and http:// would hand the answer to whoever replied.
    Config slash;
    EXPECT_EQ(cpm::binance::parse_config("rest_url = https://fapi.binance.com/\n", slash), "");
    EXPECT_EQ(slash.rest.host, "fapi.binance.com");
    Config pathed;
    EXPECT_NE(cpm::binance::parse_config("rest_url = https://fapi.binance.com/fapi/v1\n", pathed), "");
    Config plain;
    EXPECT_NE(cpm::binance::parse_config("rest_url = http://fapi.binance.com\n", plain), "");
    Config bad_port;
    EXPECT_NE(cpm::binance::parse_config("rest_url = https://fapi.binance.com:none\n", bad_port), "");
}

}  // namespace
