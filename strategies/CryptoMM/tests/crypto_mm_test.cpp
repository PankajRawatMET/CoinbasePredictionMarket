// CryptoMM: the quoting decision, which is a pure function, and the driver
// that puts it on the venue.
#include "cpm/strategy/crypto_mm.hpp"

#include "cpm/md/ticker.hpp"

#include "cpm/oms/oe_client.hpp"
#include "cpm/oms/order_rules.hpp"
#include "cpm/oms/order_state.hpp"
#include "cpm/oms/sim/simulator.hpp"

#include <gtest/gtest.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using cpm::md::BookLevel;
using cpm::md::InstrumentView;
using cpm::oms::ExecType;
using cpm::oms::ExecutionReport;
using cpm::oms::OrdStatus;
using cpm::oms::Position;
using cpm::oms::Side;
using cpm::strategy::Params;
using cpm::strategy::QuoteManager;
using cpm::strategy::TickStructure;
using cpm::strategy::desired_quotes;
using cpm::strategy::tick_at;

const std::string kSym = "SIMOE-26NOV01HOMAWY-HOM";

// A book with one level each side. The NO bid is given in its own contract's
// price, so a NO bid of 0.450 is a YES ask of 0.550.
InstrumentView book(cpm::Price yes_bid, cpm::Price no_bid, cpm::Qty size = 500) {
    InstrumentView view;
    view.symbol = kSym;
    view.book_valid = true;
    if (yes_bid > 0) view.yes.push_back(BookLevel{yes_bid, size});
    if (no_bid > 0) view.no.push_back(BookLevel{no_bid, size});
    return view;
}

Position flat() {
    Position position;
    position.symbol = kSym;
    return position;
}

Params params(int half_width = 2, cpm::Qty quantity = 100) {
    Params p;
    p.symbol = kSym;
    p.half_width_ticks = half_width;
    p.quantity = quantity;
    return p;
}

// --- the tick grid --------------------------------------------------------

TEST(CryptoMmTicks, TheTaperedGridIsFineAtTheEdgesAndCoarseInTheMiddle) {
    // O5: 0.001 below 0.100 and above 0.900, 0.01 between.
    EXPECT_EQ(tick_at(50, TickStructure::TaperedDeciCent), 1);
    EXPECT_EQ(tick_at(99, TickStructure::TaperedDeciCent), 1);
    EXPECT_EQ(tick_at(100, TickStructure::TaperedDeciCent), 10);
    EXPECT_EQ(tick_at(500, TickStructure::TaperedDeciCent), 10);
    EXPECT_EQ(tick_at(900, TickStructure::TaperedDeciCent), 10);
    EXPECT_EQ(tick_at(901, TickStructure::TaperedDeciCent), 1);

    EXPECT_EQ(tick_at(500, TickStructure::LinearCent), 10);
    EXPECT_EQ(tick_at(50, TickStructure::LinearCent), 10);
    EXPECT_EQ(tick_at(500, TickStructure::DeciCent), 1);
}

// --- the decision ---------------------------------------------------------

TEST(CryptoMmQuotes, QuotesBothSidesAroundTheMid) {
    // YES bid 0.500, NO bid 0.400 -> YES ask 0.600, mid 0.550.
    const auto quotes = desired_quotes(book(500, 400), flat(), params(2, 250));
    ASSERT_TRUE(quotes.bid.has_value());
    ASSERT_TRUE(quotes.ask.has_value());
    // Two 0.01 ticks either side of 0.550.
    EXPECT_EQ(quotes.bid->price, 530);
    EXPECT_EQ(quotes.ask->price, 570);
    EXPECT_EQ(quotes.bid->quantity, 250);
    EXPECT_EQ(quotes.ask->quantity, 250);
    EXPECT_TRUE(quotes.reason.empty());
}

TEST(CryptoMmQuotes, TheWidthIsInTicksNotInPrice) {
    for (const int width : {1, 2, 5}) {
        const auto quotes = desired_quotes(book(500, 400), flat(), params(width));
        ASSERT_TRUE(quotes.bid.has_value()) << width;
        EXPECT_EQ(quotes.bid->price, 550 - 10 * width) << width;
        EXPECT_EQ(quotes.ask->price, 550 + 10 * width) << width;
    }
}

// The mid is taken from both sides, so a one-sided book is not quotable.
TEST(CryptoMmQuotes, NeedsBothSidesOfTheBook) {
    EXPECT_FALSE(desired_quotes(book(500, 0), flat(), params()).bid.has_value());
    EXPECT_EQ(desired_quotes(book(500, 0), flat(), params()).reason, "one-sided book");
    EXPECT_FALSE(desired_quotes(book(0, 400), flat(), params()).ask.has_value());

    InstrumentView no_book;
    no_book.symbol = kSym;
    EXPECT_EQ(desired_quotes(no_book, flat(), params()).reason, "no book");
}

// Q9: both sides are bids, so a crossed book is possible and not quotable.
TEST(CryptoMmQuotes, RefusesToQuoteACrossedBook) {
    // YES bid 0.600 and NO bid 0.450 sum to 1.050: the implied ask is 0.550,
    // below the bid.
    const auto quotes = desired_quotes(book(600, 450), flat(), params());
    EXPECT_FALSE(quotes.bid.has_value());
    EXPECT_FALSE(quotes.ask.has_value());
    EXPECT_EQ(quotes.reason, "crossed or locked book");
}

// Rounding must never move a quote towards the other side.
TEST(CryptoMmQuotes, RoundsAwayFromTheTouch) {
    // Mid 0.545 on a 0.01 grid: the bid rounds down, the ask rounds up.
    const auto quotes = desired_quotes(book(500, 410), flat(), params(1));
    ASSERT_TRUE(quotes.bid.has_value());
    EXPECT_EQ(quotes.bid->price, 530) << "0.545 - 0.01 = 0.535, rounded down";
    EXPECT_EQ(quotes.ask->price, 560) << "0.545 + 0.01 = 0.555, rounded up";
    EXPECT_EQ(quotes.bid->price % 10, 0);
    EXPECT_EQ(quotes.ask->price % 10, 0);
}

// A quote that would cross is stepped back: a post-only reject would halt the
// run (OE-Q6), so the strategy does not even try.
TEST(CryptoMmQuotes, NeverQuotesThroughTheRestingBook) {
    // A one-tick-wide market: YES bid 0.500, YES ask 0.510, mid 0.505.
    const auto quotes = desired_quotes(book(500, 490), flat(), params(2));
    ASSERT_TRUE(quotes.bid.has_value());
    ASSERT_TRUE(quotes.ask.has_value());
    EXPECT_LT(quotes.bid->price, 510) << "inside the ask";
    EXPECT_GT(quotes.ask->price, 500) << "outside the bid";
}

TEST(CryptoMmQuotes, StaysInsideThePriceBounds) {
    // A market near the top of the range, where the grid is also finer.
    const auto high = desired_quotes(book(980, 15), flat(), params(3));
    if (high.ask) {
        EXPECT_LT(high.ask->price, cpm::kPriceOne);
        EXPECT_GT(high.ask->price, 0);
    }
    const auto low = desired_quotes(book(15, 980), flat(), params(3));
    if (low.bid) {
        EXPECT_GT(low.bid->price, 0);
    }
}

TEST(CryptoMmQuotes, QuotesOnlyAtPricesTheGridHas) {
    // The venue's grid is a percentage point at a time until 99, then a tenth
    // of one. A quote between rungs would be rejected, and a reject halts the
    // run (OE-Q6) - so neither the rounding nor the step away from the touch
    // may land between them.
    const auto on_grid = [](cpm::Price price) {
        return cpm::md::snap_down(price, cpm::md::TickStructure::TaperedCent) == price;
    };

    for (cpm::Price bid_price = 100; bid_price <= 980; bid_price += 7) {
        for (const int width : {1, 2, 5}) {
            auto p = params(width);
            p.ticks = cpm::md::TickStructure::TaperedCent;
            // A book whose own prices are off our grid, which is exactly when
            // stepping a tick off the touch used to land between rungs.
            // book() takes the NO bid, so this is an ask 11 thousandths
            // above the bid - a touch that is not on our grid either.
            const auto quotes =
                desired_quotes(book(bid_price, cpm::kPriceOne - bid_price - 11), flat(), p);
            if (quotes.bid) {
                EXPECT_TRUE(on_grid(quotes.bid->price)) << quotes.bid->price;
            }
            if (quotes.ask) {
                EXPECT_TRUE(on_grid(quotes.ask->price)) << quotes.ask->price;
            }
        }
    }

    // And the case that reaches the taper: one price below 99% is 98%.
    EXPECT_EQ(cpm::md::price_below(990, cpm::md::TickStructure::TaperedCent), 980);
    EXPECT_EQ(cpm::md::price_above(990, cpm::md::TickStructure::TaperedCent), 991);
    EXPECT_EQ(cpm::md::price_below(1000, cpm::md::TickStructure::TaperedCent), 999);
}

// Inventory: stop adding to the side that would extend the position.
TEST(CryptoMmQuotes, StopsQuotingTheSideThatWouldExtendThePosition) {
    auto p = params();
    p.max_inventory = 300;

    Position lengthy = flat();
    lengthy.net = 300;
    const auto long_limited = desired_quotes(book(500, 400), lengthy, p);
    EXPECT_FALSE(long_limited.bid.has_value()) << "no more buying";
    EXPECT_TRUE(long_limited.ask.has_value()) << "but it will still sell";
    EXPECT_EQ(long_limited.reason, "long inventory limit reached");

    Position shorted = flat();
    shorted.net = -300;
    const auto short_limited = desired_quotes(book(500, 400), shorted, p);
    EXPECT_TRUE(short_limited.bid.has_value());
    EXPECT_FALSE(short_limited.ask.has_value());

    // Inside the limit, both sides quote.
    Position small = flat();
    small.net = 100;
    EXPECT_TRUE(desired_quotes(book(500, 400), small, p).bid.has_value());

    // And the check can be switched off.
    p.max_inventory = 0;
    EXPECT_TRUE(desired_quotes(book(500, 400), lengthy, p).bid.has_value());
}

// --- the driver, with fakes ----------------------------------------------

struct Fake {
    Fake(Params p = params()) {
        QuoteManager::Hooks hooks;
        hooks.can_submit = [this] { return allowed; };
        hooks.send = [this](const cpm::oms::NewOrder& order) {
            if (refuse_sends) throw std::runtime_error("refused by a limit");
            sent.push_back(order);
            return order.cl_ord_id;
        };
        hooks.cancel = [this](const cpm::oms::CancelOrder& request) {
            cancels.push_back(request);
            return true;
        };
        hooks.next_cl_ord_id = [this] { return "q" + std::to_string(++counter); };
        hooks.log = [this](const std::string& line) { log.push_back(line); };
        manager = std::make_unique<QuoteManager>(std::move(p), std::move(hooks));
    }

    // Acknowledges the last order sent, as a New report would.
    void acknowledge(const cpm::oms::NewOrder& order) {
        ExecutionReport report;
        report.cl_ord_id = order.cl_ord_id;
        report.order_id = "o" + order.cl_ord_id;
        report.exec_id = "e" + order.cl_ord_id;
        report.exec_type = ExecType::New;
        report.status = OrdStatus::New;
        report.symbol = order.symbol;
        report.side = order.side;
        report.price = order.price;
        report.leaves_quantity = order.quantity;
        manager->on_report(report);
    }

    bool allowed = true;
    bool refuse_sends = false;
    std::set<std::string> acknowledged;
    int counter = 0;
    std::vector<cpm::oms::NewOrder> sent;
    std::vector<cpm::oms::CancelOrder> cancels;
    std::vector<std::string> log;
    std::unique_ptr<QuoteManager> manager;
};

TEST(CryptoMmDriver, AbandoningSendsNothingAtAll) {
    // The window closed, so the venue has already cancelled what was resting
    // (ExecRestatementReason=2). A cancel of ours now would race that one and
    // come back UNKNOWN_ORDER, which halts the daemon (OE-Q6).
    Fake fake;
    fake.manager->on_market(book(500, 400), flat(), cpm::oms::Clock::now());
    ASSERT_EQ(fake.sent.size(), 2u);
    for (const auto& order : fake.sent) fake.acknowledge(order);
    ASSERT_TRUE(fake.manager->live_bid().has_value());

    fake.manager->abandon("instrument is Closed");
    EXPECT_TRUE(fake.cancels.empty()) << "a cancel was sent into a closed market";
    EXPECT_FALSE(fake.manager->live_bid().has_value());
    EXPECT_FALSE(fake.manager->live_ask().has_value());
    // And it is quiescent at once: there are no cancels to wait for, so a
    // stop during a closed window completes rather than hanging.
    EXPECT_TRUE(fake.manager->quiescent());

    // Withdrawing, by contrast, still cancels - an operator stopping a
    // strategy on a live market has orders nobody else will cancel.
    Fake live;
    live.manager->on_market(book(500, 400), flat(), cpm::oms::Clock::now());
    for (const auto& order : live.sent) live.acknowledge(order);
    live.manager->withdraw("operator");
    EXPECT_EQ(live.cancels.size(), 2u);
}

TEST(CryptoMmStrategy, WithdrawBeforePullsTheQuotesAheadOfTheClose) {
    // The window end comes from the instrument's own name ([MD] O2), so the
    // strategy can count back from it without being told anything.
    const std::string symbol = "BTCD15M-BTC26OCT260630-0";
    const auto window_end = cpm::md::parse_ticker(symbol)->window_end;

    auto p = params();
    p.symbol = symbol;
    p.withdraw_before = std::chrono::seconds(30);

    Fake fake(p);
    cpm::strategy::CryptoMm strategy(p, [&fake] {
        cpm::strategy::StrategyHooks hooks;
        hooks.can_submit = [] { return true; };
        hooks.send = [&fake](const cpm::oms::NewOrder& order) {
            fake.sent.push_back(order);
            return order.cl_ord_id;
        };
        hooks.cancel = [&fake](const cpm::oms::CancelOrder& request) {
            fake.cancels.push_back(request);
            return true;
        };
        hooks.next_cl_ord_id = [&fake] { return "q" + std::to_string(++fake.counter); };
        hooks.log = [](const std::string&) {};
        return hooks;
    }());

    // Well inside the window: it quotes.
    strategy.on_market(book(500, 400), flat(), window_end - std::chrono::minutes(5));
    ASSERT_EQ(fake.sent.size(), 2u);

    // Inside the last 30 seconds: the quotes come out, once, while the market
    // is still open and a cancel is an ordinary cancel.
    strategy.on_market(book(500, 400), flat(), window_end - std::chrono::seconds(20));
    EXPECT_EQ(fake.cancels.size(), 2u);
    strategy.on_market(book(500, 400), flat(), window_end - std::chrono::seconds(10));
    EXPECT_EQ(fake.cancels.size(), 2u) << "it should pull once, not on every pass";
    EXPECT_EQ(fake.sent.size(), 2u) << "and it must not re-quote inside the window";
}

TEST(CryptoMmStrategy, ZeroWithdrawBeforeRestsToTheBell) {
    // The default: quote to the end and let the venue cancel at the close.
    const std::string symbol = "BTCD15M-BTC26OCT260630-0";
    const auto window_end = cpm::md::parse_ticker(symbol)->window_end;

    auto p = params();
    p.symbol = symbol;
    ASSERT_EQ(p.withdraw_before, std::chrono::seconds(0));

    Fake fake(p);
    cpm::strategy::CryptoMm strategy(p, [&fake] {
        cpm::strategy::StrategyHooks hooks;
        hooks.can_submit = [] { return true; };
        hooks.send = [&fake](const cpm::oms::NewOrder& order) {
            fake.sent.push_back(order);
            return order.cl_ord_id;
        };
        hooks.cancel = [&fake](const cpm::oms::CancelOrder& request) {
            fake.cancels.push_back(request);
            return true;
        };
        hooks.next_cl_ord_id = [&fake] { return "q" + std::to_string(++fake.counter); };
        hooks.log = [](const std::string&) {};
        return hooks;
    }());

    strategy.on_market(book(500, 400), flat(), window_end - std::chrono::minutes(5));
    ASSERT_FALSE(fake.sent.empty());
    // One second before the end it is still quoting, and has cancelled
    // nothing.
    strategy.on_market(book(500, 400), flat(), window_end - std::chrono::seconds(1));
    EXPECT_TRUE(fake.cancels.empty());

    // The close itself reaches it as abandon(), from the host.
    strategy.abandon("instrument is Closed");
    EXPECT_TRUE(fake.cancels.empty()) << "nothing may be sent into a closed market";
}

TEST(CryptoMmStrategy, WithdrawBeforeDoesNothingOnANameWithNoWindow) {
    // A sports market or a simulator symbol has no end in its name, so there
    // is nothing to count back from and the parameter is inert rather than
    // acting on a time it invented.
    auto p = params();
    p.symbol = "SIMOE-26NOV01HOMAWY-HOM";
    p.withdraw_before = std::chrono::seconds(30);

    Fake fake(p);
    cpm::strategy::CryptoMm strategy(p, [&fake] {
        cpm::strategy::StrategyHooks hooks;
        hooks.can_submit = [] { return true; };
        hooks.send = [&fake](const cpm::oms::NewOrder& order) {
            fake.sent.push_back(order);
            return order.cl_ord_id;
        };
        hooks.cancel = [&fake](const cpm::oms::CancelOrder& request) {
            fake.cancels.push_back(request);
            return true;
        };
        hooks.next_cl_ord_id = [&fake] { return "q" + std::to_string(++fake.counter); };
        hooks.log = [](const std::string&) {};
        return hooks;
    }());

    strategy.on_market(book(500, 400), flat(), cpm::oms::Clock::now());
    EXPECT_EQ(fake.sent.size(), 2u);
    EXPECT_TRUE(fake.cancels.empty());
}

TEST(CryptoMmDriver, PlacesBothQuotesOnce) {
    Fake fake;
    fake.manager->on_market(book(500, 400), flat(), {});
    ASSERT_EQ(fake.sent.size(), 2u);
    EXPECT_EQ(fake.sent[0].side, Side::Buy);
    EXPECT_EQ(fake.sent[0].price, 530);
    EXPECT_EQ(fake.sent[1].side, Side::Sell);
    EXPECT_EQ(fake.sent[1].price, 570);
    // Post-only slide, so a quote is adjusted rather than refused (OE-Q6).
    EXPECT_EQ(fake.sent[0].post_only, cpm::oms::PostOnly::Slide);
    EXPECT_EQ(fake.sent[0].time_in_force, cpm::oms::TimeInForce::Gtc);

    // An unchanged market does not re-quote.
    for (auto& order : fake.sent) fake.acknowledge(order);
    const auto placed = fake.sent.size();
    fake.manager->on_market(book(500, 400), flat(), {});
    EXPECT_EQ(fake.sent.size(), placed) << "nothing moved, so nothing was re-sent";
    EXPECT_TRUE(fake.cancels.empty());
}

TEST(CryptoMmDriver, ReQuotesWhenTheMidMoves) {
    Fake fake;
    fake.manager->on_market(book(500, 400), flat(), {});
    for (auto& order : fake.sent) fake.acknowledge(order);
    ASSERT_EQ(fake.manager->live_bid()->price, 530);

    // The market moves two ticks up: mid 0.560, so both targets move.
    fake.manager->on_market(book(520, 400), flat(), {});
    ASSERT_EQ(fake.cancels.size(), 2u) << "no amend on this venue: cancel and re-submit";
    EXPECT_EQ(fake.cancels[0].orig_cl_ord_id, "q1");
    EXPECT_EQ(fake.sent.size(), 2u) << "the replacement waits for the cancel's report";

    // Once the cancels are confirmed, the new quotes go out.
    for (const auto& request : fake.cancels) {
        ExecutionReport report;
        report.cl_ord_id = request.cl_ord_id;
        report.orig_cl_ord_id = request.orig_cl_ord_id;
        report.exec_type = ExecType::Canceled;
        report.status = OrdStatus::Canceled;
        report.symbol = kSym;
        report.side = request.side;
        fake.manager->on_report(report);
    }
    fake.manager->on_market(book(520, 400), flat(), {});
    ASSERT_EQ(fake.sent.size(), 4u);
    EXPECT_EQ(fake.sent[2].price, 540);
    EXPECT_EQ(fake.sent[3].price, 580);
}

TEST(CryptoMmDriver, LeavesAQuoteAloneWhileItsCancelIsOutstanding) {
    Fake fake;
    fake.manager->on_market(book(500, 400), flat(), {});
    for (auto& order : fake.sent) fake.acknowledge(order);
    fake.manager->on_market(book(520, 400), flat(), {});
    const auto cancels = fake.cancels.size();
    ASSERT_EQ(cancels, 2u);
    // Further passes must not stack another cancel on the same orders: the
    // gateway answers that with ORDER_ALREADY_PENDING_CANCEL, which would halt
    // the run.
    for (int pass = 0; pass < 3; ++pass) {
        fake.manager->on_market(book(530, 400), flat(), {});
    }
    EXPECT_EQ(fake.cancels.size(), cancels);
    EXPECT_EQ(fake.sent.size(), 2u) << "and nothing new until the cancels are confirmed";
}

// Only the side whose target actually moved is re-quoted.
TEST(CryptoMmDriver, ASmallMoveReQuotesOnlyTheSideThatChanged) {
    Fake fake;
    fake.manager->on_market(book(500, 400), flat(), {});
    for (auto& order : fake.sent) fake.acknowledge(order);
    // Mid 0.550 -> 0.555: the bid target stays at 0.530 after rounding down,
    // while the ask target moves from 0.570 to 0.580.
    fake.manager->on_market(book(510, 400), flat(), {});
    ASSERT_EQ(fake.cancels.size(), 1u);
    EXPECT_EQ(fake.cancels.front().side, Side::Sell);
    EXPECT_TRUE(fake.manager->live_bid().has_value());
    EXPECT_EQ(fake.manager->live_bid()->price, 530) << "the bid was left where it was";
}

TEST(CryptoMmDriver, AFilledQuoteIsReplacedOnTheNextUpdate) {
    Fake fake;
    fake.manager->on_market(book(500, 400), flat(), {});
    for (auto& order : fake.sent) fake.acknowledge(order);

    ExecutionReport fill;
    fill.cl_ord_id = "q1";  // the bid
    fill.exec_type = ExecType::Trade;
    fill.status = OrdStatus::Filled;
    fill.symbol = kSym;
    fill.side = Side::Buy;
    fill.leaves_quantity = 0;
    fill.cumulative_quantity = 100;
    fill.fill = cpm::oms::Fill{100, 530, "t1", false, -500};
    fake.manager->on_report(fill);
    EXPECT_FALSE(fake.manager->live_bid().has_value());
    EXPECT_EQ(fake.manager->fills(), 1u);

    // The position is now long, and the bid is replaced at the current mid.
    Position position = flat();
    position.net = 100;
    fake.manager->on_market(book(500, 400), position, {});
    ASSERT_EQ(fake.sent.size(), 3u);
    EXPECT_EQ(fake.sent.back().side, Side::Buy);
}

TEST(CryptoMmDriver, APartialFillLeavesTheRestQuoted) {
    Fake fake(params(2, 200));
    fake.manager->on_market(book(500, 400), flat(), {});
    for (auto& order : fake.sent) fake.acknowledge(order);

    ExecutionReport fill;
    fill.cl_ord_id = "q1";
    fill.exec_type = ExecType::Trade;
    fill.status = OrdStatus::PartiallyFilled;
    fill.symbol = kSym;
    fill.side = Side::Buy;
    fill.leaves_quantity = 120;
    fill.cumulative_quantity = 80;
    fill.fill = cpm::oms::Fill{80, 530, "t1", false, -400};
    fake.manager->on_report(fill);

    ASSERT_TRUE(fake.manager->live_bid().has_value());
    EXPECT_EQ(fake.manager->live_bid()->quantity, 120);
    // The remainder is the wrong size now, so the next pass replaces it.
    fake.manager->on_market(book(500, 400), flat(), {});
    EXPECT_EQ(fake.cancels.size(), 1u);
}

// A post-only slide moves the price the venue rests the order at. The
// strategy must not treat that as a reason to re-quote, or it cancels and
// replaces without end - every replacement is slid again. Found by running it.
TEST(CryptoMmDriver, DoesNotFightAPostOnlySlide) {
    Fake fake;
    fake.manager->on_market(book(500, 400), flat(), {});
    ASSERT_EQ(fake.sent.size(), 2u);
    ASSERT_EQ(fake.sent[0].price, 530);

    ExecutionReport slid;
    slid.cl_ord_id = fake.sent[0].cl_ord_id;
    slid.exec_type = ExecType::New;
    slid.status = OrdStatus::New;
    slid.symbol = kSym;
    slid.side = Side::Buy;
    slid.price = 499;  // the venue slid it well away from the target
    slid.leaves_quantity = fake.sent[0].quantity;
    fake.manager->on_report(slid);
    // The live price is what the venue says.
    EXPECT_EQ(fake.manager->live_bid()->price, 499);

    // The same target is not offered again, however far the slide took the
    // order: it would only be slid again.
    for (int pass = 0; pass < 5; ++pass) {
        fake.manager->on_market(book(500, 400), flat(), {});
    }
    EXPECT_TRUE(fake.cancels.empty()) << "it would be slid again, and again";
    EXPECT_EQ(fake.sent.size(), 2u);

    // A genuine move of the target still re-quotes it.
    fake.manager->on_market(book(520, 400), flat(), {});
    ASSERT_FALSE(fake.cancels.empty());
    EXPECT_TRUE(std::any_of(fake.cancels.begin(), fake.cancels.end(),
                            [](const auto& request) { return request.side == Side::Buy; }));
}

TEST(CryptoMmDriver, QuotesNothingWhileSubmissionIsBlocked) {
    Fake fake;
    fake.allowed = false;
    fake.manager->on_market(book(500, 400), flat(), {});
    EXPECT_TRUE(fake.sent.empty()) << "reconciling, halted, or a dry run";
    // But it still worked out what it would have done, and said so.
    ASSERT_FALSE(fake.log.empty());
    EXPECT_NE(fake.log.back().find("not sending"), std::string::npos) << fake.log.back();
    EXPECT_NE(fake.log.back().find("0.53"), std::string::npos) << fake.log.back();

    fake.allowed = true;
    fake.manager->on_market(book(500, 400), flat(), {});
    EXPECT_EQ(fake.sent.size(), 2u);
}

// A run that places nothing must be able to say why: silence is the one
// outcome that cannot be diagnosed afterwards.
TEST(CryptoMmDriver, ExplainsWhyItIsNotQuoting) {
    Fake fake;
    // A one-sided book has no mid.
    fake.manager->on_market(book(500, 0), flat(), {});
    EXPECT_TRUE(fake.sent.empty());
    EXPECT_EQ(fake.manager->last_reason(), "one-sided book");
    EXPECT_EQ(fake.manager->passes(), 1u);
    EXPECT_EQ(fake.manager->passes_without_quotes(), 1u);
    ASSERT_FALSE(fake.log.empty());
    EXPECT_NE(fake.log.back().find("not quoting: one-sided book"), std::string::npos)
        << fake.log.back();

    // No book at all.
    InstrumentView empty;
    empty.symbol = kSym;
    fake.manager->on_market(empty, flat(), {});
    EXPECT_EQ(fake.manager->last_reason(), "no book");

    // And once it can quote, the reason clears and it says what it is doing.
    fake.manager->on_market(book(500, 400), flat(), {});
    EXPECT_TRUE(fake.manager->last_reason().empty());
    EXPECT_EQ(fake.manager->passes(), 3u);
    EXPECT_EQ(fake.manager->passes_without_quotes(), 2u);
    // The announcement comes before the per-order lines, so look through all.
    EXPECT_TRUE(std::any_of(fake.log.begin(), fake.log.end(), [](const std::string& line) {
        return line.find("mid 0.55 -> quoting 0.53 / 0.57") != std::string::npos;
    })) << fake.log.back();
}

// The log is one line per change, not one per pass: this runs several times a
// second.
TEST(CryptoMmDriver, DoesNotRepeatItself) {
    Fake fake;
    for (int pass = 0; pass < 20; ++pass) {
        fake.manager->on_market(book(500, 0), flat(), {});
    }
    EXPECT_EQ(fake.log.size(), 1u) << "twenty identical passes, one line";
    EXPECT_EQ(fake.manager->passes(), 20u);
}

TEST(CryptoMmDriver, SurvivesARefusalBeforeTheWire) {
    Fake fake;
    fake.refuse_sends = true;
    fake.manager->on_market(book(500, 400), flat(), {});
    EXPECT_TRUE(fake.sent.empty());
    EXPECT_FALSE(fake.manager->live_bid().has_value());
    EXPECT_FALSE(fake.log.empty()) << "and it said why";

    // Tried again on the next update.
    fake.refuse_sends = false;
    fake.manager->on_market(book(500, 400), flat(), {});
    EXPECT_EQ(fake.sent.size(), 2u);
}

TEST(CryptoMmDriver, WithdrawsBothSidesOnRequest) {
    Fake fake;
    fake.manager->on_market(book(500, 400), flat(), {});
    for (auto& order : fake.sent) fake.acknowledge(order);
    fake.manager->withdraw("the instrument closed");
    EXPECT_EQ(fake.cancels.size(), 2u);
    EXPECT_EQ(fake.manager->quotes_cancelled(), 2u);
    // And it does not stack more cancels while they are outstanding.
    fake.manager->withdraw("again");
    EXPECT_EQ(fake.cancels.size(), 2u);
}

TEST(CryptoMmDriver, AOneSidedBookWithdrawsTheQuotes) {
    Fake fake;
    fake.manager->on_market(book(500, 400), flat(), {});
    for (auto& order : fake.sent) fake.acknowledge(order);
    // The NO side empties: there is no mid, so both quotes come down.
    fake.manager->on_market(book(500, 0), flat(), {});
    EXPECT_EQ(fake.cancels.size(), 2u);
}

// The point of the strategy: the quotes follow the mid. As the book walks up
// tick by tick, both sides are re-quoted to stay x ticks either side of it.
TEST(CryptoMmDriver, TheQuotesFollowTheMid) {
    Fake fake;
    const auto settle = [&fake](cpm::Price yes_bid, cpm::Price no_bid) {
        // One pass to cancel what has moved, the cancels confirmed, then a
        // second pass to put the replacements out.
        fake.manager->on_market(book(yes_bid, no_bid), flat(), {});
        for (const auto& request : fake.cancels) {
            ExecutionReport report;
            report.cl_ord_id = request.cl_ord_id;
            report.orig_cl_ord_id = request.orig_cl_ord_id;
            report.exec_type = ExecType::Canceled;
            report.status = OrdStatus::Canceled;
            report.symbol = kSym;
            report.side = request.side;
            fake.manager->on_report(report);
        }
        fake.cancels.clear();
        fake.manager->on_market(book(yes_bid, no_bid), flat(), {});
        // Acknowledge whatever went out, at the price asked for.
        for (const auto& order : fake.sent) {
            if (fake.acknowledged.insert(order.cl_ord_id).second) fake.acknowledge(order);
        }
    };

    // The mid walks from 0.550 to 0.600 in 0.01 steps. At each step the quotes
    // straddle the mid, sit at least two ticks from it, and are on the grid -
    // and both move as the mid does, which is the whole point.
    cpm::Price previous_bid = 0;
    cpm::Price previous_ask = 0;
    for (cpm::Price yes_bid = 500; yes_bid <= 550; yes_bid += 10) {
        const cpm::Price mid = (yes_bid + (cpm::kPriceOne - 400)) / 2;
        settle(yes_bid, 400);
        const auto bid = fake.manager->live_bid();
        const auto ask = fake.manager->live_ask();
        ASSERT_TRUE(bid.has_value()) << cpm::format_price(mid);
        ASSERT_TRUE(ask.has_value()) << cpm::format_price(mid);
        const std::string at = " at mid " + cpm::format_price(mid);

        EXPECT_LT(bid->price, mid) << at;
        EXPECT_GT(ask->price, mid) << at;
        EXPECT_LE(bid->price, mid - 20) << "at least two 0.01 ticks below" << at;
        EXPECT_GE(ask->price, mid + 20) << "at least two 0.01 ticks above" << at;
        EXPECT_EQ(bid->price % 10, 0) << "on the 0.01 grid" << at;
        EXPECT_EQ(ask->price % 10, 0) << at;
        // The quotes track the mid upwards, never backwards.
        EXPECT_GE(bid->price, previous_bid) << at;
        EXPECT_GE(ask->price, previous_ask) << at;
        previous_bid = bid->price;
        previous_ask = ask->price;
    }
    // By the end both sides have moved well above where they started.
    EXPECT_GT(previous_bid, 530);
    EXPECT_GT(previous_ask, 570);
    EXPECT_GE(fake.manager->quotes_cancelled(), 4u) << "each move is a cancel and a re-submit";
}

// And down again, including the quantity: y is what is shown on each side.
TEST(CryptoMmDriver, BothSidesAlwaysShowTheConfiguredQuantity) {
    Fake fake(params(3, 250));
    fake.manager->on_market(book(500, 400), flat(), {});
    ASSERT_EQ(fake.sent.size(), 2u);
    for (const auto& order : fake.sent) {
        EXPECT_EQ(order.quantity, 250);
    }
    EXPECT_EQ(fake.sent[0].price, 520) << "three 0.01 ticks below the 0.550 mid";
    EXPECT_EQ(fake.sent[1].price, 580);
}

// --- against oe_sim ------------------------------------------------------

std::uint16_t free_port() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    socklen_t len = sizeof(addr);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    ::close(fd);
    return ntohs(addr.sin_port);
}

template <typename Pred>
bool eventually(Pred pred, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(20ms);
    }
    return pred();
}

// The quotes really do rest on a venue, and the reports come back.
TEST(CryptoMmLive, QuotesRestOnTheSimulatorAndAreCancelled) {
    const cpm::Credentials creds{"mm-key", "mm-pass", "b2Utc2ltLXNlY3JldA=="};
    const auto port = free_port();
    std::ostringstream sim_log;
    cpm::Logger sim_logger(sim_log);
    cpm::oms::sim::SimOptions sim_options;
    sim_options.credentials = creds;
    sim_options.port = port;
    sim_options.dict_dir = CPM_DICT_DIR;
    cpm::oms::sim::Simulator sim(std::move(sim_options), sim_logger);
    sim.start();

    std::ostringstream client_log;
    cpm::Logger client_logger(client_log);
    cpm::oms::OeClientOptions options;
    options.config.credentials = creds;
    options.config.endpoint = {"127.0.0.1", port, false, ""};
    options.config.subaccount = "dev-1";
    options.dict_dir = CPM_DICT_DIR;
    cpm::oms::OeClient client(std::move(options), client_logger);

    cpm::oms::OrderState state;
    QuoteManager::Hooks hooks;
    hooks.can_submit = [] { return true; };
    hooks.send = [&](const cpm::oms::NewOrder& order) {
        state.record_sent(order);
        return client.send(order);
    };
    hooks.cancel = [&](const cpm::oms::CancelOrder& request) {
        state.record_cancel_sent(request);
        return client.cancel(request);
    };
    int counter = 0;
    hooks.next_cl_ord_id = [&counter] { return cpm::oms::make_cl_ord_id("mm-", ++counter); };
    hooks.log = [&](const std::string& line) { client_logger.info(line); };

    QuoteManager manager(params(2, 100), std::move(hooks));
    client.on_events(cpm::oms::combine_oe(state.callbacks(), [&] {
        cpm::oms::OeCallbacks c;
        c.report = [&](const ExecutionReport& report) { manager.on_report(report); };
        return c;
    }()));
    cpm::oms::OrderLimits limits;
    limits.allow_orders = true;
    limits.max_quantity = 1000;
    limits.max_notional = 10000000;
    limits.max_orders_per_sec = 20;
    client.set_limits(limits);
    client.start();
    ASSERT_TRUE(client.wait_logged_on(5s)) << client_log.str();

    // The simulator seeds bids from 0.450 and asks from 0.550, so the mid is
    // 0.500 and the quotes land two 0.01 ticks either side.
    manager.on_market(book(450, 450), flat(), {});
    ASSERT_TRUE(eventually([&] { return manager.live_bid() && manager.live_ask(); }, 5s))
        << client_log.str();
    // Wait for the venue to acknowledge them, not merely for them to be
    // recorded as sent: record_sent() is immediate.
    ASSERT_TRUE(eventually(
        [&] {
            const auto orders = state.orders();
            return orders.size() >= 2 && std::all_of(orders.begin(), orders.end(), [](const auto& o) {
                       return o.status == OrdStatus::New;
                   });
        },
        5s))
        << client_log.str();

    const auto resting_bids = sim.book("SIMOE-26NOV01HOMAWY-HOM", Side::Buy);
    EXPECT_TRUE(std::any_of(resting_bids.begin(), resting_bids.end(), [](const auto& order) {
        return order.cl_ord_id.rfind("mm-", 0) == 0 && order.price == 480;
    })) << "the bid rests at 0.480";
    const auto resting_asks = sim.book("SIMOE-26NOV01HOMAWY-HOM", Side::Sell);
    EXPECT_TRUE(std::any_of(resting_asks.begin(), resting_asks.end(), [](const auto& order) {
        return order.cl_ord_id.rfind("mm-", 0) == 0 && order.price == 520;
    })) << "and the ask at 0.520";

    // Nothing failed to decode, and no order was rejected.
    for (const auto& order : state.orders()) {
        EXPECT_NE(order.status, OrdStatus::Rejected) << order.cl_ord_id << " " << order.text;
    }
    EXPECT_EQ(state.anomalies(), 0u);

    manager.withdraw("end of the test");
    ASSERT_TRUE(eventually(
        [&] {
            const auto bids = sim.book("SIMOE-26NOV01HOMAWY-HOM", Side::Buy);
            return std::none_of(bids.begin(), bids.end(), [](const auto& order) {
                return order.cl_ord_id.rfind("mm-", 0) == 0;
            });
        },
        5s))
        << client_log.str();
    client.stop();
    sim.stop();
}

// --- the pricing speed bump (B3) -----------------------------------------
// The reference venue moves; our quotes become stale before this market
// knows it. The guard leaves, and does not come back until this market has
// repriced (docs/design-binance.md section 4).

namespace {

cpm::binance::ReferenceView reference(const char* mid, bool stale = false) {
    cpm::binance::ReferenceView view;
    view.present = true;
    view.stale = stale;
    view.mid = cpm::parse_scaled(mid, cpm::binance::kScale).value_or(0);
    view.tick_size = cpm::parse_scaled("0.01", cpm::binance::kScale).value_or(0);
    return view;
}

Params guarded(std::int64_t threshold_ticks) {
    auto p = params();
    p.mid_move_threshold_ticks = threshold_ticks;
    return p;
}

// The venue confirming the cancels. Nothing new goes out until it does -
// the manager will not replace a quote whose cancel is still in flight -
// so a test that skips this is testing the wrong thing.
void confirm_cancels(Fake& fake) {
    for (const auto& request : fake.cancels) {
        ExecutionReport report;
        report.cl_ord_id = request.cl_ord_id;
        report.orig_cl_ord_id = request.orig_cl_ord_id;
        report.exec_type = ExecType::Canceled;
        report.status = OrdStatus::Canceled;
        report.symbol = kSym;
        report.side = request.side;
        fake.manager->on_report(report);
    }
    fake.cancels.clear();
}

}  // namespace

TEST(CryptoMmSpeedBump, IsOffByDefaultAndChangesNothing) {
    Fake fake(params());  // no threshold
    // No reference at all, which is the daemon as it was before B3.
    fake.manager->on_market(book(500, 400), flat(), cpm::oms::Clock::now());
    EXPECT_EQ(fake.sent.size(), 2u) << "a strategy without the guard still quotes";
}

TEST(CryptoMmSpeedBump, WillNotQuoteWithoutAReferenceWhenItIsOn) {
    Fake fake(guarded(2500));
    // The guard is on and there is no price: not quoting is the whole point
    // of B-Q4, and quoting unprotected is what it exists to prevent.
    fake.manager->on_market(book(500, 400), flat(), cpm::oms::Clock::now());
    EXPECT_TRUE(fake.sent.empty());
    EXPECT_EQ(fake.manager->last_reason(), "no reference price yet");
}

TEST(CryptoMmSpeedBump, QuotesOnceTheReferenceIsThere) {
    Fake fake(guarded(2500));
    fake.manager->on_reference(reference("67000.00"), cpm::oms::Clock::now());
    fake.manager->on_market(book(500, 400), flat(), cpm::oms::Clock::now());
    EXPECT_EQ(fake.sent.size(), 2u);
}

TEST(CryptoMmSpeedBump, CancelsWhenTheReferenceMovesPastTheThreshold) {
    Fake fake(guarded(2500));  // 2500 ticks of 0.01 = $25
    fake.manager->on_reference(reference("67000.00"), cpm::oms::Clock::now());
    fake.manager->on_market(book(500, 400), flat(), cpm::oms::Clock::now());
    ASSERT_EQ(fake.sent.size(), 2u);
    for (const auto& order : fake.sent) fake.acknowledge(order);

    // $24 is inside the threshold: nothing happens, because a market maker
    // that left on every wobble would never be in the market.
    fake.manager->on_reference(reference("67024.00"), cpm::oms::Clock::now());
    EXPECT_TRUE(fake.cancels.empty());

    // $25 is not. Both sides come off, and on the reference's own update -
    // no book arrived in between, which is B-Q5's whole point.
    fake.manager->on_reference(reference("67025.00"), cpm::oms::Clock::now());
    EXPECT_EQ(fake.cancels.size(), 2u);
    EXPECT_NE(fake.manager->last_reason().find("the reference moved"), std::string::npos);
}

TEST(CryptoMmSpeedBump, CancelsWhicheverWayTheReferenceWent) {
    Fake fake(guarded(1000));
    fake.manager->on_reference(reference("67000.00"), cpm::oms::Clock::now());
    fake.manager->on_market(book(500, 400), flat(), cpm::oms::Clock::now());
    for (const auto& order : fake.sent) fake.acknowledge(order);

    // Down is as stale as up: the quote on one side is picked off either
    // way, and leaving is right whichever it is.
    fake.manager->on_reference(reference("66990.00"), cpm::oms::Clock::now());
    EXPECT_EQ(fake.cancels.size(), 2u);
}

TEST(CryptoMmSpeedBump, StaysOutUntilThisMarketReprices) {
    Fake fake(guarded(1000));
    fake.manager->on_reference(reference("67000.00"), cpm::oms::Clock::now());
    fake.manager->on_market(book(500, 400), flat(), cpm::oms::Clock::now());
    for (const auto& order : fake.sent) fake.acknowledge(order);
    fake.manager->on_reference(reference("67010.00"), cpm::oms::Clock::now());
    ASSERT_EQ(fake.cancels.size(), 2u);
    fake.sent.clear();

    // The same book, again and again: the reference has moved and CPM has
    // not caught up, so quoting now is walking into the pick-off.
    for (int i = 0; i < 5; ++i) fake.manager->on_market(book(500, 400), flat(), cpm::oms::Clock::now());
    EXPECT_TRUE(fake.sent.empty());
    EXPECT_NE(fake.manager->last_reason().find("waiting for this market"), std::string::npos);

    // One tick of movement here is enough: what matters is that this market
    // has absorbed what the reference already knows (B-Q3).
    confirm_cancels(fake);
    fake.manager->on_market(book(510, 400), flat(), cpm::oms::Clock::now());
    EXPECT_FALSE(fake.sent.empty()) << "it must come back once this market moves";
}

TEST(CryptoMmSpeedBump, MeasuresFromWhereItLastPricedNotFromWhereItStarted) {
    Fake fake(guarded(1000));  // $10
    fake.manager->on_reference(reference("67000.00"), cpm::oms::Clock::now());
    fake.manager->on_market(book(500, 400), flat(), cpm::oms::Clock::now());
    for (const auto& order : fake.sent) fake.acknowledge(order);

    // $9 of drift, no breach. Then this market moves and we re-quote, which
    // takes a fresh reference at 67009 (B-Q9).
    fake.manager->on_reference(reference("67009.00"), cpm::oms::Clock::now());
    ASSERT_TRUE(fake.cancels.empty());
    fake.manager->on_market(book(520, 380), flat(), cpm::oms::Clock::now());
    confirm_cancels(fake);
    fake.manager->on_market(book(520, 380), flat(), cpm::oms::Clock::now());
    for (const auto& order : fake.sent) fake.acknowledge(order);
    fake.cancels.clear();

    // Another $9 from there is $18 from the start, and still no breach:
    // the measure is "since we priced", not a drift summed up forever.
    fake.manager->on_reference(reference("67018.00"), cpm::oms::Clock::now());
    EXPECT_TRUE(fake.cancels.empty());
    // $10 from where we priced is.
    fake.manager->on_reference(reference("67019.00"), cpm::oms::Clock::now());
    EXPECT_FALSE(fake.cancels.empty());
}

TEST(CryptoMmSpeedBump, WithdrawsWhileTheReferenceIsStale) {
    Fake fake(guarded(2500));
    fake.manager->on_reference(reference("67000.00"), cpm::oms::Clock::now());
    fake.manager->on_market(book(500, 400), flat(), cpm::oms::Clock::now());
    ASSERT_EQ(fake.sent.size(), 2u);
    for (const auto& order : fake.sent) fake.acknowledge(order);
    fake.sent.clear();

    // No reference means no protection. The quotes come off and stay off
    // until the feed is back, and the daemon is untouched (B-Q4).
    auto gone = reference("67000.00", /*stale=*/true);
    gone.age = std::chrono::milliseconds(7000);
    fake.manager->on_reference(gone, cpm::oms::Clock::now());
    fake.manager->on_market(book(500, 400), flat(), cpm::oms::Clock::now());
    EXPECT_EQ(fake.cancels.size(), 2u);
    EXPECT_TRUE(fake.sent.empty());
    EXPECT_NE(fake.manager->last_reason().find("stale"), std::string::npos);

    // And it comes back when the feed does.
    confirm_cancels(fake);
    fake.manager->on_reference(reference("67000.00"), cpm::oms::Clock::now());
    fake.manager->on_market(book(500, 400), flat(), cpm::oms::Clock::now());
    EXPECT_FALSE(fake.sent.empty());
}

TEST(CryptoMmSpeedBump, WillNotQuoteWithoutATickSizeToCountIn) {
    Fake fake(guarded(2500));
    auto view = reference("67000.00");
    view.tick_size = 0;  // exchangeInfo was unreachable at startup
    fake.manager->on_reference(view, cpm::oms::Clock::now());
    fake.manager->on_market(book(500, 400), flat(), cpm::oms::Clock::now());
    EXPECT_TRUE(fake.sent.empty());
    EXPECT_NE(fake.manager->last_reason().find("tick size"), std::string::npos);
}

TEST(CryptoMmSpeedBump, DoesNothingWithNothingResting) {
    Fake fake(guarded(1000));
    // A reference that moves while we are out of the market is not an
    // event: there is nothing to cancel, and the next quote prices itself
    // against wherever the reference is by then.
    fake.manager->on_reference(reference("67000.00"), cpm::oms::Clock::now());
    fake.manager->on_reference(reference("68000.00"), cpm::oms::Clock::now());
    EXPECT_TRUE(fake.cancels.empty());
    fake.manager->on_market(book(500, 400), flat(), cpm::oms::Clock::now());
    EXPECT_EQ(fake.sent.size(), 2u);
}

}  // namespace
