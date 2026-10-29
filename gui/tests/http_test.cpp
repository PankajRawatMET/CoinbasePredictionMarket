// The HTTP parser as a pure function: a request split across reads, a header
// that never ends, a body nobody should send. All the cases that are painful
// to arrange over a real socket, arranged here instead.
#include "cpm/gui/http.hpp"

#include <gtest/gtest.h>

namespace {

using cpm::gui::HttpLimits;
using cpm::gui::HttpParse;
using cpm::gui::HttpResponse;
using cpm::gui::parse_request;

TEST(Http, ParsesAGetWithAQuery) {
    const auto parsed = parse_request("GET /api/events?t=abc123&x=1 HTTP/1.1\r\nHost: localhost:8080\r\n\r\n");
    ASSERT_EQ(parsed.state, HttpParse::State::Complete);
    EXPECT_EQ(parsed.request.method, "GET");
    EXPECT_EQ(parsed.request.path, "/api/events");
    EXPECT_EQ(parsed.request.query, "t=abc123&x=1");
    ASSERT_TRUE(parsed.request.query_param("t").has_value());
    EXPECT_EQ(*parsed.request.query_param("t"), "abc123");
    EXPECT_FALSE(parsed.request.query_param("nope").has_value());
    ASSERT_NE(parsed.request.header("HOST"), nullptr);  // the name's case is the browser's business
    EXPECT_EQ(*parsed.request.header("HOST"), "localhost:8080");
    EXPECT_TRUE(parsed.request.keep_alive());
}

TEST(Http, WaitsForTheRestOfARequestThatArrivedInPieces) {
    const std::string whole =
        "POST /api/strategies HTTP/1.1\r\nContent-Length: 10\r\n\r\n{\"kind\":1}";
    for (std::size_t cut = 1; cut < whole.size(); ++cut) {
        const auto partial = parse_request(std::string_view(whole).substr(0, cut));
        EXPECT_EQ(partial.state, HttpParse::State::Incomplete) << "cut at " << cut;
    }
    const auto parsed = parse_request(whole);
    ASSERT_EQ(parsed.state, HttpParse::State::Complete);
    EXPECT_EQ(parsed.request.body, "{\"kind\":1}");
    EXPECT_EQ(parsed.consumed, whole.size());
}

TEST(Http, ReportsHowMuchOfTheBufferOneRequestUsed) {
    const std::string two =
        "GET /api/snapshot HTTP/1.1\r\n\r\nGET /api/schema HTTP/1.1\r\n\r\n";
    const auto first = parse_request(two);
    ASSERT_EQ(first.state, HttpParse::State::Complete);
    const auto second = parse_request(std::string_view(two).substr(first.consumed));
    ASSERT_EQ(second.state, HttpParse::State::Complete);
    EXPECT_EQ(second.request.path, "/api/schema");
}

TEST(Http, DecodesPercentEscapesInThePath) {
    const auto parsed = parse_request("POST /api/strategies/s%31/start HTTP/1.1\r\n\r\n");
    ASSERT_EQ(parsed.state, HttpParse::State::Complete);
    EXPECT_EQ(parsed.request.path, "/api/strategies/s1/start");
}

TEST(Http, APlusIsASpaceInAQueryAndAPlusInAPath) {
    const auto parsed = parse_request("GET /a+b?t=x+y HTTP/1.1\r\n\r\n");
    ASSERT_EQ(parsed.state, HttpParse::State::Complete);
    EXPECT_EQ(parsed.request.path, "/a+b");
    EXPECT_EQ(*parsed.request.query_param("t"), "x y");
}

TEST(Http, RefusesTheFramingsItDoesNotImplement) {
    const auto chunked =
        parse_request("POST /api/strategies HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n");
    EXPECT_EQ(chunked.state, HttpParse::State::Bad);
    EXPECT_EQ(chunked.error_status, 411);

    const auto version = parse_request("GET / HTTP/2.0\r\n\r\n");
    EXPECT_EQ(version.state, HttpParse::State::Bad);
    EXPECT_EQ(version.error_status, 505);

    const auto line = parse_request("GET\r\n\r\n");
    EXPECT_EQ(line.state, HttpParse::State::Bad);
    EXPECT_EQ(line.error_status, 400);

    const auto header = parse_request("GET / HTTP/1.1\r\nnot-a-header\r\n\r\n");
    EXPECT_EQ(header.state, HttpParse::State::Bad);
    EXPECT_EQ(header.error_status, 400);

    const auto length = parse_request("POST / HTTP/1.1\r\nContent-Length: ten\r\n\r\n");
    EXPECT_EQ(length.state, HttpParse::State::Bad);
    EXPECT_EQ(length.error_status, 400);
}

TEST(Http, RefusesWhatIsTooLargeRatherThanBufferingIt) {
    HttpLimits limits;
    limits.max_head = 128;
    limits.max_body = 16;

    const std::string long_header =
        "GET / HTTP/1.1\r\nX-Pad: " + std::string(300, 'a') + "\r\n\r\n";
    const auto head = parse_request(long_header, limits);
    EXPECT_EQ(head.state, HttpParse::State::Bad);
    EXPECT_EQ(head.error_status, 431);

    // Over the limit before the head has even ended: still refused, rather
    // than held in memory while a sender keeps typing.
    const auto endless = parse_request(std::string(300, 'a'), limits);
    EXPECT_EQ(endless.state, HttpParse::State::Bad);
    EXPECT_EQ(endless.error_status, 431);

    const auto body = parse_request("POST / HTTP/1.1\r\nContent-Length: 64\r\n\r\n", limits);
    EXPECT_EQ(body.state, HttpParse::State::Bad);
    EXPECT_EQ(body.error_status, 413);
}

TEST(Http, HonoursConnectionClose) {
    const auto eleven = parse_request("GET / HTTP/1.1\r\nConnection: Close\r\n\r\n");
    ASSERT_EQ(eleven.state, HttpParse::State::Complete);
    EXPECT_FALSE(eleven.request.keep_alive());

    const auto ten = parse_request("GET / HTTP/1.0\r\n\r\n");
    ASSERT_EQ(ten.state, HttpParse::State::Complete);
    EXPECT_FALSE(ten.request.keep_alive());

    const auto ten_keeps = parse_request("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n");
    ASSERT_EQ(ten_keeps.state, HttpParse::State::Complete);
    EXPECT_TRUE(ten_keeps.request.keep_alive());
}

TEST(Http, RendersAResponseWithALengthAndTheSafetyHeaders) {
    HttpResponse response;
    response.status = 409;
    response.body = R"({"ok":false})";
    response.headers.emplace_back("X-Test", "1");
    const auto text = cpm::gui::render_response(response);
    EXPECT_NE(text.find("HTTP/1.1 409 Conflict\r\n"), std::string::npos);
    EXPECT_NE(text.find("Content-Length: 12\r\n"), std::string::npos);
    EXPECT_NE(text.find("Cache-Control: no-store\r\n"), std::string::npos);
    EXPECT_NE(text.find("X-Content-Type-Options: nosniff\r\n"), std::string::npos);
    // The token rides in the URL on the first load: no address of this page
    // may leave with a request to anywhere else.
    EXPECT_NE(text.find("Referrer-Policy: no-referrer\r\n"), std::string::npos);
    EXPECT_NE(text.find("X-Test: 1\r\n"), std::string::npos);
    EXPECT_NE(text.find("\r\n\r\n" + response.body), std::string::npos);
}

TEST(Http, SecretComparisonIsLengthSafe) {
    EXPECT_TRUE(cpm::gui::secret_equal("abc123", "abc123"));
    EXPECT_FALSE(cpm::gui::secret_equal("abc123", "abc124"));
    EXPECT_FALSE(cpm::gui::secret_equal("abc123", "abc12"));
    EXPECT_FALSE(cpm::gui::secret_equal("", "x"));
}

}  // namespace
