#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace cpm {

// Fixed-point decimals for market data (design §8): prices in thousandths,
// sizes in hundredths, never through double.
using Price = std::int64_t;   // 0.580 -> 580
using Qty = std::int64_t;     // 30.72 -> 3072
using Amount = std::int64_t;  // 0.049500 -> 49500; negative for a maker rebate

inline constexpr int kPriceScale = 3;
inline constexpr int kQtyScale = 2;
// Order entry amounts: fees, spend caps and payouts, 6 dp (OE "Price &
// Quantity Encoding").
inline constexpr int kAmtScale = 6;
inline constexpr Price kPriceOne = 1000;

// Parses a non-negative decimal string into `scale_digits` fixed point.
// Strict: digits with at most one '.', at least one digit on each side of it,
// no sign, exponent, blank or separator, and never more decimal places than
// the scale allows. Fewer is fine: the wire carries 0-3 dp (O4, "270=0.4").
// Returns nullopt on anything else, including values too large for int64.
std::optional<std::int64_t> parse_scaled(std::string_view text, int scale_digits);

// Formats without trailing zeros, as the gateway sends it (O3/O4):
// 440 -> "0.44", 400 -> "0.4", 435 -> "0.435"; 200 -> "2", 250 -> "2.5".
// Negative values are not expected on the wire and are not supported.
std::string format_scaled(std::int64_t value, int scale_digits);

// Order entry parses decimals differently from market data. The gateway
// "truncates excess precision toward zero, not rejected" (OE spec), so a client
// that rejected 0.6505 would refuse a value the venue itself accepts as 0.650.
// Amounts may also be negative: MiscFeeAmt(137) is a maker rebate when it is.
// Anything else - a stray sign, a second '.', non-digits - is still refused.
std::optional<std::int64_t> parse_signed_truncating(std::string_view text, int scale_digits);

// Like format_scaled, but writes a leading '-' for negatives.
std::string format_signed(std::int64_t value, int scale_digits);

inline std::optional<Amount> parse_amount(std::string_view text) {
    return parse_signed_truncating(text, kAmtScale);
}
inline std::string format_amount(Amount amount) { return format_signed(amount, kAmtScale); }

inline std::optional<Price> parse_price(std::string_view text) { return parse_scaled(text, kPriceScale); }
inline std::optional<Qty> parse_qty(std::string_view text) { return parse_scaled(text, kQtyScale); }
inline std::string format_price(Price price) { return format_scaled(price, kPriceScale); }
inline std::string format_qty(Qty qty) { return format_scaled(qty, kQtyScale); }

}  // namespace cpm
