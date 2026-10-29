// The parameter schema: one description of a strategy's tunables that both the
// GUI form and the command line validate against.
#include "cpm/strategy/params.hpp"

#include <gtest/gtest.h>

namespace {

using cpm::strategy::ParamSchema;
using cpm::strategy::ParamSpec;
using cpm::strategy::ParamValues;
using cpm::strategy::param_decimal;
using cpm::strategy::param_int;
using cpm::strategy::param_text;
using cpm::strategy::validate;

ParamSchema schema() {
    ParamSchema out;

    ParamSpec symbol;
    symbol.name = "symbol";
    symbol.type = ParamSpec::Type::Text;
    out.push_back(symbol);

    ParamSpec width;
    width.name = "width-ticks";
    width.type = ParamSpec::Type::Int;
    width.default_value = "2";
    width.min = "1";
    width.max = "500";
    out.push_back(width);

    ParamSpec qty;
    qty.name = "qty";
    qty.type = ParamSpec::Type::Decimal;
    qty.scale = 2;
    qty.default_value = "1";
    qty.min = "0.01";
    qty.max = "10000";
    out.push_back(qty);

    ParamSpec ticks;
    ticks.name = "ticks";
    ticks.type = ParamSpec::Type::Enum;
    ticks.default_value = "tapered";
    ticks.choices = {"tapered", "cent", "deci"};
    out.push_back(ticks);

    return out;
}

TEST(Params, FillsInEveryDefault) {
    const auto result = validate(schema(), {{"symbol", "BTCD15M-X"}});
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.values.at("symbol"), "BTCD15M-X");
    EXPECT_EQ(result.values.at("width-ticks"), "2");
    EXPECT_EQ(result.values.at("qty"), "1");
    EXPECT_EQ(result.values.at("ticks"), "tapered");
}

TEST(Params, ARequiredValueHasNoDefaultToFallBackOn) {
    const auto result = validate(schema(), {{"width-ticks", "3"}});
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.error, "symbol is required");
}

TEST(Params, AnUnknownNameIsRefusedRatherThanIgnored) {
    // A typo that silently quoted at the default width is the quiet wrong
    // answer this check exists to prevent.
    const auto result = validate(schema(), {{"symbol", "X"}, {"with-ticks", "3"}});
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.error, "unknown parameter with-ticks");
}

TEST(Params, BoundsAreInclusiveAndReportedInTheOperatorsUnits) {
    EXPECT_TRUE(validate(schema(), {{"symbol", "X"}, {"width-ticks", "1"}}).ok());
    EXPECT_TRUE(validate(schema(), {{"symbol", "X"}, {"width-ticks", "500"}}).ok());

    const auto low = validate(schema(), {{"symbol", "X"}, {"width-ticks", "0"}});
    EXPECT_FALSE(low.ok());
    EXPECT_EQ(low.error, "width-ticks is too small (1 to 500)");

    const auto high = validate(schema(), {{"symbol", "X"}, {"width-ticks", "501"}});
    EXPECT_FALSE(high.ok());
    EXPECT_EQ(high.error, "width-ticks is too large (1 to 500)");
}

TEST(Params, IntegersMustBeIntegers) {
    for (const char* bad : {"", "2.5", "two", "2 ", " 2", "0x2", "2e3", "--2"}) {
        const auto result = validate(schema(), {{"symbol", "X"}, {"width-ticks", bad}});
        EXPECT_FALSE(result.ok()) << "accepted width-ticks=\"" << bad << "\"";
    }
    EXPECT_TRUE(validate(schema(), {{"symbol", "X"}, {"width-ticks", "+3"}}).ok());
}

TEST(Params, AnIntegerThatWouldOverflowIsRefusedRatherThanWrapped) {
    const auto result = validate(schema(), {{"symbol", "X"}, {"width-ticks", "99999999999999999999"}});
    EXPECT_FALSE(result.ok());
}

TEST(Params, DecimalsRespectTheirScale) {
    EXPECT_TRUE(validate(schema(), {{"symbol", "X"}, {"qty", "2.5"}}).ok());
    EXPECT_TRUE(validate(schema(), {{"symbol", "X"}, {"qty", "2.55"}}).ok());
    // Three places at a scale of two: refused rather than silently truncated.
    const auto result = validate(schema(), {{"symbol", "X"}, {"qty", "2.555"}});
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.error, "qty must be a decimal with at most 2 places");
}

TEST(Params, DecimalBoundsAreComparedInScaledUnits) {
    // 0.01 is the minimum, so 0.009 would be below it - and is also too
    // precise, which is caught first. 0 is below the bound outright.
    const auto result = validate(schema(), {{"symbol", "X"}, {"qty", "0"}});
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.error, "qty is too small (0.01 to 10000)");
}

TEST(Params, ANegativeDecimalSaysSoRatherThanBlamingItsPrecision) {
    const auto result = validate(schema(), {{"symbol", "X"}, {"qty", "-1"}});
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.error, "qty must not be negative (0.01 to 10000)");
}

TEST(Params, AnEnumListsWhatItWouldHaveAccepted) {
    EXPECT_TRUE(validate(schema(), {{"symbol", "X"}, {"ticks", "cent"}}).ok());
    const auto result = validate(schema(), {{"symbol", "X"}, {"ticks", "Tapered"}});
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.error, "ticks must be one of: tapered, cent, deci");
}

TEST(Params, TextMustNotBeEmpty) {
    const auto result = validate(schema(), {{"symbol", ""}});
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.error, "symbol must not be empty");
}

TEST(Params, TheTypedReadersReturnScaledIntegers) {
    const auto result = validate(schema(), {{"symbol", "BTCD15M-X"}, {"qty", "2.5"}});
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(param_text(result.values, "symbol"), "BTCD15M-X");
    EXPECT_EQ(param_int(result.values, "width-ticks"), 2);
    // 2.5 contracts at a scale of 2 is 250, never a double.
    EXPECT_EQ(param_decimal(result.values, "qty", 2), 250);
}

TEST(Params, TheTypedReadersThrowForANameTheSchemaLacks) {
    const auto result = validate(schema(), {{"symbol", "X"}});
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_THROW(param_int(result.values, "no-such-param"), std::out_of_range);
}

}  // namespace
