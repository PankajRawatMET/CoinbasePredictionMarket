// The JSON both ways, with no socket in sight. The rule this file exists to
// hold: a scaled integer leaves as a string, and a number arrives as the text
// that was sent - nothing on either path goes through a double.
#include "cpm/common/json.hpp"

#include <gtest/gtest.h>

namespace {

using cpm::JsonParse;
using cpm::JsonValue;
using cpm::JsonWriter;

TEST(JsonWriter, WritesAnObjectWithCommasInTheRightPlaces) {
    JsonWriter out;
    out.object_begin();
    out.field("a", "one");
    out.field("b", 2);
    out.field("c", true);
    out.object_end();
    EXPECT_EQ(out.str(), R"({"a":"one","b":2,"c":true})");
}

TEST(JsonWriter, NestsObjectsAndArrays) {
    JsonWriter out;
    out.object_begin();
    out.key("items").array_begin();
    out.object_begin();
    out.field("id", "s1");
    out.object_end();
    out.object_begin();
    out.field("id", "s2");
    out.object_end();
    out.array_end();
    out.key("empty").array_begin();
    out.array_end();
    out.object_end();
    EXPECT_EQ(out.str(), R"({"items":[{"id":"s1"},{"id":"s2"}],"empty":[]})");
}

TEST(JsonWriter, ScaledValuesAreStringsAndMissingOnesAreNull) {
    JsonWriter out;
    out.object_begin();
    out.field_price("bid", std::optional<cpm::Price>(580));
    out.field_price("ask", std::nullopt);
    // Zero is a real bid and must not render like an absent one.
    out.field_price("floor", std::optional<cpm::Price>(0));
    out.field_qty("qty", std::optional<cpm::Qty>(3072));
    out.field_scaled("fees", -49500, cpm::kAmtScale);
    out.object_end();
    EXPECT_EQ(out.str(),
              R"({"bid":"0.58","ask":null,"floor":"0","qty":"30.72","fees":"-0.0495"})");
}

TEST(JsonWriter, EscapesWhatWouldBreakTheDocument) {
    JsonWriter out;
    out.object_begin();
    out.field("why", "he said \"stop\"\n\tnow\\then");
    // Split, because "a\x01b" would be one hex escape of 0x1b and not what it reads as.
    out.field("control", std::string_view("a\x01" "b", 3));
    out.object_end();
    EXPECT_EQ(out.str(),
              R"({"why":"he said \"stop\"\n\tnow\\then","control":"a\u0001b"})");
}

TEST(JsonWriter, ABareBoolIsNotMistakenForAString) {
    JsonWriter out;
    out.object_begin();
    out.field("armed", false);
    out.field("note", "false");
    out.field("count", std::size_t{7});
    out.object_end();
    EXPECT_EQ(out.str(), R"({"armed":false,"note":"false","count":7})");
}

TEST(JsonParse, ReadsTheCreateRequest) {
    const auto parsed = cpm::json_parse(
        R"({"kind":"CryptoMM","params":{"symbol":"SIM-HOM","qty":"2","width-ticks":3}})");
    ASSERT_TRUE(parsed.ok()) << parsed.error;
    const auto* kind = parsed.value.find("kind");
    ASSERT_NE(kind, nullptr);
    EXPECT_EQ(kind->text, "CryptoMM");
    const auto* params = parsed.value.find("params");
    ASSERT_NE(params, nullptr);
    ASSERT_TRUE(params->is_object());
    ASSERT_NE(params->find("symbol"), nullptr);
    EXPECT_EQ(params->find("symbol")->text, "SIM-HOM");
    // A number arrives as the digits it was written with, not as a double.
    ASSERT_NE(params->find("width-ticks"), nullptr);
    EXPECT_EQ(params->find("width-ticks")->type, JsonValue::Type::Number);
    EXPECT_EQ(params->find("width-ticks")->text, "3");
}

TEST(JsonParse, KeepsADecimalExactly) {
    const auto parsed = cpm::json_parse(R"({"price":0.5805})");
    ASSERT_TRUE(parsed.ok()) << parsed.error;
    EXPECT_EQ(parsed.value.find("price")->text, "0.5805");
}

TEST(JsonParse, ReadsEscapesIncludingSurrogatePairs) {
    const auto parsed = cpm::json_parse(R"({"a":"line\nbreak","b":"\u00e9","c":"\ud83d\ude00"})");
    ASSERT_TRUE(parsed.ok()) << parsed.error;
    EXPECT_EQ(parsed.value.find("a")->text, "line\nbreak");
    EXPECT_EQ(parsed.value.find("b")->text, "\xc3\xa9");
    EXPECT_EQ(parsed.value.find("c")->text, "\xf0\x9f\x98\x80");
}

TEST(JsonParse, RefusesWhatIsNotJson) {
    const char* bad[] = {
        "",
        "{",
        "{\"a\":}",
        "{\"a\":1,}",
        "{'a':1}",
        "{\"a\":01}",
        "{\"a\":1} trailing",
        "{\"a\":\"unterminated",
        "[1,2",
        "{\"a\":\"\\q\"}",
        "{\"a\":\"\\ud83d\"}",
    };
    for (const char* text : bad) {
        const auto parsed = cpm::json_parse(text);
        EXPECT_FALSE(parsed.ok()) << "accepted: " << text;
    }
}

TEST(JsonParse, RefusesSomethingNestedDeepEnoughToWalkOffTheStack) {
    std::string deep(200, '[');
    deep += std::string(200, ']');
    const auto parsed = cpm::json_parse(deep);
    EXPECT_FALSE(parsed.ok());
    EXPECT_NE(parsed.error.find("nested too deeply"), std::string::npos) << parsed.error;
}

TEST(JsonParse, FindOnANonObjectIsNull) {
    const auto parsed = cpm::json_parse(R"(["a","b"])");
    ASSERT_TRUE(parsed.ok()) << parsed.error;
    EXPECT_EQ(parsed.value.find("a"), nullptr);
}

}  // namespace
