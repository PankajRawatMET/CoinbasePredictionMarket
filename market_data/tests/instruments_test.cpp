// The REST instrument catalog, without a network. The body below is the one
// the sandbox actually returned on 2026-10-28
// (docs/spec/capture-rest-20261028T1205-instruments.txt), trimmed to the
// fields anything reads.
#include "cpm/md/instruments.hpp"

#include "cpm/md/ticker.hpp"

#include <gtest/gtest.h>

namespace {

using cpm::md::InstrumentQuery;
using cpm::md::open_instruments;
using cpm::md::parse_rest_url;
using cpm::md::RestConfig;
using cpm::md::TickStructure;

RestConfig config() {
    RestConfig out;
    out.api_key = "test-key";
    out.passphrase = "test-passphrase";
    out.secret_b64 = "Y3BtLXJlc3QtdGVzdC1zZWNyZXQtbm90LXJlYWw=";
    out.url = "https://api.cpm-sandbox.fairx.net/api";
    return out;
}

// 2026-10-28T12:00:00Z, so the timestamp in the signature is fixed.
cpm::md::Clock::time_point at(long long seconds) {
    return cpm::md::Clock::time_point(std::chrono::seconds(seconds));
}

const char* kBody = R"({"data":[
  {"ticker":"BTCD15M-BTC26OCT281230-0","id":"241172901338808320","series_ticker":"BTCD15M",
   "status":"OPEN","expected_close_time":1793190600000,"expected_open_time":1793189700000,
   "close_time":null,"price_level_structure":"TAPERED_DECI_CENT","open_interest":"6.00"}
],"cursor":""})";

cpm::HttpsResponse answer(int status, std::string body) {
    cpm::HttpsResponse out;
    out.status = status;
    out.body = std::move(body);
    return out;
}

TEST(RestUrl, AcceptsEveryShapeAnOperatorMightPaste) {
    // The spec writes endpoints as /api/v1/...; the operator pasted the base
    // ending in /api. All three have to land in the same place.
    for (const auto& url : {"https://api.cpm-sandbox.fairx.net",
                            "https://api.cpm-sandbox.fairx.net/",
                            "https://api.cpm-sandbox.fairx.net/api",
                            "https://api.cpm-sandbox.fairx.net/api/",
                            "https://api.cpm-sandbox.fairx.net/api/v1"}) {
        const auto parsed = parse_rest_url(url);
        EXPECT_TRUE(parsed.error.empty()) << url << ": " << parsed.error;
        EXPECT_EQ(parsed.host, "api.cpm-sandbox.fairx.net") << url;
        EXPECT_EQ(parsed.port, 443) << url;
        EXPECT_EQ(parsed.prefix, "/api/v1") << url;
    }

    const auto with_port = parse_rest_url("https://localhost:8443/api");
    EXPECT_EQ(with_port.host, "localhost");
    EXPECT_EQ(with_port.port, 8443);
    EXPECT_EQ(with_port.prefix, "/api/v1");
}

TEST(RestUrl, RefusesWhatWouldSendCredentialsInTheClear) {
    // An API key goes on this connection.
    EXPECT_NE(parse_rest_url("http://api.cpm-sandbox.fairx.net").error.find("must be https"),
              std::string::npos);
    EXPECT_FALSE(parse_rest_url("").error.empty());
    EXPECT_FALSE(parse_rest_url("https://").error.empty());
    EXPECT_FALSE(parse_rest_url("https://host:notaport/api").error.empty());
}

TEST(RestSignature, IsWhatTheSpecAsksFor) {
    // base64(HMAC-SHA256(timestamp + method + path)), query excluded, empty
    // body on a GET (API spec 2.2). Vector computed independently.
    EXPECT_EQ(cpm::md::rest_signature("Y3BtLXJlc3QtdGVzdC1zZWNyZXQtbm90LXJlYWw=", "1793188800",
                                      "GET", "/api/v1/instruments"),
              "1FeW2juh5x2g8vJef+xuJpaiB55T99ApJH2aSJC3vdE=");
}

TEST(Instruments, AsksForOneSeriesAndSignsWithoutTheQuery) {
    cpm::HttpsRequest seen;
    const auto result = open_instruments(config(), "BTCD15M",
                                         [&seen](const cpm::HttpsRequest& request) {
                                             seen = request;
                                             return answer(200, kBody);
                                         },
                                         at(1793188800));
    ASSERT_TRUE(result.ok()) << result.error;

    EXPECT_EQ(seen.host, "api.cpm-sandbox.fairx.net");
    EXPECT_EQ(seen.path, "/api/v1/instruments");
    // One series per call: the venue answers 400 to a repeated series_ticker
    // and zero rows to a comma-separated one.
    EXPECT_EQ(seen.query, "series_ticker=BTCD15M&status=OPEN");

    const auto header = [&seen](const std::string& name) {
        for (const auto& [key, value] : seen.headers) {
            if (key == name) return value;
        }
        return std::string();
    };
    EXPECT_EQ(header("CB-ACCESS-KEY"), "test-key");
    EXPECT_EQ(header("CB-ACCESS-PASSPHRASE"), "test-passphrase");
    EXPECT_EQ(header("CB-ACCESS-TIMESTAMP"), "1793188800");
    // Signed over the path alone - the query is deliberately not in it.
    EXPECT_EQ(header("CB-ACCESS-SIGN"), "1FeW2juh5x2g8vJef+xuJpaiB55T99ApJH2aSJC3vdE=");
}

TEST(Instruments, ReadsWhatTheVenueAnswered) {
    const auto result = open_instruments(config(), "BTCD15M",
                                         [](const cpm::HttpsRequest&) { return answer(200, kBody); },
                                         at(1793188800));
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_EQ(result.instruments.size(), 1u);

    const auto& instrument = result.instruments.front();
    EXPECT_EQ(instrument.ticker, "BTCD15M-BTC26OCT281230-0");
    EXPECT_EQ(instrument.id, "241172901338808320");
    EXPECT_EQ(instrument.series_ticker, "BTCD15M");
    EXPECT_EQ(instrument.status, "OPEN");
    // expected_close_time, in milliseconds, is 2026-10-28T12:30:00Z - and the
    // ticker says the same, which is the cross-check that keeps parse_ticker
    // honest.
    EXPECT_EQ(instrument.expected_close, at(1793190600));
    EXPECT_EQ(cpm::md::parse_ticker(instrument.ticker)->window_end, instrument.expected_close);
    // The venue states the grid, so it stops being the --tick-structure guess.
    ASSERT_TRUE(instrument.ticks.has_value());
    EXPECT_EQ(*instrument.ticks, TickStructure::TaperedDeciCent);
}

TEST(Instruments, SortsBySoonestToEndAndFallsBackToTheName) {
    // Two open at once should not happen, but if it does the one trading now
    // is the one that ends first. And a row with no expected_close_time still
    // has its end in its name.
    const char* body = R"({"data":[
      {"ticker":"BTCD15M-BTC26OCT281245-0","status":"OPEN","expected_close_time":1793191500000},
      {"ticker":"BTCD15M-BTC26OCT281230-0","status":"OPEN"}
    ]})";
    const auto result = open_instruments(config(), "BTCD15M",
                                         [body](const cpm::HttpsRequest&) { return answer(200, body); },
                                         at(1793188800));
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_EQ(result.instruments.size(), 2u);
    EXPECT_EQ(result.instruments[0].ticker, "BTCD15M-BTC26OCT281230-0");
    EXPECT_EQ(result.instruments[0].expected_close, at(1793190600)) << "taken from the name";
    EXPECT_EQ(result.instruments[1].ticker, "BTCD15M-BTC26OCT281245-0");
    EXPECT_FALSE(result.instruments[0].ticks.has_value()) << "no structure was stated";
}

TEST(Instruments, SaysWhatWentWrongRatherThanReturningNothing) {
    const auto with = [](cpm::HttpsResponse response) {
        return open_instruments(config(), "BTCD15M",
                                [&response](const cpm::HttpsRequest&) { return response; },
                                at(1793188800));
    };

    cpm::HttpsResponse unreachable;
    unreachable.error = "the TLS handshake failed";
    EXPECT_NE(with(unreachable).error.find("TLS handshake"), std::string::npos);

    const auto refused = with(answer(401, R"({"error":"UNAUTHORIZED"})"));
    EXPECT_NE(refused.error.find("HTTP 401"), std::string::npos);
    EXPECT_NE(refused.error.find("UNAUTHORIZED"), std::string::npos) << refused.error;

    EXPECT_NE(with(answer(200, "<html>not json</html>")).error.find("not JSON"), std::string::npos);
    EXPECT_NE(with(answer(200, R"({"cursor":""})")).error.find("no \"data\" array"),
              std::string::npos);

    // An empty answer is not an error: the series has no window open, which
    // happens in the second between one closing and the next opening.
    const auto empty = with(answer(200, R"({"data":[]})"));
    EXPECT_TRUE(empty.ok()) << empty.error;
    EXPECT_TRUE(empty.instruments.empty());
}

TEST(Instruments, RollingSeriesIgnoresFeederExpiredAndUnrelatedInstruments) {
    const char* body = R"({"data":[
      {"ticker":"ETHFEEDERTEST","series_ticker":"ETHD15M","status":"OPEN"},
      {"ticker":"ETHD15M-ETH26OCT281145-0","status":"OPEN"},
      {"ticker":"BTCD15M-BTC26OCT281230-0","status":"OPEN"},
      {"ticker":"ETHD15M-ETH26OCT281245-0","status":"LISTED"},
      {"ticker":"ETHD15M-ETH26OCT281230-0","series_ticker":"ETHD15M","status":"OPEN"}
    ]})";
    const auto result = open_instruments(config(), "ETHD15M",
        [body](const cpm::HttpsRequest&) { return answer(200, body); }, at(1793188800));
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_EQ(result.instruments.size(), 1u);
    EXPECT_EQ(result.instruments.front().ticker, "ETHD15M-ETH26OCT281230-0");
}

TEST(Instruments, AFeederAloneDoesNotResolveATimedSeries) {
    const auto result = open_instruments(config(), "ETHD15M", [](const cpm::HttpsRequest&) {
        return answer(200, R"({"data":[{"ticker":"ETHFEEDERTEST","status":"OPEN"}]})");
    }, at(1793188800));
    EXPECT_TRUE(result.ok());
    EXPECT_TRUE(result.instruments.empty());
}

TEST(Instruments, RefusesToAskWithoutCredentials) {
    RestConfig incomplete;
    incomplete.url = "https://api.cpm-sandbox.fairx.net";
    bool asked = false;
    const auto result = open_instruments(incomplete, "BTCD15M",
                                         [&asked](const cpm::HttpsRequest&) {
                                             asked = true;
                                             return answer(200, kBody);
                                         },
                                         at(1793188800));
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("creds-rest.env"), std::string::npos) << result.error;
    EXPECT_FALSE(asked) << "it must not open a connection it cannot sign";
}

}  // namespace
