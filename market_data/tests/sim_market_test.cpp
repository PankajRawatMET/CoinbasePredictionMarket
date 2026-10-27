// Market model invariants: books sorted, capped at 10 levels, never crossed
// or locked; the delta stream rebuilds the books exactly; trades follow the
// YES/NO matching rules observed on the sandbox (docs/open-questions.txt O4).
#include "cpm/md/sim/market.hpp"

#include <gtest/gtest.h>

#include <map>

namespace {

using namespace cpm::md::sim;

const Clock::time_point kT0 = Clock::time_point{} + std::chrono::hours(500000);

bool valid_tick(Price p) {
    if (p <= 0 || p >= kPriceOne) return false;
    return (p <= 100 || p >= 900) || p % 10 == 0;
}

void expect_sane(const Instrument& inst) {
    for (const auto* side : {&inst.yes, &inst.no}) {
        EXPECT_LE(side->size(), Market::kMaxLevels) << inst.symbol;
        for (std::size_t i = 0; i < side->size(); ++i) {
            EXPECT_TRUE(valid_tick((*side)[i].price)) << (*side)[i].price;
            EXPECT_GT((*side)[i].qty, 0);
            if (i > 0) {
                EXPECT_GT((*side)[i - 1].price, (*side)[i].price) << "not sorted best first";
            }
        }
    }
    if (!inst.yes.empty() && !inst.no.empty()) {
        EXPECT_LT(inst.yes.front().price + inst.no.front().price, kPriceOne) << "crossed or locked";
    }
}

std::vector<InstrumentSpec> specs() {
    return {{"A", "1", 550, 6}, {"B", "2", 200, 10}, {"C", "3", 900, 2}};
}

TEST(SimMarket, InitialBooks) {
    Market m(specs(), 7, kT0);
    for (const auto& s : m.symbols()) expect_sane(*m.find(s));
    EXPECT_EQ(m.find("B")->yes.size(), 10u);
    EXPECT_EQ(m.find("A")->yes.front().price, 540);  // fair 0.55 - one cent
    EXPECT_EQ(m.find("A")->no.front().price, 440);   // (1 - 0.55) - one cent
    EXPECT_EQ(m.find("X"), nullptr);
}

TEST(SimMarket, DeltaStreamRebuildsBooksAndKeepsInvariants) {
    Market m(specs(), 42, kT0);
    std::map<std::string, Instrument> shadow;
    for (const auto& s : m.symbols()) shadow[s] = *m.find(s);

    int trades = 0;
    for (int i = 0; i < 20000; ++i) {
        const auto t = kT0 + std::chrono::milliseconds(i);
        const Step step = m.step(t);
        ASSERT_FALSE(step.symbol.empty());
        auto& book = shadow[step.symbol];
        for (const Side s : {Side::Yes, Side::No}) {
            const auto& side = s == Side::Yes ? book.yes : book.no;
            const auto before = side.empty() ? std::optional<Level>() : std::optional<Level>(side.front());
            EXPECT_EQ(step.best_before[s == Side::Yes ? 0 : 1], before);
        }
        if (step.trade) {
            ++trades;
            // The trade hits the best bid of one side (the first delta).
            ASSERT_FALSE(step.book.empty());
            const auto& hit = step.book.front();
            const auto& side = hit.side == Side::Yes ? book.yes : book.no;
            ASSERT_FALSE(side.empty());
            EXPECT_EQ(hit.price, side.front().price);
            EXPECT_EQ(step.trade->qty, side.front().qty - hit.qty);
            if (hit.side == Side::No) {
                EXPECT_EQ(step.trade->aggressor, 1);  // bought YES against a NO bid
                EXPECT_EQ(step.trade->yes_price, kPriceOne - hit.price);
            } else {
                EXPECT_EQ(step.trade->aggressor, 2);  // sold YES into a YES bid
                EXPECT_EQ(step.trade->yes_price, hit.price);
            }
            EXPECT_EQ(step.volume, book.volume + step.trade->qty);
            book.volume = step.volume;
        }
        for (const auto& d : step.book) {
            auto& side = d.side == Side::Yes ? book.yes : book.no;
            apply_delta(side, d);
            if (d.action == BookDelta::Action::Delete) {
                EXPECT_EQ(d.qty, 0);
                continue;
            }
            // Absolute quantity and the level it now sits at.
            ASSERT_GE(d.level, 1);
            ASSERT_LE(static_cast<std::size_t>(d.level), side.size());
            EXPECT_EQ(side[static_cast<std::size_t>(d.level) - 1].price, d.price);
            EXPECT_EQ(side[static_cast<std::size_t>(d.level) - 1].qty, d.qty);
        }
        const Instrument& real = *m.find(step.symbol);
        ASSERT_EQ(book.yes, real.yes) << "step " << i;
        ASSERT_EQ(book.no, real.no) << "step " << i;
        expect_sane(real);
    }
    EXPECT_GT(trades, 1000);
}

TEST(SimMarket, SameSeedSameEvents) {
    Market a(specs(), 99, kT0);
    Market b(specs(), 99, kT0);
    for (int i = 0; i < 500; ++i) {
        const auto sa = a.step(kT0);
        const auto sb = b.step(kT0);
        ASSERT_EQ(sa.symbol, sb.symbol);
        ASSERT_EQ(sa.book.size(), sb.book.size());
        for (std::size_t k = 0; k < sa.book.size(); ++k) {
            EXPECT_EQ(sa.book[k].price, sb.book[k].price);
            EXPECT_EQ(sa.book[k].qty, sb.book[k].qty);
        }
    }
}

// Closed, book cleared, Determined, then the next window Listed and Open;
// settlement removes the old instrument [OBS O6].
TEST(SimMarket, RollThenSettle) {
    Market m(specs(), 1, kT0);
    const auto levels = m.find("A")->yes.size() + m.find("A")->no.size();
    const Roll roll = m.roll("A", {"A2", "4", 500, 3}, kT0);

    ASSERT_EQ(roll.closing.size(), 1u);
    EXPECT_EQ(roll.closing[0].status, Status::Closed);
    EXPECT_EQ(roll.cleared.book.size(), levels);
    for (const auto& d : roll.cleared.book) EXPECT_EQ(d.action, BookDelta::Action::Delete);
    ASSERT_EQ(roll.after.size(), 3u);
    EXPECT_EQ(roll.after[0].symbol, "A");
    EXPECT_EQ(roll.after[0].status, Status::Determined);
    EXPECT_EQ(roll.after[1].symbol, "A2");
    EXPECT_EQ(roll.after[1].status, Status::Listed);
    EXPECT_EQ(roll.after[2].status, Status::Open);

    EXPECT_TRUE(m.find("A")->yes.empty());
    EXPECT_EQ(m.find("A2")->status, Status::Open);
    // A closed instrument gets no more events.
    for (int i = 0; i < 300; ++i) EXPECT_NE(m.step(kT0).symbol, "A");

    EXPECT_EQ(m.settle("A", kT0).status, Status::Settled);
    EXPECT_EQ(m.find("A"), nullptr);
}

// As the sandbox sends them: no trailing zeros [OBS O3/O4].
TEST(SimMarket, WireFormatting) {
    EXPECT_EQ(format_price(440), "0.44");
    EXPECT_EQ(format_price(400), "0.4");
    EXPECT_EQ(format_price(435), "0.435");
    EXPECT_EQ(format_price(5), "0.005");
    EXPECT_EQ(format_price(1000), "1");
    EXPECT_EQ(format_qty(200), "2");
    EXPECT_EQ(format_qty(250), "2.5");
    EXPECT_EQ(format_qty(125), "1.25");
    EXPECT_EQ(format_qty(0), "0");
}

}  // namespace
