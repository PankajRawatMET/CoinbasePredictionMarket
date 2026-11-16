// The server over a real loopback socket, driven with the same bytes a browser
// sends. The fakes in support.hpp stand in for the sessions, so everything
// here runs with no venue, no simulator and no FIX.
#include "cpm/gui/server.hpp"

#include "cpm/gui/json.hpp"
#include "cpm/gui/views.hpp"
#include "support.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

namespace {

using namespace cpm::gui::test;

class ServerTest : public ::testing::Test {
protected:
    void SetUp() override {
        Server::Options options;
        options.port = 0;  // the kernel picks one, so tests never collide
        options.token = "deadbeefcafe0001";
        options.logs = &ring;
        server = std::make_unique<Server>(*daemon.api, options, daemon.logger);
        ASSERT_EQ(server->start(), "");
        ASSERT_TRUE(client.connect(server->port()));
    }

    void TearDown() override {
        client.disconnect();
        if (server) server->stop();
    }

    std::string request(const std::string& method, const std::string& target,
                        const std::string& body = {}, const std::string& extra = {}) {
        std::string text = method + " " + target + " HTTP/1.1\r\nHost: localhost\r\n" + extra;
        if (!body.empty()) text += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        text += "\r\n" + body;
        EXPECT_TRUE(client.write(text));
        return client.response();
    }

    std::string authed(const std::string& method, const std::string& target,
                       const std::string& body = {}) {
        return request(method, target, body, "Authorization: Bearer " + server->token() + "\r\n");
    }

    Daemon daemon;
    cpm::LogRing ring;
    std::unique_ptr<Server> server;
    Client client;
};

TEST_F(ServerTest, RefusesEverythingWithoutTheToken) {
    const auto response = request("GET", "/api/snapshot");
    EXPECT_EQ(status_of(response), 401);
    EXPECT_NE(body_of(response).find("run token"), std::string::npos);
}

TEST_F(ServerTest, RefusesAWrongToken) {
    const auto response =
        request("GET", "/api/snapshot", {}, "Authorization: Bearer 0000000000000000\r\n");
    EXPECT_EQ(status_of(response), 401);
}

TEST_F(ServerTest, AcceptsTheTokenInTheHeaderOrTheQuery) {
    EXPECT_EQ(status_of(authed("GET", "/api/snapshot")), 200);
    // EventSource cannot set a header, so the query has to work too.
    EXPECT_EQ(status_of(request("GET", "/api/snapshot?t=" + server->token())), 200);
}

TEST_F(ServerTest, RefusesARequestForwardedFromAnotherName) {
    // A page on another site cannot reach a loopback server it has to name
    // something else, even if that name resolves to 127.0.0.1.
    const auto response = request("GET", "/api/snapshot?t=" + server->token(), {},
                                  "X-Nothing: 1\r\n");
    EXPECT_EQ(status_of(response), 200);  // Host: localhost, from request()

    std::string text = "GET /api/snapshot?t=" + server->token() +
                       " HTTP/1.1\r\nHost: trading.example.com\r\n\r\n";
    ASSERT_TRUE(client.write(text));
    EXPECT_EQ(status_of(client.response()), 403);
}

TEST_F(ServerTest, ServesThePageAsOneSelfContainedDocument) {
    const auto response = request("GET", "/?t=" + server->token());
    EXPECT_EQ(status_of(response), 200);
    EXPECT_NE(response.find("Content-Type: text/html"), std::string::npos);
    EXPECT_NE(response.find("Content-Security-Policy:"), std::string::npos);
    EXPECT_NE(body_of(response).find("<!doctype html>"), std::string::npos);
    EXPECT_NE(body_of(response).find("/api/events"), std::string::npos);
}

TEST_F(ServerTest, SaysWhatItHasAndWhatItDoesNot) {
    EXPECT_EQ(status_of(authed("GET", "/api/nothing")), 404);
    EXPECT_EQ(status_of(authed("POST", "/api/snapshot")), 405);
    EXPECT_EQ(status_of(authed("GET", "/api/stop-all")), 405);
    EXPECT_EQ(status_of(authed("POST", "/api/strategies/s1/levitate")), 404);
}

TEST_F(ServerTest, TheSnapshotCarriesTheDaemonsState) {
    const auto body = body_of(authed("GET", "/api/snapshot"));
    const auto parsed = cpm::gui::json_parse(body);
    ASSERT_TRUE(parsed.ok()) << parsed.error << " in " << body;
    ASSERT_NE(parsed.value.find("state"), nullptr);
    EXPECT_EQ(parsed.value.find("state")->text, "live");
    EXPECT_TRUE(parsed.value.find("allow_orders")->boolean);
    ASSERT_NE(parsed.value.find("market_data"), nullptr);
    EXPECT_TRUE(parsed.value.find("market_data")->find("logged_on")->boolean);
    ASSERT_NE(parsed.value.find("strategies"), nullptr);
    EXPECT_TRUE(parsed.value.find("strategies")->items.empty());
}

TEST_F(ServerTest, ServesAnInstrumentsWholePriceGrid) {
    // Nothing is watching the symbol yet, so the daemon has no ladder for it.
    EXPECT_EQ(status_of(authed("GET", "/api/ladder?symbol=" + kSym)), 404);
    EXPECT_EQ(status_of(authed("GET", "/api/ladder")), 400);

    ASSERT_EQ(status_of(authed("POST", "/api/strategies",
                               R"({"kind":"CryptoMM","params":{"symbol":")" + kSym +
                                   R"(","qty":"1"}})")),
              200);

    const auto response = authed("GET", "/api/ladder?symbol=" + kSym);
    ASSERT_EQ(status_of(response), 200) << body_of(response);
    const auto parsed = cpm::gui::json_parse(body_of(response));
    ASSERT_TRUE(parsed.ok()) << parsed.error;
    EXPECT_EQ(parsed.value.find("symbol")->text, kSym);
    EXPECT_EQ(parsed.value.find("structure")->text, "TAPERED_DECI_CENT");

    // TAPERED_DECI_CENT, which is what the venue reports for these series
    // (docs/spec/capture-rest-20261028T1205-instruments.txt): a tenth of a
    // point below 10% and above 90%, one point between.
    const auto& rungs = parsed.value.find("rungs")->items;
    ASSERT_EQ(rungs.size(), 281u) << "101 tenths, 80 points, 100 more tenths";
    EXPECT_EQ(rungs.front().find("price")->text, "0");
    EXPECT_EQ(rungs.front().find("price_ticks")->text, "0");
    EXPECT_EQ(rungs.back().find("price")->text, "100");
    EXPECT_EQ(rungs.back().find("price_ticks")->text, "1000");
    EXPECT_EQ(rungs[1].find("price")->text, "0.1");
    EXPECT_EQ(rungs[99].find("price")->text, "9.9");
    EXPECT_EQ(rungs[100].find("price")->text, "10");
    EXPECT_EQ(rungs[101].find("price")->text, "11");
    EXPECT_EQ(rungs[180].find("price")->text, "90");
    EXPECT_EQ(rungs[181].find("price")->text, "90.1");
    // The ends are rungs but not prices an order can be placed at (O5).
    EXPECT_FALSE(rungs.front().find("quotable")->boolean);
    EXPECT_FALSE(rungs.back().find("quotable")->boolean);
    EXPECT_TRUE(rungs[50].find("quotable")->boolean);
    EXPECT_TRUE(rungs[1].find("quotable")->boolean);
    // Ascending, so the page can draw it straight down the screen.
    for (std::size_t i = 1; i < rungs.size(); ++i) {
        EXPECT_GT(std::stoll(rungs[i].find("price_ticks")->text),
                  std::stoll(rungs[i - 1].find("price_ticks")->text));
    }
    EXPECT_EQ(status_of(authed("POST", "/api/ladder?symbol=" + kSym)), 405);
}

TEST_F(ServerTest, TheSchemaIsWhatAFormWouldBeBuiltFrom) {
    const auto body = body_of(authed("GET", "/api/schema"));
    const auto parsed = cpm::gui::json_parse(body);
    ASSERT_TRUE(parsed.ok()) << parsed.error;
    const auto* kinds = parsed.value.find("kinds");
    ASSERT_NE(kinds, nullptr);
    ASSERT_EQ(kinds->items.size(), 1u);
    EXPECT_EQ(kinds->items[0].find("name")->text, "CryptoMM");
    const auto* params = kinds->items[0].find("params");
    ASSERT_NE(params, nullptr);
    EXPECT_FALSE(params->items.empty());
    bool saw_symbol = false;
    bool saw_spread = false;
    for (const auto& param : params->items) {
        // Every parameter carries a label, even when it is empty: the page
        // title-cases the name in that case, and a missing field would be a
        // different question from an empty one.
        ASSERT_NE(param.find("label"), nullptr) << param.find("name")->text;
        if (param.find("name")->text == "symbol") {
            saw_symbol = true;
            EXPECT_TRUE(param.find("required")->boolean);
        }
        if (param.find("name")->text == "width-ticks") {
            saw_spread = true;
            // The flag keeps its name; the row calls it what the operator
            // calls it (GUI-Q25).
            EXPECT_EQ(param.find("label")->text, "Spread");
        }
    }
    EXPECT_TRUE(saw_symbol);
    EXPECT_TRUE(saw_spread);
}

TEST_F(ServerTest, CreatesStartsAndArmsOverHttp) {
    const auto created = authed("POST", "/api/strategies",
                                R"({"kind":"CryptoMM","params":{"symbol":")" + kSym +
                                    R"(","qty":"1"}})");
    ASSERT_EQ(status_of(created), 200) << body_of(created);
    const auto parsed = cpm::gui::json_parse(body_of(created));
    ASSERT_TRUE(parsed.ok()) << parsed.error;
    ASSERT_NE(parsed.value.find("id"), nullptr);
    const auto id = parsed.value.find("id")->text;
    ASSERT_FALSE(id.empty());

    EXPECT_EQ(status_of(authed("POST", "/api/strategies/" + id + "/start")), 200);
    EXPECT_EQ(status_of(authed("POST", "/api/strategies/" + id + "/arm")), 200);

    const auto snapshot = cpm::gui::json_parse(body_of(authed("GET", "/api/snapshot")));
    ASSERT_TRUE(snapshot.ok());
    const auto* strategies = snapshot.value.find("strategies");
    ASSERT_EQ(strategies->items.size(), 1u);
    EXPECT_EQ(strategies->items[0].find("id")->text, id);
    EXPECT_EQ(strategies->items[0].find("state")->text, "running");
    EXPECT_TRUE(strategies->items[0].find("armed")->boolean);
    EXPECT_EQ(strategies->items[0].find("params")->find("symbol")->text, kSym);

    // Stopping disarms (GUI-Q13), and the GUI sees that without being told.
    EXPECT_EQ(status_of(authed("POST", "/api/strategies/" + id + "/stop")), 200);
    const auto after = cpm::gui::json_parse(body_of(authed("GET", "/api/snapshot")));
    EXPECT_FALSE(after.value.find("strategies")->items[0].find("armed")->boolean);
}

TEST_F(ServerTest, RetunesAStrategyOverHttpUntilItIsArmed) {
    const auto created = authed("POST", "/api/strategies",
                                R"({"kind":"CryptoMM","params":{"symbol":")" + kSym +
                                    R"(","qty":"1"}})");
    ASSERT_EQ(status_of(created), 200) << body_of(created);
    const auto id = cpm::gui::json_parse(body_of(created)).value.find("id")->text;
    const auto params = "/api/strategies/" + id + "/params";

    // A feed that quotes nothing: the parameters are the operator's to change
    // (GUI-Q30). Numbers as well as strings, as the page sends them.
    const auto retuned = authed("POST", params, R"({"params":{"width-ticks":3,"qty":"4"}})");
    ASSERT_EQ(status_of(retuned), 200) << body_of(retuned);
    auto snapshot = cpm::gui::json_parse(body_of(authed("GET", "/api/snapshot")));
    const auto* after = snapshot.value.find("strategies")->items[0].find("params");
    EXPECT_EQ(after->find("width-ticks")->text, "3");
    EXPECT_EQ(after->find("qty")->text, "4");

    // Armed, and the same request is refused with its reason.
    ASSERT_EQ(status_of(authed("POST", "/api/strategies/" + id + "/start")), 200);
    ASSERT_EQ(status_of(authed("POST", "/api/strategies/" + id + "/arm")), 200);
    const auto refused = authed("POST", params, R"({"params":{"width-ticks":"5"}})");
    EXPECT_EQ(status_of(refused), 409);
    EXPECT_NE(body_of(refused).find("disarm it"), std::string::npos) << body_of(refused);

    // Disarmed again, and it takes the edit while it runs.
    ASSERT_EQ(status_of(authed("POST", "/api/strategies/" + id + "/disarm")), 200);
    EXPECT_EQ(status_of(authed("POST", params, R"({"params":{"width-ticks":"5"}})")), 200);
    snapshot = cpm::gui::json_parse(body_of(authed("GET", "/api/snapshot")));
    const auto& strategy = snapshot.value.find("strategies")->items[0];
    EXPECT_EQ(strategy.find("params")->find("width-ticks")->text, "5");
    EXPECT_EQ(strategy.find("state")->text, "running");
}

TEST_F(ServerTest, RefusesAParamsBodyThatIsNotWhatItClaims) {
    const auto created = authed("POST", "/api/strategies",
                                R"({"kind":"CryptoMM","params":{"symbol":")" + kSym +
                                    R"(","qty":"1"}})");
    ASSERT_EQ(status_of(created), 200) << body_of(created);
    const auto id = cpm::gui::json_parse(body_of(created)).value.find("id")->text;
    const auto params = "/api/strategies/" + id + "/params";

    EXPECT_EQ(status_of(authed("POST", params, "{not json")), 400);
    EXPECT_EQ(status_of(authed("POST", params, R"({"params":[]})")), 400);
    EXPECT_EQ(status_of(authed("POST", params, R"({"params":{"qty":null}})")), 400);
    // A value the schema refuses is the daemon's answer, not the parser's.
    EXPECT_EQ(status_of(authed("POST", params, R"({"params":{"qty":"0"}})")), 409);
    // No params at all changes nothing and says so by succeeding.
    EXPECT_EQ(status_of(authed("POST", params, "{}")), 200);
}

TEST_F(ServerTest, ARefusalCrossesTheWireIntact) {
    const std::string body =
        R"({"kind":"CryptoMM","params":{"symbol":")" + kSym + R"(","qty":"1"}})";
    ASSERT_EQ(status_of(authed("POST", "/api/strategies", body)), 200);

    // GUI-Q6: two strategies on one symbol share a position, so the second is
    // refused - and the reason has to reach the operator, not just a log.
    const auto again = authed("POST", "/api/strategies", body);
    EXPECT_EQ(status_of(again), 409);
    const auto parsed = cpm::gui::json_parse(body_of(again));
    ASSERT_TRUE(parsed.ok());
    EXPECT_FALSE(parsed.value.find("ok")->boolean);
    EXPECT_NE(parsed.value.find("error")->text.find(kSym), std::string::npos);

    EXPECT_EQ(status_of(authed("POST", "/api/strategies/nobody/start")), 409);
}

TEST_F(ServerTest, RefusesABodyThatIsNotWhatItClaims) {
    EXPECT_EQ(status_of(authed("POST", "/api/strategies", "{not json")), 400);
    EXPECT_EQ(status_of(authed("POST", "/api/strategies", R"({"params":{}})")), 400);
    EXPECT_EQ(status_of(authed("POST", "/api/strategies", R"({"kind":"CryptoMM","params":[]})")), 400);
    EXPECT_EQ(status_of(authed("POST", "/api/strategies",
                               R"({"kind":"CryptoMM","params":{"symbol":null}})")),
              400);
    // A kind nobody registered is an operator error, not a malformed request.
    EXPECT_EQ(status_of(authed("POST", "/api/strategies", R"({"kind":"Arbitrage","params":{}})")),
              409);
}

TEST_F(ServerTest, TheKillSwitchIsOnePost) {
    const auto created = authed("POST", "/api/strategies",
                                R"({"kind":"CryptoMM","params":{"symbol":")" + kSym +
                                    R"(","qty":"1"}})");
    const auto id = cpm::gui::json_parse(body_of(created)).value.find("id")->text;
    ASSERT_EQ(status_of(authed("POST", "/api/strategies/" + id + "/start")), 200);
    ASSERT_EQ(status_of(authed("POST", "/api/strategies/" + id + "/arm")), 200);

    EXPECT_EQ(status_of(authed("POST", "/api/stop-all")), 200);
    const auto after = cpm::gui::json_parse(body_of(authed("GET", "/api/snapshot")));
    const auto& strategy = after.value.find("strategies")->items[0];
    // The row shows the series, not the window it is on (GUI-Q26). kSym is
    // SIMOE-26NOV01HOMAWY-HOM, which parse_ticker cannot take apart, so
    // this is also the fallback being exercised.
    EXPECT_EQ(strategy.find("series")->text, "SIMOE");
    EXPECT_FALSE(strategy.find("armed")->boolean);
    EXPECT_NE(strategy.find("state")->text, "running");
}

// --- exiting a position over HTTP (G13) -----------------------------------

TEST_F(ServerTest, ExitsAPositionWithOnePost) {
    cpm::trader::PositionView position;
    position.symbol = kSym;
    position.net = 500;  // long 5
    position.fills = 2;
    daemon.positions.push_back(position);

    // The quantity is in contracts, written as the page shows it.
    EXPECT_EQ(status_of(authed("POST", "/api/positions/" + kSym + "/exit", R"({"qty":2})")), 200);
    ASSERT_EQ(daemon.exits.size(), 1u);
    EXPECT_EQ(daemon.exits.front().symbol, kSym);
    EXPECT_EQ(daemon.exits.front().side, cpm::oms::Side::Sell);
    EXPECT_EQ(daemon.exits.front().quantity, 200);

    // No body, or no quantity in it, is the whole position.
    EXPECT_EQ(status_of(authed("POST", "/api/positions/" + kSym + "/exit")), 200);
    ASSERT_EQ(daemon.exits.size(), 2u);
    EXPECT_EQ(daemon.exits.back().quantity, 500);
}

TEST_F(ServerTest, AnExitRefusalCrossesTheWireAsOne) {
    // Nothing is held, so there is nothing to exit: an operator error, which
    // is a 409 like every other refusal, with the daemon's own words in it.
    const auto response = authed("POST", "/api/positions/" + kSym + "/exit", R"({"qty":1})");
    EXPECT_EQ(status_of(response), 409);
    EXPECT_NE(body_of(response).find("holds no position"), std::string::npos)
        << body_of(response);
    EXPECT_TRUE(daemon.exits.empty());
}

TEST_F(ServerTest, RefusesAnExitBodyThatIsNotAQuantity) {
    cpm::trader::PositionView position;
    position.symbol = kSym;
    position.net = 500;
    daemon.positions.push_back(position);

    EXPECT_EQ(status_of(authed("POST", "/api/positions/" + kSym + "/exit", "{not json")), 400);
    EXPECT_EQ(status_of(authed("POST", "/api/positions/" + kSym + "/exit", R"({"qty":"two"})")),
              400);
    EXPECT_EQ(status_of(authed("POST", "/api/positions/" + kSym + "/exit", R"({"qty":-1})")), 400);
    EXPECT_EQ(status_of(authed("POST", "/api/positions/" + kSym + "/exit", R"({"qty":[]})")), 400);
    // An exit is a POST, and the path needs the action on the end.
    EXPECT_EQ(status_of(authed("GET", "/api/positions/" + kSym + "/exit")), 405);
    EXPECT_EQ(status_of(authed("POST", "/api/positions/" + kSym)), 404);
    EXPECT_TRUE(daemon.exits.empty()) << "nothing may reach the venue from a bad request";
}

TEST_F(ServerTest, SendsTheLogTailOnTheStreamAndThenOnlyWhatIsNew) {
    // A line written before the page arrived: the panel at the bottom must
    // show it, or a reload would lose the halt that explains the screen.
    ring.add(cpm::LogLevel::Warn, "2026-10-29T05:00:00.000Z", "the feed went quiet");

    Client stream;
    ASSERT_TRUE(stream.connect(server->port()));
    ASSERT_TRUE(stream.write("GET /api/events?t=" + server->token() +
                             " HTTP/1.1\r\nHost: localhost\r\n\r\n"));
    stream.drain_headers();
    ASSERT_NE(stream.event().find("event: snapshot"), std::string::npos);

    const auto first = stream.event();
    ASSERT_NE(first.find("event: log"), std::string::npos) << first;
    const auto parsed = cpm::gui::json_parse(first.substr(first.find("data: ") + 6));
    ASSERT_TRUE(parsed.ok()) << parsed.error;
    const auto* lines = parsed.value.find("lines");
    ASSERT_NE(lines, nullptr);
    ASSERT_EQ(lines->items.size(), 1u);
    EXPECT_EQ(lines->items[0].find("text")->text, "the feed went quiet");
    EXPECT_EQ(lines->items[0].find("level")->text, "WARN");
    EXPECT_EQ(lines->items[0].find("at")->text, "2026-10-29T05:00:00.000Z");

    // The next event carries the new line and not the old one: a page that
    // has been watching all along is sent each line once.
    ring.add(cpm::LogLevel::Error, "2026-10-29T05:00:01.000Z", "order entry rejected");
    const auto second = stream.event();
    ASSERT_NE(second.find("event: log"), std::string::npos) << second;
    const auto again = cpm::gui::json_parse(second.substr(second.find("data: ") + 6));
    ASSERT_TRUE(again.ok()) << again.error;
    ASSERT_EQ(again.value.find("lines")->items.size(), 1u);
    EXPECT_EQ(again.value.find("lines")->items[0].find("text")->text, "order entry rejected");

    stream.disconnect();
}

TEST_F(ServerTest, TheStreamSendsASnapshotFirstAndThenOnEveryChange) {
    Client stream;
    ASSERT_TRUE(stream.connect(server->port()));
    ASSERT_TRUE(stream.write("GET /api/events?t=" + server->token() +
                             " HTTP/1.1\r\nHost: localhost\r\n\r\n"));
    const auto head = stream.drain_headers();
    EXPECT_NE(head.find("200 OK"), std::string::npos);
    EXPECT_NE(head.find("text/event-stream"), std::string::npos);

    const auto first = stream.event();
    ASSERT_NE(first.find("event: snapshot"), std::string::npos) << first;
    // The reconnect delay travels with the first event, so a browser that
    // loses the stream waits this daemon's two seconds rather than its own
    // default (G5).
    EXPECT_NE(first.find("retry: 2000"), std::string::npos) << first;
    const auto first_data = first.substr(first.find("data: ") + 6);
    const auto parsed = cpm::gui::json_parse(first_data);
    ASSERT_TRUE(parsed.ok()) << parsed.error << " in " << first_data;
    const auto first_revision = parsed.value.find("revision")->text;

    // Anything that changes the daemon pushes the next one, with no polling
    // and no request from the page.
    ASSERT_EQ(status_of(authed("POST", "/api/strategies",
                               R"({"kind":"CryptoMM","params":{"symbol":")" + kSym +
                                   R"(","qty":"1"}})")),
              200);

    const auto second = stream.event();
    ASSERT_NE(second.find("event: snapshot"), std::string::npos) << second;
    const auto second_data = second.substr(second.find("data: ") + 6);
    const auto next = cpm::gui::json_parse(second_data);
    ASSERT_TRUE(next.ok()) << next.error;
    EXPECT_NE(next.value.find("revision")->text, first_revision);
    EXPECT_EQ(next.value.find("strategies")->items.size(), 1u);
}

TEST(ServerStandalone, AQuietStreamStillSaysSomething) {
    // Nothing happening in the market must not look the same as a tunnel that
    // has died, at either end. The heartbeat is a comment, so the page never
    // has to know it exists.
    Daemon daemon;
    Server::Options options;
    options.port = 0;
    options.heartbeat = std::chrono::seconds(0);  // every pass, rather than every 15 s
    Server server(*daemon.api, options, daemon.logger);
    ASSERT_EQ(server.start(), "");

    Client stream;
    ASSERT_TRUE(stream.connect(server.port()));
    ASSERT_TRUE(stream.write("GET /api/events?t=" + server.token() +
                             " HTTP/1.1\r\nHost: localhost\r\n\r\n"));
    ASSERT_FALSE(stream.drain_headers().empty());
    EXPECT_NE(stream.event().find("event: snapshot"), std::string::npos);
    // Nothing has changed, so the next thing through is a heartbeat - a named
    // event, not a comment, because the page has to be able to see it.
    const auto beat = stream.event();
    EXPECT_NE(beat.find("event: heartbeat"), std::string::npos) << beat;
    EXPECT_NE(beat.find("\"revision\":"), std::string::npos) << beat;
    server.stop();
}

TEST_F(ServerTest, TheStreamNeedsTheTokenToo) {
    Client stream;
    ASSERT_TRUE(stream.connect(server->port()));
    ASSERT_TRUE(stream.write("GET /api/events HTTP/1.1\r\nHost: localhost\r\n\r\n"));
    EXPECT_EQ(status_of(stream.response()), 401);
}

TEST_F(ServerTest, AnsweresSeveralRequestsOnOneConnection) {
    // A browser reuses its connection; a server that answered only the first
    // request would look like an intermittent one.
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(status_of(authed("GET", "/api/snapshot")), 200) << "request " << i;
    }
}

TEST_F(ServerTest, RefusesABodyTooLargeToBeAnything) {
    const std::string huge(200'000, 'x');
    const auto response = authed("POST", "/api/strategies", huge);
    EXPECT_EQ(status_of(response), 413);
}

TEST(ServerStandalone, LetsGoOfAConnectionNobodyIsUsing) {
    // A browser keeps spare connections open, and something that opens one and
    // then says nothing at all is how the 16 slots would otherwise be spent.
    Daemon daemon;
    Server::Options options;
    options.port = 0;
    options.idle_timeout = std::chrono::seconds(1);
    Server server(*daemon.api, options, daemon.logger);
    ASSERT_EQ(server.start(), "");

    Client silent;
    ASSERT_TRUE(silent.connect(server.port()));
    EXPECT_TRUE(silent.closed_within(4000)) << "an idle connection was held forever";

    // And it says so in advance, so the browser retires its end first rather
    // than losing a POST to the race.
    Client talker;
    ASSERT_TRUE(talker.connect(server.port()));
    ASSERT_TRUE(talker.write("GET /api/snapshot?t=" + server.token() +
                             " HTTP/1.1\r\nHost: localhost\r\n\r\n"));
    EXPECT_NE(talker.response().find("Keep-Alive: timeout=1"), std::string::npos);
    server.stop();
}

TEST(ServerStandalone, DoesNotReapAStreamForSittingThere) {
    // Sitting there is what a stream is for: an operator watching a quiet
    // market must not be disconnected for watching it.
    Daemon daemon;
    Server::Options options;
    options.port = 0;
    options.idle_timeout = std::chrono::seconds(1);
    Server server(*daemon.api, options, daemon.logger);
    ASSERT_EQ(server.start(), "");

    Client stream;
    ASSERT_TRUE(stream.connect(server.port()));
    ASSERT_TRUE(stream.write("GET /api/events?t=" + server.token() +
                             " HTTP/1.1\r\nHost: localhost\r\n\r\n"));
    ASSERT_FALSE(stream.drain_headers().empty());
    EXPECT_FALSE(stream.closed_within(3000)) << "a stream was reaped as idle";
    server.stop();
}

TEST(ServerStandalone, NamesThePortWhenItIsAlreadyTaken) {
    Daemon daemon;
    Server::Options options;
    options.port = 0;
    Server first(*daemon.api, options, daemon.logger);
    ASSERT_EQ(first.start(), "");

    Server::Options second_options;
    second_options.port = first.port();
    Server second(*daemon.api, second_options, daemon.logger);
    const auto error = second.start();
    EXPECT_NE(error.find("already in use"), std::string::npos) << error;
    EXPECT_NE(error.find(std::to_string(first.port())), std::string::npos) << error;
    first.stop();
}

TEST(ServerStandalone, EveryRunGetsItsOwnToken) {
    const auto first = cpm::gui::make_token();
    const auto second = cpm::gui::make_token();
    EXPECT_EQ(first.size(), 16u);
    EXPECT_NE(first, second);
    EXPECT_EQ(first.find_first_not_of("0123456789abcdef"), std::string::npos);
}

TEST(ServerStandalone, TheBannerSaysHowToReachItAndWhetherItCanTrade) {
    Daemon daemon;
    Server::Options options;
    options.port = 0;
    Server server(*daemon.api, options, daemon.logger);
    ASSERT_EQ(server.start(), "");

    const auto observer = server.banner(false);
    ASSERT_EQ(observer.size(), 4u);
    EXPECT_NE(observer[0].find("127.0.0.1:" + std::to_string(server.port())), std::string::npos);
    EXPECT_NE(observer[1].find(server.token()), std::string::npos);
    EXPECT_NE(observer[2].find("ssh -L"), std::string::npos);
    EXPECT_NE(observer[3].find("--allow-orders"), std::string::npos);

    EXPECT_NE(server.banner(true)[3].find("PERMITTED"), std::string::npos);
    server.stop();
}

TEST(ServerStandalone, StoppingIsIdempotentAndClosesTheStreams) {
    Daemon daemon;
    Server::Options options;
    options.port = 0;
    auto server = std::make_unique<Server>(*daemon.api, options, daemon.logger);
    ASSERT_EQ(server->start(), "");
    const int port = server->port();

    Client stream;
    ASSERT_TRUE(stream.connect(port));
    ASSERT_TRUE(stream.write("GET /api/events?t=" + server->token() +
                             " HTTP/1.1\r\nHost: localhost\r\n\r\n"));
    ASSERT_FALSE(stream.drain_headers().empty());

    server->stop();
    server->stop();  // the shutdown path runs twice if a signal and a quit race
    server.reset();

    Client after;
    EXPECT_FALSE(after.connect(port));
}

}  // namespace
