// The WebSocket client (B1), against a server that speaks the protocol from
// the other side rather than against a recording. Everything here is on
// loopback with no TLS: the frames are the same either way, and a test that
// needed a key pair is a test nobody runs.
#include "cpm/common/websocket.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using cpm::ws::Client;
using cpm::ws::Frame;
using cpm::ws::Opcode;
using cpm::ws::Parse;
using cpm::ws::Server;

// Pumps both ends until `done` or the deadline: a client's pong only reaches
// the server if someone reads for it.
template <typename Done>
bool together(Client& client, Server& server, const Done& done,
              const std::function<void(std::string_view)>& on_text = {},
              std::chrono::milliseconds limit = 3s) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        client.pump(20ms, on_text);
        server.pump(20ms);
        if (done()) return true;
    }
    return done();
}

struct Pair {
    Server server;
    Client client;
    std::vector<std::string> received;

    // Starts a server, connects a client and completes the handshake. The
    // accept has to happen while the client is connecting, so it runs on a
    // thread of its own.
    void connect() {
        ASSERT_EQ(server.start(0), "");
        std::string accept_error;
        bool accepted = false;
        std::thread accepting([&] { accepted = server.accept(3s, accept_error); });

        Client::Options options;
        options.host = "127.0.0.1";
        options.port = server.port();
        options.path = "/ws/btcusdt@bookTicker";
        options.tls = false;
        const auto failed = client.connect(options);
        accepting.join();
        ASSERT_EQ(failed, "");
        ASSERT_TRUE(accepted) << accept_error;
    }

    std::function<void(std::string_view)> sink() {
        return [this](std::string_view text) { received.emplace_back(text); };
    }
};

// ---- the codec, both ways -------------------------------------------------

TEST(WebSocketFrame, RoundTripsThroughItself) {
    for (const std::string& payload : {std::string(""), std::string("hello"),
                                      std::string(200, 'x'),      // 16-bit length
                                      std::string(70000, 'y')}) { // 64-bit length
        for (const bool masked : {false, true}) {
            auto wire = cpm::ws::encode_frame(Opcode::Text, payload, masked);
            Frame frame;
            std::string error;
            ASSERT_EQ(cpm::ws::parse_frame(wire, frame, error), Parse::Ok)
                << error << " for " << payload.size() << " bytes, masked=" << masked;
            EXPECT_EQ(frame.payload, payload);
            EXPECT_EQ(frame.opcode, Opcode::Text);
            EXPECT_TRUE(frame.fin);
            EXPECT_TRUE(wire.empty()) << "the frame was not consumed";
        }
    }
}

TEST(WebSocketFrame, AsksForMoreRatherThanGuessing) {
    const auto whole = cpm::ws::encode_frame(Opcode::Text, std::string(300, 'z'), true);
    Frame frame;
    std::string error;
    // Every prefix short of the whole frame must say "not yet", not "bad":
    // a socket delivers what it likes, when it likes.
    for (std::size_t cut = 0; cut < whole.size(); ++cut) {
        std::string partial = whole.substr(0, cut);
        EXPECT_EQ(cpm::ws::parse_frame(partial, frame, error), Parse::NeedMore)
            << "at " << cut << " of " << whole.size();
    }
    std::string complete = whole;
    EXPECT_EQ(cpm::ws::parse_frame(complete, frame, error), Parse::Ok);
}

TEST(WebSocketFrame, ClientFramesAreMaskedAndServerFramesAreNot) {
    const std::string payload = "mask me";
    const auto from_client = cpm::ws::encode_frame(Opcode::Text, payload, true);
    const auto from_server = cpm::ws::encode_frame(Opcode::Text, payload, false);
    // The RFC requires a client to mask, and Binance closes a connection
    // that forgets. The bit is the second byte's top.
    EXPECT_NE(static_cast<unsigned char>(from_client[1]) & 0x80, 0);
    EXPECT_EQ(static_cast<unsigned char>(from_server[1]) & 0x80, 0);
    // Masked bytes are not the payload sitting in the clear.
    EXPECT_EQ(from_client.find(payload), std::string::npos);
    EXPECT_NE(from_server.find(payload), std::string::npos);
}

TEST(WebSocketFrame, AcceptKeyIsTheOneFromTheRfc) {
    // RFC 6455 section 1.3's worked example, which is the whole point of
    // the check: a server that answers something else is not a WebSocket.
    EXPECT_EQ(cpm::ws::accept_key("dGhlIHNhbXBsZSBub25jZQ=="), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

TEST(WebSocketFrame, EveryKeyIsItsOwn) {
    const auto one = cpm::ws::make_client_key();
    const auto two = cpm::ws::make_client_key();
    EXPECT_EQ(one.size(), 24u);  // 16 bytes, base64
    EXPECT_NE(one, two);
}

// ---- the client against a real socket -------------------------------------

TEST(WebSocketClient, ShakesHandsAndCarriesMessages) {
    Pair pair;
    pair.connect();
    if (::testing::Test::HasFatalFailure()) return;

    ASSERT_EQ(pair.server.send_text(R"({"u":1,"s":"BTCUSDT","b":"1.00","a":"2.00"})"), "");
    ASSERT_TRUE(together(pair.client, pair.server,
                         [&] { return !pair.received.empty(); }, pair.sink()));
    EXPECT_EQ(pair.received.front(), R"({"u":1,"s":"BTCUSDT","b":"1.00","a":"2.00"})");
    EXPECT_TRUE(pair.client.connected());
}

TEST(WebSocketClient, AnswersAPingWithItsOwnPayload) {
    Pair pair;
    pair.connect();
    if (::testing::Test::HasFatalFailure()) return;

    // Binance pings every 20 s and drops a client that has not answered
    // within a minute, so this is what keeps the feed up for a day.
    ASSERT_EQ(pair.server.send_ping("keepalive"), "");
    EXPECT_TRUE(together(pair.client, pair.server, [&] { return pair.server.saw_pong(); },
                         pair.sink()));
    EXPECT_GE(pair.server.pongs(), 1);
    EXPECT_TRUE(pair.client.connected());
}

TEST(WebSocketClient, PutsAFragmentedMessageBackTogether) {
    Pair pair;
    pair.connect();
    if (::testing::Test::HasFatalFailure()) return;

    // A text frame with FIN clear, then a continuation that ends it.
    // Binance does not fragment today; the RFC allows it, and a client that
    // broke on it would break in the middle of the night.
    std::string first = cpm::ws::encode_frame(Opcode::Text, R"({"b":"1.)", false);
    first[0] = static_cast<char>(static_cast<unsigned char>(first[0]) & 0x7F);  // FIN off
    const auto rest = cpm::ws::encode_frame(Opcode::Continuation, R"(00"})", false);

    ASSERT_EQ(pair.server.send_raw(first), "");
    ASSERT_EQ(pair.server.send_raw(rest), "");
    ASSERT_TRUE(together(pair.client, pair.server, [&] { return !pair.received.empty(); },
                         pair.sink()));
    ASSERT_EQ(pair.received.size(), 1u) << "a fragmented message is one message";
    EXPECT_EQ(pair.received.front(), R"({"b":"1.00"})");
}

TEST(WebSocketClient, AnswersAPingThatArrivesMidMessage) {
    Pair pair;
    pair.connect();
    if (::testing::Test::HasFatalFailure()) return;

    // A control frame may be interleaved between fragments, and answering
    // it must not corrupt the message being assembled.
    std::string first = cpm::ws::encode_frame(Opcode::Text, "half ", false);
    first[0] = static_cast<char>(static_cast<unsigned char>(first[0]) & 0x7F);
    ASSERT_EQ(pair.server.send_raw(first), "");
    ASSERT_EQ(pair.server.send_ping("mid"), "");
    ASSERT_EQ(pair.server.send_raw(cpm::ws::encode_frame(Opcode::Continuation, "a message",
                                                         false)), "");
    ASSERT_TRUE(together(pair.client, pair.server,
                         [&] { return !pair.received.empty() && pair.server.saw_pong(); },
                         pair.sink()));
    EXPECT_EQ(pair.received.front(), "half a message");
    EXPECT_GE(pair.server.pongs(), 1);
}

TEST(WebSocketClient, DeliversAMessageSplitAcrossReads) {
    Pair pair;
    pair.connect();
    if (::testing::Test::HasFatalFailure()) return;

    // 300 KB forces the 16- and 64-bit length paths and several recv()s, so
    // the client's buffer has to hold a partial frame across pumps.
    const std::string big(300u * 1024, 'q');
    ASSERT_EQ(pair.server.send_text(big), "");
    ASSERT_TRUE(together(pair.client, pair.server, [&] { return !pair.received.empty(); },
                         pair.sink(), 5s));
    ASSERT_EQ(pair.received.size(), 1u);
    EXPECT_EQ(pair.received.front().size(), big.size());
    EXPECT_EQ(pair.received.front(), big);
}

TEST(WebSocketClient, NoticesACloseFrame) {
    Pair pair;
    pair.connect();
    if (::testing::Test::HasFatalFailure()) return;

    ASSERT_EQ(pair.server.send_close(), "");
    EXPECT_TRUE(together(pair.client, pair.server, [&] { return !pair.client.connected(); },
                         pair.sink()));
    EXPECT_FALSE(pair.client.connected());
    EXPECT_NE(pair.client.error(), "");
}

TEST(WebSocketClient, NoticesADroppedConnection) {
    Pair pair;
    pair.connect();
    if (::testing::Test::HasFatalFailure()) return;

    // No close frame, just gone - a 24-hour disconnect, or a tunnel dying.
    pair.server.drop();
    EXPECT_TRUE(together(pair.client, pair.server, [&] { return !pair.client.connected(); },
                         pair.sink()));
    EXPECT_FALSE(pair.client.connected());
}

TEST(WebSocketClient, RefusesAServerThatIsNotOne) {
    // A plain HTTP server on the port: the upgrade must be refused rather
    // than its body read as frames.
    Server server;
    ASSERT_EQ(server.start(0), "");
    std::thread rude([&] {
        std::string ignored;
        // accept() does the WebSocket handshake, so instead just let the
        // connection be refused by never accepting it at all.
        (void)ignored;
    });

    Client::Options options;
    options.host = "127.0.0.1";
    options.port = server.port();
    options.tls = false;
    options.handshake_timeout = 500ms;
    Client client;
    const auto failed = client.connect(options);
    rude.join();
    EXPECT_NE(failed, "") << "a server that never answers must not look connected";
    EXPECT_FALSE(client.connected());
}

TEST(WebSocketClient, RefusesAConnectionThatGoesNowhere) {
    Client::Options options;
    options.host = "127.0.0.1";
    options.port = 1;  // nothing listens on port 1
    options.tls = false;
    options.handshake_timeout = 500ms;
    Client client;
    EXPECT_NE(client.connect(options), "");
    EXPECT_FALSE(client.connected());
}

}  // namespace
