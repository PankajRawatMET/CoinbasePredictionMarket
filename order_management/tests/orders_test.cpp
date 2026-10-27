// The client-side order rules: the checks that stop a request the gateway
// could only answer with BusinessMessageReject(j) from reaching the wire.
#include "cpm/oms/order_rules.hpp"

#include <gtest/gtest.h>

#include <set>
#include <stdexcept>
#include <string>

namespace {

using cpm::oms::CancelOrder;
using cpm::oms::MassCancel;
using cpm::oms::NewOrder;
using cpm::oms::OrderType;
using cpm::oms::PostOnly;
using cpm::oms::Side;
using cpm::oms::TimeInForce;

cpm::OeConfig config_with_subaccount() {
    cpm::OeConfig config;
    config.subaccount = "sub-1";
    return config;
}

NewOrder limit_buy() {
    NewOrder order;
    order.cl_ord_id = "o1";
    order.symbol = "MKT-A";
    order.side = Side::Buy;
    order.quantity = 100;  // 1.00 contract
    order.type = OrderType::Limit;
    order.price = 650;     // 0.650
    order.time_in_force = TimeInForce::Gtc;
    return order;
}

std::string reason(const NewOrder& order, const cpm::OeConfig& config) {
    const auto result = cpm::oms::check_order(order, config);
    return result.value_or("");
}

TEST(OrderRules, AcceptsAPlainLimitOrder) {
    EXPECT_EQ(reason(limit_buy(), config_with_subaccount()), "");
}

// Parties with PartyRole=24 is required on every order.
TEST(OrderRules, RefusesAnOrderWithNoSubAccount) {
    cpm::OeConfig config;  // subaccount empty, as creds-oe.env ships it
    EXPECT_NE(reason(limit_buy(), config).find("sub-account"), std::string::npos);
    EXPECT_NE(reason(limit_buy(), config).find("CPM_OE_SUBACCOUNT"), std::string::npos);
}

// The venue creates a sub-account lazily, so any well-formed name works - but
// which characters are allowed depends on the length.
TEST(OrderRules, SubAccountNamesFollowTheLengthBands) {
    const auto bad = [](const std::string& name) {
        return cpm::oms::check_subaccount(name).value_or("");
    };
    // 1-18: any printable ASCII.
    EXPECT_EQ(bad("dev-1"), "");
    EXPECT_EQ(bad("my sub_account #1"), "");
    EXPECT_EQ(bad(std::string(18, 'x')), "");
    EXPECT_NE(bad("dev-1").find("printable ASCII"), std::string::npos);
    EXPECT_NE(bad("").find("must not be empty"), std::string::npos);

    // 19-20: alphanumerics and '-' only.
    EXPECT_EQ(bad(std::string(19, 'a')), "");
    EXPECT_EQ(bad("abc-def-ghi-jkl-mno"), "");
    EXPECT_NE(bad("abc_def_ghi_jkl_mno").find("alphanumerics and '-'"), std::string::npos);

    // 21-31: lowercase hex, no leading zero.
    EXPECT_EQ(bad(std::string(21, 'a')), "");
    EXPECT_NE(bad(std::string(21, 'A')).find("lowercase hex"), std::string::npos);
    EXPECT_NE(bad(std::string(21, 'z')).find("lowercase hex"), std::string::npos);
    EXPECT_NE(bad("0" + std::string(20, 'a')).find("leading '0'"), std::string::npos);

    // 32-35 and >36 are invalid outright; 36 must be a canonical UUID.
    for (const std::size_t size : {std::size_t{32}, std::size_t{35}, std::size_t{37}}) {
        EXPECT_NE(bad(std::string(size, 'a')).find("valid lengths"), std::string::npos) << size;
    }
    EXPECT_EQ(bad("123e4567-e89b-12d3-a456-426614174000"), "");
    EXPECT_NE(bad("123E4567-E89B-12D3-A456-426614174000").find("canonical UUID"), std::string::npos);
    EXPECT_NE(bad("123e4567xe89b-12d3-a456-426614174000").find("canonical UUID"), std::string::npos);
}

// An order carries the configured sub-account, so a bad one is caught here
// rather than coming back as "invalid PartyID".
TEST(OrderRules, AnInvalidSubAccountStopsTheOrder) {
    cpm::OeConfig config;
    config.subaccount = std::string(33, 'a');
    EXPECT_NE(reason(limit_buy(), config).find("valid lengths"), std::string::npos);
    config.subaccount = "dev-1";
    EXPECT_EQ(reason(limit_buy(), config), "");
}

TEST(OrderRules, ClOrdIdMustFitSixteenBytesOfPrintableAscii) {
    const auto config = config_with_subaccount();
    auto order = limit_buy();
    order.cl_ord_id = std::string(16, 'a');
    EXPECT_EQ(reason(order, config), "");
    order.cl_ord_id = std::string(17, 'a');
    EXPECT_NE(reason(order, config).find("16 bytes"), std::string::npos);
    order.cl_ord_id = "";
    EXPECT_NE(reason(order, config).find("required"), std::string::npos);
    // Multi-byte characters eat more than one byte each and are refused rather
    // than truncated.
    order.cl_ord_id = "ord-\xc3\xa9";
    EXPECT_NE(reason(order, config).find("printable ASCII"), std::string::npos);
}

// OrdType(40) and Price(44) must agree, both ways.
TEST(OrderRules, PriceAndOrderTypeMustAgree) {
    const auto config = config_with_subaccount();
    auto order = limit_buy();
    order.price = 0;
    EXPECT_NE(reason(order, config).find("Price(44)"), std::string::npos);

    order = limit_buy();
    order.type = OrderType::Market;
    EXPECT_NE(reason(order, config).find("omitted for a market order"), std::string::npos);
    order.price = 0;
    EXPECT_EQ(reason(order, config), "");
}

TEST(OrderRules, PricesOutsideTheTradableRangeAreRefused) {
    const auto config = config_with_subaccount();
    auto order = limit_buy();
    for (const cpm::Price price : {0, 1000, 1001, -5}) {
        order.price = price;
        EXPECT_NE(reason(order, config).find("0.001 and 0.999"), std::string::npos)
            << "accepted " << price;
    }
    order.price = 1;    // 0.001
    EXPECT_EQ(reason(order, config), "");
    order.price = 999;  // 0.999
    EXPECT_EQ(reason(order, config), "");
}

// A market order is aggressive by construction, so post-only can never hold.
TEST(OrderRules, AMarketOrderCannotBePostOnly) {
    const auto config = config_with_subaccount();
    auto order = limit_buy();
    order.type = OrderType::Market;
    order.price = 0;
    for (const auto mode : {PostOnly::Slide, PostOnly::Reject}) {
        order.post_only = mode;
        EXPECT_NE(reason(order, config).find("POST_ONLY_WOULD_CROSS"), std::string::npos);
    }
    // On a limit order both modes are fine.
    order = limit_buy();
    order.post_only = PostOnly::Reject;
    EXPECT_EQ(reason(order, config), "");
}

TEST(OrderRules, MaxExecutionCostIsFokOnlyAndPositive) {
    const auto config = config_with_subaccount();
    auto order = limit_buy();
    order.max_execution_cost = 1250000;  // 1.25 USDC
    EXPECT_NE(reason(order, config).find("requires TimeInForce(59)=FOK"), std::string::npos);

    order.time_in_force = TimeInForce::Fok;
    EXPECT_EQ(reason(order, config), "");
    order.max_execution_cost = 0;
    EXPECT_NE(reason(order, config).find("must be positive"), std::string::npos);
    order.max_execution_cost = -1;
    EXPECT_NE(reason(order, config).find("must be positive"), std::string::npos);
}

TEST(OrderRules, QuantityMustBePositive) {
    const auto config = config_with_subaccount();
    auto order = limit_buy();
    order.quantity = 0;
    EXPECT_NE(reason(order, config).find("OrderQty(38)"), std::string::npos);
    order.quantity = -100;
    EXPECT_NE(reason(order, config).find("OrderQty(38)"), std::string::npos);
    // The minimum increment is 0.01 contract, which is 1 in hundredths.
    order.quantity = 1;
    EXPECT_EQ(reason(order, config), "");
}

TEST(OrderRules, CancelsNeedAnIdentifierAndASymbol) {
    CancelOrder cancel;
    cancel.cl_ord_id = "c1";
    cancel.symbol = "MKT-A";
    EXPECT_NE(cpm::oms::check_cancel(cancel).value_or("").find("OrigClOrdID(41) or OrderID(37)"),
              std::string::npos);

    cancel.orig_cl_ord_id = "o1";
    EXPECT_FALSE(cpm::oms::check_cancel(cancel).has_value());

    // Either identifier is enough; the gateway prefers OrderID when both come.
    cancel.orig_cl_ord_id.clear();
    cancel.order_id = "1001";
    EXPECT_FALSE(cpm::oms::check_cancel(cancel).has_value());

    cancel.symbol.clear();
    EXPECT_NE(cpm::oms::check_cancel(cancel).value_or("").find("Symbol(55)"), std::string::npos);
}

TEST(OrderRules, APerSecurityMassCancelNeedsItsSymbol) {
    MassCancel request;
    request.cl_ord_id = "m1";
    request.scope = MassCancel::Scope::Security;
    EXPECT_NE(cpm::oms::check_mass_cancel(request).value_or("").find("per-security"), std::string::npos);
    request.symbol = "MKT-A";
    EXPECT_FALSE(cpm::oms::check_mass_cancel(request).has_value());

    // The other two scopes need nothing else.
    for (const auto scope : {MassCancel::Scope::TradingSession, MassCancel::Scope::All}) {
        MassCancel all;
        all.cl_ord_id = "m2";
        all.scope = scope;
        EXPECT_FALSE(cpm::oms::check_mass_cancel(all).has_value());
    }
}

// ClOrdIDs must be unique among live orders and fit 16 bytes, so the counter is
// base36 and the prefix is short (OE-Q9).
TEST(OrderRules, GeneratedClOrdIdsAreShortAndUnique) {
    EXPECT_EQ(cpm::oms::make_cl_ord_id("A7F3-", 0), "A7F3-0");
    EXPECT_EQ(cpm::oms::make_cl_ord_id("A7F3-", 35), "A7F3-z");
    EXPECT_EQ(cpm::oms::make_cl_ord_id("A7F3-", 36), "A7F3-10");

    // A prefix of 6 characters or fewer leaves room for 36^10 orders, which is
    // the practical limit worth supporting.
    const auto big = cpm::oms::make_cl_ord_id("A7F3-", 36'000'000'000ULL);
    EXPECT_LE(big.size(), 16u) << big;
    EXPECT_FALSE(cpm::oms::check_cl_ord_id(big).has_value()) << big;

    // Beyond that the id would not fit the venue's 16 bytes, so it throws
    // rather than emitting something that can only be rejected.
    EXPECT_THROW(cpm::oms::make_cl_ord_id("A7F3-", 0xFFFFFFFFFFFFFFFFULL), std::invalid_argument);
    EXPECT_THROW(cpm::oms::make_cl_ord_id(std::string(17, 'p'), 0), std::invalid_argument);

    // And successive counters never collide.
    std::set<std::string> seen;
    for (std::uint64_t i = 0; i < 5000; ++i) {
        EXPECT_TRUE(seen.insert(cpm::oms::make_cl_ord_id("x", i)).second) << i;
    }
}

}  // namespace
