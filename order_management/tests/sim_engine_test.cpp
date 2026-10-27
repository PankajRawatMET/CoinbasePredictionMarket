// The oe_sim matching engine: the semantics Stages 4-6 will be tested against,
// so the ones the spec is most explicit about are pinned here.
#include "cpm/oms/sim/engine.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using cpm::oms::MassCancel;
using cpm::oms::NewOrder;
using cpm::oms::OrderType;
using cpm::oms::PostOnly;
using cpm::oms::Side;
using cpm::oms::TimeInForce;
using cpm::oms::sim::Engine;
using cpm::oms::sim::Event;
using cpm::oms::sim::Fees;
using cpm::oms::sim::Instrument;
using Kind = cpm::oms::sim::Event::Kind;

const std::string kSym = "SIMOE-A";
const std::string kSub = "dev-1";

cpm::oms::Clock::time_point at(int secs) {
    return cpm::oms::Clock::time_point{} + std::chrono::seconds(secs);
}

std::vector<Instrument> instruments() {
    return {{kSym, true, std::chrono::milliseconds{0}},
            {"SIMOE-CLOSED", false, std::chrono::milliseconds{0}},
            {"SIMOE-BUMP", true, std::chrono::milliseconds{200}}};
}

NewOrder order(const std::string& id, Side side, cpm::Qty quantity, cpm::Price price,
               const std::string& symbol = kSym) {
    NewOrder o;
    o.cl_ord_id = id;
    o.symbol = symbol;
    o.side = side;
    o.quantity = quantity;
    o.price = price;
    o.type = OrderType::Limit;
    o.time_in_force = TimeInForce::Gtc;
    return o;
}

struct Rig {
    Rig() : engine(instruments(), Fees{100, -50}) {}

    // Rests a maker and returns nothing but its order id.
    std::string rest(const std::string& id, Side side, cpm::Qty quantity, cpm::Price price) {
        const auto events = engine.submit(order(id, side, quantity, price), kSub, at(1));
        EXPECT_EQ(events.size(), 1u);
        EXPECT_EQ(events.front().kind, Kind::New);
        return events.front().order_id;
    }

    Engine engine;
};

TEST(SimEngine, ARestingOrderIsAcceptedAndSitsOnTheBook) {
    Rig rig;
    const auto events = rig.engine.submit(order("o1", Side::Buy, 100, 400), kSub, at(1));
    ASSERT_EQ(events.size(), 1u);
    const auto& event = events.front();
    EXPECT_EQ(event.kind, Kind::New);
    EXPECT_EQ(event.cl_ord_id, "o1");
    EXPECT_EQ(event.leaves, 100);
    EXPECT_EQ(event.cumulative, 0);
    EXPECT_EQ(event.average_price, 0) << "AvgPx is zero before any fill";
    EXPECT_EQ(event.subaccount, kSub);
    EXPECT_FALSE(event.order_id.empty());

    const auto book = rig.engine.book(kSym, Side::Buy);
    ASSERT_EQ(book.size(), 1u);
    EXPECT_EQ(book.front().price, 400);
    EXPECT_EQ(rig.engine.book(kSym, Side::Sell).size(), 0u);
}

// A fill produces two reports, taker first, sharing one TrdMatchID. The taker
// pays, the maker is paid.
TEST(SimEngine, ACrossingOrderFillsAndBothSidesAreReported) {
    Rig rig;
    rig.rest("maker", Side::Sell, 100, 500);
    const auto events = rig.engine.submit(order("taker", Side::Buy, 100, 520), kSub, at(2));

    ASSERT_EQ(events.size(), 2u) << "one fill each, and nothing resting afterwards";
    const auto& taker = events[0];
    const auto& maker = events[1];
    EXPECT_EQ(taker.kind, Kind::Fill);
    EXPECT_EQ(maker.kind, Kind::Fill);
    EXPECT_EQ(taker.cl_ord_id, "taker");
    EXPECT_EQ(maker.cl_ord_id, "maker");
    EXPECT_TRUE(taker.aggressor);
    EXPECT_FALSE(maker.aggressor);
    EXPECT_EQ(taker.trade_match_id, maker.trade_match_id);

    // The resting order's price wins, not the aggressor's limit.
    EXPECT_EQ(taker.last_price, 500);
    EXPECT_EQ(taker.last_quantity, 100);
    EXPECT_EQ(taker.leaves, 0);
    EXPECT_EQ(taker.cumulative, 100);
    EXPECT_EQ(taker.average_price, 500);

    // 1.00 contract at 0.500 is 0.5 USDC: a 100 bps taker fee and a 50 bps
    // maker rebate.
    EXPECT_EQ(taker.fee, 5000) << cpm::format_amount(taker.fee);
    EXPECT_EQ(maker.fee, -2500) << cpm::format_amount(maker.fee);
    EXPECT_TRUE(rig.engine.book(kSym, Side::Sell).empty());
}

TEST(SimEngine, APartialFillLeavesTheRemainderResting) {
    Rig rig;
    rig.rest("maker", Side::Sell, 40, 500);
    const auto events = rig.engine.submit(order("taker", Side::Buy, 100, 500), kSub, at(2));
    ASSERT_EQ(events.size(), 3u);
    EXPECT_EQ(events[0].kind, Kind::Fill);
    EXPECT_EQ(events[0].last_quantity, 40);
    EXPECT_EQ(events[0].leaves, 60);
    EXPECT_EQ(events[1].kind, Kind::Fill) << "the maker's side of the same trade";
    EXPECT_EQ(events[2].kind, Kind::New) << "the remainder rests, reported after the fills";
    EXPECT_EQ(events[2].leaves, 60);
    EXPECT_EQ(events[2].cumulative, 40);

    const auto book = rig.engine.book(kSym, Side::Buy);
    ASSERT_EQ(book.size(), 1u);
    EXPECT_EQ(book.front().leaves, 60);
}

// An IOC remainder EXPIRES; it is never Canceled.
TEST(SimEngine, AnIocRemainderExpires) {
    Rig rig;
    rig.rest("maker", Side::Sell, 40, 500);
    auto ioc = order("ioc", Side::Buy, 100, 500);
    ioc.time_in_force = TimeInForce::Ioc;
    const auto events = rig.engine.submit(ioc, kSub, at(2));

    ASSERT_EQ(events.size(), 3u);
    EXPECT_EQ(events[0].kind, Kind::Fill);
    EXPECT_EQ(events[2].kind, Kind::Expired);
    EXPECT_EQ(events[2].text, "TIME_IN_FORCE");
    EXPECT_EQ(events[2].leaves, 0);
    EXPECT_EQ(events[2].cumulative, 40);
    EXPECT_EQ(events[2].reject_reason, 0) << "an expiry carries no OrdRejReason";
    EXPECT_TRUE(rig.engine.book(kSym, Side::Buy).empty());
}

// FOK is all-or-nothing and checked before anything executes.
TEST(SimEngine, AnUnfillableFokExpiresWithoutTrading) {
    Rig rig;
    rig.rest("maker", Side::Sell, 40, 500);
    auto fok = order("fok", Side::Buy, 100, 500);
    fok.time_in_force = TimeInForce::Fok;
    const auto events = rig.engine.submit(fok, kSub, at(2));

    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events.front().kind, Kind::Expired);
    EXPECT_EQ(events.front().text, "FOK_NOT_FILLED");
    EXPECT_EQ(events.front().cumulative, 0) << "nothing executed";
    // The maker is untouched.
    ASSERT_EQ(rig.engine.book(kSym, Side::Sell).size(), 1u);
    EXPECT_EQ(rig.engine.book(kSym, Side::Sell).front().leaves, 40);
}

TEST(SimEngine, AFillableFokTradesCompletely) {
    Rig rig;
    rig.rest("m1", Side::Sell, 60, 500);
    rig.rest("m2", Side::Sell, 60, 510);
    auto fok = order("fok", Side::Buy, 100, 520);
    fok.time_in_force = TimeInForce::Fok;
    const auto events = rig.engine.submit(fok, kSub, at(2));

    // Two trades, so four reports, and the taker ends filled.
    ASSERT_EQ(events.size(), 4u);
    EXPECT_EQ(events[0].last_price, 500) << "the better price first";
    EXPECT_EQ(events[2].last_price, 510);
    EXPECT_EQ(events[2].leaves, 0);
    EXPECT_EQ(events[2].cumulative, 100);
    // VWAP of 60 at 0.500 and 40 at 0.510.
    EXPECT_EQ(events[2].average_price, (60 * 500 + 40 * 510) / 100);
}

// A cap breach is REJECTED, not Expired, and carries OrdRejReason 3.
TEST(SimEngine, AMaxExecutionCostBreachIsRejected) {
    Rig rig;
    rig.rest("maker", Side::Sell, 100, 500);
    auto fok = order("fok", Side::Buy, 100, 520);
    fok.time_in_force = TimeInForce::Fok;
    fok.max_execution_cost = 100;  // 0.0001 USDC, far below the 0.5 USDC cost
    const auto events = rig.engine.submit(fok, kSub, at(2));

    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events.front().kind, Kind::Rejected);
    EXPECT_EQ(events.front().reject_reason, 3);
    EXPECT_NE(events.front().text.find("MAX_EXECUTION_COST_EXCEEDED"), std::string::npos);
    EXPECT_EQ(rig.engine.book(kSym, Side::Sell).size(), 1u) << "nothing executed";

    // A cap above the all-in cost lets it through.
    auto generous = fok;
    generous.cl_ord_id = "fok2";
    generous.max_execution_cost = 1000000;  // 1 USDC
    const auto filled = rig.engine.submit(generous, kSub, at(3));
    EXPECT_EQ(filled.size(), 2u);
    EXPECT_EQ(filled.front().kind, Kind::Fill);
}

TEST(SimEngine, PostOnlyRejectsOrSlidesRatherThanCrossing) {
    Rig rig;
    rig.rest("maker", Side::Sell, 100, 500);

    auto reject = order("po-reject", Side::Buy, 100, 520);
    reject.post_only = PostOnly::Reject;
    const auto rejected = rig.engine.submit(reject, kSub, at(2));
    ASSERT_EQ(rejected.size(), 1u);
    EXPECT_EQ(rejected.front().kind, Kind::Rejected);
    EXPECT_EQ(rejected.front().reject_reason, 11);
    EXPECT_EQ(rejected.front().text, "POST_ONLY_WOULD_CROSS");

    auto slide = order("po-slide", Side::Buy, 100, 520);
    slide.post_only = PostOnly::Slide;
    const auto slid = rig.engine.submit(slide, kSub, at(3));
    ASSERT_EQ(slid.size(), 1u);
    EXPECT_EQ(slid.front().kind, Kind::New);
    EXPECT_EQ(slid.front().price, 499) << "slid just inside the opposite best";
    EXPECT_EQ(rig.engine.book(kSym, Side::Sell).size(), 1u) << "and it did not trade";

    // Post-only that would not cross anyway rests at its own price.
    auto quiet = order("po-quiet", Side::Buy, 100, 400);
    quiet.post_only = PostOnly::Reject;
    const auto resting = rig.engine.submit(quiet, kSub, at(4));
    EXPECT_EQ(resting.front().kind, Kind::New);
    EXPECT_EQ(resting.front().price, 400);
}

// A market order is forced to the worst admissible price and to IOC.
TEST(SimEngine, AMarketOrderTakesTheWorstPriceAndIsForcedToIoc) {
    Rig rig;
    rig.rest("maker", Side::Sell, 40, 500);
    NewOrder market;
    market.cl_ord_id = "mkt";
    market.symbol = kSym;
    market.side = Side::Buy;
    market.quantity = 100;
    market.type = OrderType::Market;
    market.time_in_force = TimeInForce::Gtc;  // the gateway overrides this
    const auto events = rig.engine.submit(market, kSub, at(2));

    ASSERT_EQ(events.size(), 3u);
    EXPECT_EQ(events[0].kind, Kind::Fill);
    EXPECT_EQ(events[0].price, cpm::kPriceOne - 1) << "BUY is forced to 0.999";
    EXPECT_EQ(events[0].last_price, 500) << "but it pays the resting price";
    EXPECT_EQ(events[2].kind, Kind::Expired) << "the remainder of an IOC";
    EXPECT_EQ(events[2].time_in_force, TimeInForce::Ioc);
}

// Detectable on receipt, so answered with BusinessMessageReject(j) and no
// ExecutionReport at all.
TEST(SimEngine, RequestsRefusedOnReceipt) {
    Rig rig;
    const auto reject_text = [&](const NewOrder& o, const std::string& subaccount) {
        const auto events = rig.engine.submit(o, subaccount, at(2));
        if (events.size() != 1u || events.front().kind != Kind::BusinessRejected) return std::string("<none>");
        return events.front().text;
    };

    EXPECT_NE(reject_text(order("a", Side::Buy, 100, 400), "").find("INVALID_SUBACCOUNT"),
              std::string::npos);

    auto market_with_price = order("b", Side::Buy, 100, 400);
    market_with_price.type = OrderType::Market;
    EXPECT_NE(reject_text(market_with_price, kSub).find("takes no price"), std::string::npos);

    auto post_only_market = order("c", Side::Buy, 100, 0);
    post_only_market.type = OrderType::Market;
    post_only_market.post_only = PostOnly::Reject;
    EXPECT_EQ(reject_text(post_only_market, kSub), "POST_ONLY_WOULD_CROSS");

    auto cap_without_fok = order("d", Side::Buy, 100, 400);
    cap_without_fok.max_execution_cost = 1000;
    EXPECT_NE(reject_text(cap_without_fok, kSub).find("requires TimeInForce(59)=FOK"), std::string::npos);
}

TEST(SimEngine, RequestsTheEngineRejects) {
    Rig rig;
    const auto rejection = [&](const NewOrder& o) {
        const auto events = rig.engine.submit(o, kSub, at(2));
        EXPECT_EQ(events.size(), 1u);
        return events.front();
    };

    EXPECT_EQ(rejection(order("a", Side::Buy, 100, 400, "SIMOE-NOPE")).reject_reason, 1);
    EXPECT_EQ(rejection(order("b", Side::Buy, 100, 400, "SIMOE-CLOSED")).text, "INSTRUMENT_NOT_ACTIVE");
    EXPECT_EQ(rejection(order("c", Side::Buy, 0, 400)).reject_reason, 13);

    rig.rest("dup", Side::Buy, 100, 400);
    const auto duplicate = rejection(order("dup", Side::Buy, 100, 410));
    EXPECT_EQ(duplicate.reject_reason, 6);
    EXPECT_EQ(duplicate.text, "DUPLICATE_CLIENT_ORDER_ID");
}

TEST(SimEngine, CancelsNameTheCancelAndTheOrder) {
    Rig rig;
    const auto order_id = rig.rest("o1", Side::Buy, 100, 400);
    cpm::oms::CancelOrder request{"c1", "o1", "", kSym, Side::Buy};
    const auto events = rig.engine.cancel(request, at(3));

    ASSERT_EQ(events.size(), 1u);
    const auto& event = events.front();
    EXPECT_EQ(event.kind, Kind::Canceled);
    // OE-O2: the report names the cancel as ClOrdID and the order as OrigClOrdID.
    EXPECT_EQ(event.cl_ord_id, "c1");
    EXPECT_EQ(event.orig_cl_ord_id, "o1");
    EXPECT_EQ(event.order_id, order_id);
    EXPECT_EQ(event.leaves, 0);
    EXPECT_EQ(event.text, "USER_REQUEST");
    EXPECT_TRUE(rig.engine.book(kSym, Side::Buy).empty());

    // Cancelling it again cannot find it.
    const auto again = rig.engine.cancel(request, at(4));
    ASSERT_EQ(again.size(), 1u);
    EXPECT_EQ(again.front().kind, Kind::CancelRejected);
    EXPECT_EQ(again.front().cancel_reject_reason, 1);
    EXPECT_EQ(again.front().text, "UNKNOWN_ORDER");
    EXPECT_EQ(again.front().order_id, "0") << "the sentinel for an unknown order";
}

TEST(SimEngine, CancelsResolveByOrderIdFirst) {
    Rig rig;
    const auto order_id = rig.rest("o1", Side::Buy, 100, 400);
    cpm::oms::CancelOrder by_id{"c1", "wrong-clordid", order_id, kSym, Side::Buy};
    const auto events = rig.engine.cancel(by_id, at(3));
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events.front().kind, Kind::Canceled) << "OrderID wins when both are given";
}

// A session may only cancel its own orders, which is also what keeps a mass
// cancel from touching the simulator's seeded liquidity.
TEST(SimEngine, OneSessionCannotCancelAnothersOrders) {
    Rig rig;
    rig.engine.submit(order("theirs", Side::Buy, 100, 400), "other-account", at(1), "someone-else");
    rig.rest("mine", Side::Buy, 100, 390);

    // By ClOrdID, and by OrderID.
    const auto by_id = rig.engine.cancel({"c1", "theirs", "", kSym, Side::Buy}, at(2));
    ASSERT_EQ(by_id.size(), 1u);
    EXPECT_EQ(by_id.front().kind, Kind::CancelRejected) << "another session's order is simply not found";
    EXPECT_EQ(by_id.front().text, "UNKNOWN_ORDER");

    // A mass cancel of everything touches only our own.
    MassCancel all;
    all.cl_ord_id = "m1";
    all.scope = MassCancel::Scope::All;
    const auto canceled = rig.engine.mass_cancel(all, kSub, at(3));
    ASSERT_EQ(canceled.size(), 1u);
    EXPECT_EQ(canceled.front().cl_ord_id, "mine");
    EXPECT_EQ(rig.engine.open_orders(), 1u) << "theirs survives";

    // And the same ClOrdID may be used by each session independently.
    const auto reused = rig.engine.submit(order("theirs", Side::Buy, 100, 380), kSub, at(4));
    ASSERT_EQ(reused.size(), 1u);
    EXPECT_EQ(reused.front().kind, Kind::New);
}

TEST(SimEngine, MassCancelAppliesItsFiltersTogether) {
    Rig rig;
    rig.rest("b1", Side::Buy, 100, 400);
    rig.rest("b2", Side::Buy, 100, 390);
    rig.rest("s1", Side::Sell, 100, 600);
    const auto other = order("x1", Side::Buy, 100, 400, "SIMOE-BUMP");
    rig.engine.submit(other, kSub, at(1));

    // Side filter only.
    MassCancel buys;
    buys.cl_ord_id = "m1";
    buys.scope = MassCancel::Scope::All;
    buys.side = Side::Buy;
    auto canceled = rig.engine.mass_cancel(buys, kSub, at(5));
    EXPECT_EQ(canceled.size(), 3u) << "both symbols' buys, including the speed-bumped one";
    for (const auto& event : canceled) {
        EXPECT_EQ(event.kind, Kind::Canceled);
        EXPECT_EQ(event.text, "USER_MASS_CANCEL");
    }
    EXPECT_TRUE(rig.engine.book(kSym, Side::Buy).empty());
    EXPECT_EQ(rig.engine.book(kSym, Side::Sell).size(), 1u) << "the sell is untouched";

    // Per security.
    MassCancel per_security;
    per_security.cl_ord_id = "m2";
    per_security.scope = MassCancel::Scope::Security;
    per_security.symbol = "SIMOE-NOTHING-HERE";
    EXPECT_TRUE(rig.engine.mass_cancel(per_security, kSub, at(6)).empty());
    per_security.symbol = kSym;
    EXPECT_EQ(rig.engine.mass_cancel(per_security, kSub, at(6)).size(), 1u);
    EXPECT_EQ(rig.engine.open_orders(), 0u);
}

// A speed bump holds the order as PendingNew until it is released.
TEST(SimEngine, SpeedBumpedOrdersAreNotMatchableUntilReleased) {
    Rig rig;
    const auto events = rig.engine.submit(order("o1", Side::Buy, 100, 400, "SIMOE-BUMP"), kSub, at(1));
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events.front().kind, Kind::PendingNew);
    EXPECT_EQ(events.front().leaves, 100);
    // Not matchable yet, so it is not in the visible book.
    EXPECT_TRUE(rig.engine.book("SIMOE-BUMP", Side::Buy).empty());

    // Too early.
    EXPECT_TRUE(rig.engine.release_speed_bumped(at(1)).empty());

    const auto released = rig.engine.release_speed_bumped(at(2));
    ASSERT_EQ(released.size(), 1u);
    EXPECT_EQ(released.front().kind, Kind::New);
    EXPECT_EQ(rig.engine.book("SIMOE-BUMP", Side::Buy).size(), 1u);
}

TEST(SimEngine, PriceThenTimePriority) {
    Rig rig;
    rig.rest("first", Side::Sell, 50, 500);
    rig.rest("second", Side::Sell, 50, 500);
    rig.rest("better", Side::Sell, 50, 490);

    const auto events = rig.engine.submit(order("taker", Side::Buy, 150, 520), kSub, at(5));
    // Three trades: the better price, then the two at 500 oldest first.
    ASSERT_EQ(events.size(), 6u);
    EXPECT_EQ(events[1].cl_ord_id, "better");
    EXPECT_EQ(events[3].cl_ord_id, "first");
    EXPECT_EQ(events[5].cl_ord_id, "second");
}

TEST(SimEngine, OrderAndTradeIdentifiersAreLargeAndIncreasing) {
    Rig rig;
    const auto first = rig.rest("o1", Side::Buy, 100, 400);
    const auto second = rig.rest("o2", Side::Buy, 100, 390);
    // Shaped like the sandbox's: far beyond 32 bits, and increasing.
    EXPECT_GT(first.size(), 10u);
    EXPECT_LT(std::stoull(first), std::stoull(second));
}

}  // namespace
