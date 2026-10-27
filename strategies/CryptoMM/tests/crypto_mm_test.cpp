// CryptoMM: the quoting decision, which is a pure function, and the driver
// that puts it on the venue.
#include "cpm/strategy/crypto_mm.hpp"

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
    if (low.bid) EXPECT_GT(low.bid->price, 0);
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

}  // namespace
