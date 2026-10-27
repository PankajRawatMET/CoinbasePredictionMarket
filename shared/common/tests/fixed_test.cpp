// Fixed-point decimals (design §8): prices in thousandths, sizes in
// hundredths, parsed straight from the wire string and never through a double.
#include "cpm/common/fixed.hpp"

#include <gtest/gtest.h>

namespace {

using cpm::format_price;
using cpm::format_qty;
using cpm::parse_price;
using cpm::parse_qty;
using cpm::parse_scaled;

TEST(Fixed, ParsesPricesSeenOnTheSandbox) {
    // 1-3 dp, with and without trailing zeros (O3/O4/O5).
    EXPECT_EQ(parse_price("0.58"), 580);
    EXPECT_EQ(parse_price("0.4"), 400);
    EXPECT_EQ(parse_price("0.435"), 435);
    EXPECT_EQ(parse_price("0.001"), 1);
    EXPECT_EQ(parse_price("0.999"), 999);
    EXPECT_EQ(parse_price("0"), 0);
    EXPECT_EQ(parse_price("1"), 1000);
    EXPECT_EQ(parse_price("1.000"), 1000);
}

TEST(Fixed, ParsesSizesAtTwoDecimalPlaces) {
    EXPECT_EQ(parse_qty("30.72"), 3072);
    EXPECT_EQ(parse_qty("2"), 200);
    EXPECT_EQ(parse_qty("2.5"), 250);
    EXPECT_EQ(parse_qty("0"), 0);
}

TEST(Fixed, RejectsMoreDecimalPlacesThanTheScale) {
    EXPECT_FALSE(parse_price("0.5801").has_value());
    EXPECT_FALSE(parse_qty("2.555").has_value());
    // The scale is the only limit: fewer places are fine.
    EXPECT_EQ(parse_scaled("7", 0), 7);
    EXPECT_FALSE(parse_scaled("7.1", 0).has_value());
}

TEST(Fixed, RejectsAnythingThatIsNotAPlainDecimal) {
    for (const char* text : {"", " ", "0.", ".5", "-0.5", "+0.5", " 0.5", "0.5 ", "0,5", "1e-3", "abc", "0.5x",
                             "0..5", "0.5.5", "1_000"}) {
        EXPECT_FALSE(parse_price(text).has_value()) << "accepted \"" << text << "\"";
    }
}

TEST(Fixed, RejectsValuesTooLargeForInt64) {
    EXPECT_FALSE(parse_qty("9223372036854775807").has_value());
    EXPECT_FALSE(parse_qty("99999999999999999999").has_value());
    EXPECT_EQ(parse_qty("92233720368547758"), 9223372036854775800);
}

TEST(Fixed, FormatsWithoutTrailingZeros) {
    EXPECT_EQ(format_price(440), "0.44");
    EXPECT_EQ(format_price(400), "0.4");
    EXPECT_EQ(format_price(435), "0.435");
    EXPECT_EQ(format_price(1), "0.001");
    EXPECT_EQ(format_price(0), "0");
    EXPECT_EQ(format_price(1000), "1");
    EXPECT_EQ(format_qty(200), "2");
    EXPECT_EQ(format_qty(250), "2.5");
    EXPECT_EQ(format_qty(3072), "30.72");
}

TEST(Fixed, FormatThenParseIsIdentityOverEveryPriceAndSize) {
    for (cpm::Price price = 0; price <= cpm::kPriceOne; ++price) {
        EXPECT_EQ(parse_price(format_price(price)), price) << price;
    }
    for (cpm::Qty qty = 0; qty <= 10000; ++qty) {
        EXPECT_EQ(parse_qty(format_qty(qty)), qty) << qty;
    }
}

// --- order entry decimals -------------------------------------------------

TEST(Fixed, AmountsCarrySixDecimalsAndASign) {
    // Fees, spend caps and payouts (OE "Price & Quantity Encoding").
    EXPECT_EQ(cpm::parse_amount("0.049500"), 49500);
    EXPECT_EQ(cpm::parse_amount("3.000000"), 3000000);
    EXPECT_EQ(cpm::parse_amount("0"), 0);
    // A negative MiscFeeAmt(137) is a maker rebate.
    EXPECT_EQ(cpm::parse_amount("-0.325000"), -325000);
    EXPECT_EQ(cpm::format_amount(-325000), "-0.325");
    EXPECT_EQ(cpm::format_amount(49500), "0.0495");
    EXPECT_EQ(cpm::format_amount(0), "0");
}

// The gateway truncates excess precision toward zero rather than rejecting it,
// so a client that refused it would refuse a value the venue accepts (OE-Q8).
TEST(Fixed, OrderEntryTruncatesExcessPrecisionWhereMarketDataRejects) {
    EXPECT_EQ(cpm::parse_signed_truncating("0.6505", cpm::kPriceScale), 650);
    EXPECT_EQ(cpm::parse_signed_truncating("0.6509999", cpm::kPriceScale), 650);
    EXPECT_EQ(cpm::parse_signed_truncating("1.239", cpm::kQtyScale), 123);
    // Toward zero, not away from it.
    EXPECT_EQ(cpm::parse_signed_truncating("-0.6505", cpm::kPriceScale), -650);
    // Market data still refuses the same input.
    EXPECT_FALSE(cpm::parse_price("0.6505").has_value());
}

TEST(Fixed, TruncatingParseStillRefusesNonDecimals) {
    for (const char* text : {"", "-", "+", ".", "0..5", "0.5.5", "abc", "0x10", "1 0", "--1"}) {
        EXPECT_FALSE(cpm::parse_signed_truncating(text, 3).has_value()) << "accepted '" << text << "'";
    }
    // No exponent form.
    EXPECT_FALSE(cpm::parse_signed_truncating("1e-3", 3).has_value());
}

TEST(Fixed, SignedFormatAndParseRoundTrip) {
    for (std::int64_t value : {-1000000, -325000, -1, 0, 1, 49500, 3000000}) {
        EXPECT_EQ(cpm::parse_amount(cpm::format_amount(value)), value) << value;
    }
}

}  // namespace
