// Replays the real sandbox captures in docs/spec/ through the decoder and the
// book: the strongest check available that Stage 3 and Stage 4 agree with the
// gateway, since every message here was actually sent by it.
//
// The captures have the API key redacted, so BodyLength and CheckSum no longer
// match and the messages are parsed without session validation. Field content,
// group structure and ordering are untouched.
#include "cpm/fix_common/session_settings.hpp"
#include "cpm/md/market_state.hpp"
#include "fix/md_decoder.hpp"

#include <quickfix/DataDictionary.h>
#include <quickfix/Message.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

using cpm::md::BookAnomaly;
using cpm::md::BookSide;
using cpm::md::MarketState;

constexpr char kSoh = '\x01';

struct Replay {
    MarketState state;
    std::size_t messages = 0;  // every inbound message, admin included
    std::size_t decode_errors = 0;
    std::size_t snapshots = 0;
    std::size_t book_updates = 0;
    std::size_t volumes = 0;
    std::size_t open_interest = 0;
    std::size_t trades = 0;
    std::size_t lifecycle_events = 0;
    std::size_t rejects = 0;
    std::vector<std::string> error_reasons;
    std::vector<std::string> anomaly_details;
};

class Captures : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        transport_ = new FIX::DataDictionary(std::string(CPM_DICT_DIR) + "/" + cpm::fix::kTransportDictionary);
        app_ = new FIX::DataDictionary(std::string(CPM_DICT_DIR) + "/" + cpm::fix::kAppDictionary);
    }
    static void TearDownTestSuite() {
        delete transport_;
        delete app_;
    }

    // Every inbound message of one capture, in order, through decode and state.
    // MarketState holds a mutex, so the result is handed back by pointer.
    static std::unique_ptr<Replay> replay(const std::string& file) {
        std::ifstream log(std::string(CPM_CAPTURE_DIR) + "/" + file);
        EXPECT_TRUE(log.is_open()) << "cannot open capture " << file;
        auto result = std::make_unique<Replay>();
        Replay& out = *result;
        out.state.on_anomaly([&out](const BookAnomaly& anomaly) {
            out.anomaly_details.push_back(std::string(to_string(anomaly.kind)) + " on " + anomaly.symbol + ": " +
                                          anomaly.detail);
        });
        const auto callbacks = out.state.callbacks();

        std::string line;
        while (std::getline(log, line)) {
            const auto marker = line.find(" << 8=FIXT.1.1|");
            if (marker == std::string::npos) continue;
            std::string wire = line.substr(marker + 4);
            while (!wire.empty() && (wire.back() == '\n' || wire.back() == '\r')) wire.pop_back();
            std::replace(wire.begin(), wire.end(), '|', kSoh);

            FIX::Message message;
            try {
                message.setString(wire, false, transport_, app_);
            } catch (const std::exception& e) {
                ADD_FAILURE() << "cannot parse " << wire << ": " << e.what();
                continue;
            }
            ++out.messages;
            const auto batch = cpm::md::fix::decode_message(message, "mQlk****");
            out.decode_errors += batch.errors.size();
            for (const auto& error : batch.errors) out.error_reasons.push_back(error.reason + " in " + error.raw);
            out.snapshots += batch.book_snapshots.size();
            out.book_updates += batch.book_updates.size();
            out.volumes += batch.volumes.size();
            out.open_interest += batch.open_interest.size();
            out.trades += batch.trades.size();
            out.lifecycle_events += batch.lifecycle.size();
            out.rejects += batch.rejects.size();

            for (const auto& event : batch.book_snapshots) callbacks.book_snapshot(event);
            for (const auto& event : batch.book_updates) callbacks.book_update(event);
            for (const auto& event : batch.trades) callbacks.trade(event);
            for (const auto& event : batch.volumes) callbacks.volume(event);
            for (const auto& event : batch.open_interest) callbacks.open_interest(event);
            for (const auto& event : batch.lifecycle) callbacks.lifecycle(event);
        }
        return result;
    }

    static std::string first(const std::vector<std::string>& lines) { return lines.empty() ? "" : lines.front(); }

    static inline FIX::DataDictionary* transport_ = nullptr;
    static inline FIX::DataDictionary* app_ = nullptr;
};

// Every capture must decode and apply cleanly. An anomaly here is either a
// gateway change or a bug in the book rules of design §8.
TEST_F(Captures, EveryCaptureDecodesAndAppliesWithoutAnomalies) {
    for (const char* file : {"capture-sandbox-20261023T0925.log", "capture-sandbox-20261023T1018.log",
                             "capture-sandbox-20261023T1117.log", "capture-sandbox-20261023T1144.log",
                             "capture-sandbox-20261026T0502-instruments.log",
                             "capture-sandbox-20261026T0533.log",
                             "capture-sandbox-20261026T0607-roll.log"}) {
        SCOPED_TRACE(file);
        const auto out = replay(file);
        EXPECT_GT(out->messages, 0u);
        EXPECT_EQ(out->decode_errors, 0u) << first(out->error_reasons);
        EXPECT_EQ(out->state.totals().anomalies, 0u) << first(out->anomaly_details);
    }
}

// O9, the capture with real depth: L2 and L1 books on one instrument, plus
// per-instrument and all-instrument trades.
TEST_F(Captures, BookWithDepthRebuildsExactly) {
    const auto out = replay("capture-sandbox-20261026T0533.log");
    const std::string symbol = "BTCD15M-BTC26OCT260545-0";
    const auto view = out->state.instrument(symbol);
    ASSERT_TRUE(view.has_value());

    // The counts the client logged live: 2 W, 1480 X -> 1482 events.
    EXPECT_EQ(out->snapshots, 2u);
    EXPECT_EQ(out->book_updates, 1050u);
    EXPECT_EQ(out->trades, 430u);
    EXPECT_EQ(out->decode_errors, 0u) << first(out->error_reasons);
    EXPECT_EQ(view->security_id, "240346204372729859");
    EXPECT_TRUE(view->book_valid);

    // O7/first-wins: the L2 subscription snapshotted first and keeps the book.
    EXPECT_EQ(view->book_md_req_id, "book-1");
    EXPECT_EQ(out->state.ignored_foreign_snapshots(), 1u);
    EXPECT_GT(out->state.ignored_foreign_updates(), 300u) << "the L1 subscription's duplicate stream";

    // Q7: 1023 was the position after the change on every entry that had one.
    EXPECT_GT(view->book_counters.price_levels_checked, 600u);
    EXPECT_EQ(view->book_counters.count(BookAnomaly::Kind::PriceLevelMismatch), 0u);
    EXPECT_EQ(view->book_counters.anomalies, 0u) << first(out->anomaly_details);

    // Q9: never crossed or locked, and the top of book is the O9 snapshot's
    // 0.500 / 0.480 moved on by 722 entries.
    EXPECT_FALSE(view->crossed_or_locked);
    ASSERT_TRUE(view->implied_yes_ask.has_value());
    EXPECT_LT(*view->implied_yes_ask, cpm::kPriceOne);
    EXPECT_GT(view->yes.size(), 0u);
    EXPECT_LE(view->yes.size(), cpm::md::OrderBook::kMaxLevels);
    EXPECT_LE(view->no.size(), cpm::md::OrderBook::kMaxLevels);
    for (const auto* side : {&view->yes, &view->no}) {
        EXPECT_TRUE(std::is_sorted(side->begin(), side->end(),
                                   [](const auto& a, const auto& b) { return a.price > b.price; }));
    }

    // Trades arrived under both subscriptions, so every one was seen twice
    // except any that fell outside the narrower per-instrument window.
    EXPECT_GT(view->trades, 100u);
    EXPECT_GT(view->duplicate_trades, 100u);
    EXPECT_EQ(view->trades_without_id, 0u);
    ASSERT_TRUE(view->last_trade.has_value());
    EXPECT_NE(view->last_trade->aggressor, cpm::md::Aggressor::None);
    EXPECT_FALSE(view->last_trade->entry_id.empty());

    // The all-instrument trade subscription also brought other instruments,
    // which have no book of their own.
    EXPECT_GT(out->state.symbols().size(), 1u);
    for (const auto& other : out->state.instruments()) {
        if (other.symbol == symbol) continue;
        EXPECT_FALSE(other.book_valid);
        EXPECT_GT(other.trades, 0u);
    }
}

// O10: a 15-minute window closing, with the book cleared by the venue, the
// successor listed, and the old instrument settled a minute later.
TEST_F(Captures, AWindowRollClearsTheBookAndSettlesTheInstrument) {
    const auto out = replay("capture-sandbox-20261026T0607-roll.log");
    const std::string closed = "BTCD15M-BTC26OCT260615-0";
    const std::string next = "BTCD15M-BTC26OCT260630-0";

    EXPECT_EQ(out->decode_errors, 0u) << first(out->error_reasons);
    EXPECT_EQ(out->state.totals().anomalies, 0u) << first(out->anomaly_details);

    const auto view = out->state.instrument(closed);
    ASSERT_TRUE(view.has_value());
    // The venue deleted every resting level around the Closed message, so the
    // book ends exactly empty: proof it never drifted over 1325 updates.
    EXPECT_TRUE(view->book_valid);
    EXPECT_TRUE(view->yes.empty());
    EXPECT_TRUE(view->no.empty());
    EXPECT_FALSE(view->implied_yes_ask.has_value());
    EXPECT_FALSE(view->implied_no_ask.has_value());
    EXPECT_GT(view->book_counters.updates, 1300u);
    EXPECT_GT(view->book_counters.price_levels_checked, 1100u);
    EXPECT_EQ(view->book_counters.count(BookAnomaly::Kind::PriceLevelMismatch), 0u);
    EXPECT_EQ(view->book_counters.count(BookAnomaly::Kind::DeleteOfMissingPrice), 0u);

    // 18 Closed -> 27 Determined -> 28 Settled, each delivered twice (O10).
    EXPECT_EQ(view->status, cpm::md::LifecycleStatus::Settled);
    EXPECT_EQ(view->duplicate_status_events, 3u);
    ASSERT_EQ(view->history.size(), 3u);
    EXPECT_EQ(view->history[0].first, cpm::md::LifecycleStatus::Closed);
    EXPECT_EQ(view->history[1].first, cpm::md::LifecycleStatus::Determined);
    EXPECT_EQ(view->history[2].first, cpm::md::LifecycleStatus::Settled);

    // Trades came under both the per-instrument and the all-instrument
    // subscription, so each was seen twice and counted once.
    EXPECT_EQ(view->trades, view->duplicate_trades);
    EXPECT_GT(view->trades, 200u);

    // The per-instrument subscriptions named the OLD symbol, so the successor
    // arrives only under the all-instrument ones: one copy of everything, and
    // nothing to deduplicate. That asymmetry is the per-subscription delivery
    // model of O7 seen from the other side.
    const auto successor = out->state.instrument(next);
    ASSERT_TRUE(successor.has_value());
    EXPECT_EQ(successor->status, cpm::md::LifecycleStatus::Open);
    EXPECT_FALSE(successor->book_valid) << "its book was never subscribed";
    EXPECT_GT(successor->trades, 0u);
    EXPECT_EQ(successor->duplicate_trades, 0u);
    EXPECT_EQ(successor->duplicate_status_events, 0u);
    ASSERT_EQ(successor->history.size(), 2u);
    EXPECT_EQ(successor->history[0].first, cpm::md::LifecycleStatus::Listed);
    EXPECT_EQ(successor->history[1].first, cpm::md::LifecycleStatus::Open);
}

// O8: volume and open interest with no W at all, on two instruments.
TEST_F(Captures, VolumeAndOpenInterestWithoutASnapshot) {
    const auto out = replay("capture-sandbox-20261026T0502-instruments.log");
    EXPECT_EQ(out->volumes, 64u);  // 64 X, each carrying one B and one C entry
    EXPECT_EQ(out->open_interest, 64u);
    EXPECT_EQ(out->snapshots, 0u) << "the live gateway sends no W here (O8)";
    EXPECT_EQ(out->state.symbols().size(), 2u);
    for (const auto& view : out->state.instruments()) {
        EXPECT_FALSE(view.book_valid) << view.symbol << " has no book channel here";
        ASSERT_TRUE(view.volume.has_value());
        ASSERT_TRUE(view.open_interest.has_value());
        EXPECT_GT(*view.volume, 0);
        EXPECT_GT(*view.open_interest, 0);
        EXPECT_EQ(view.status, cpm::md::LifecycleStatus::Unknown) << "no f in this capture (Q13)";
    }
    const auto btc = out->state.instrument("BTCD15M-BTC26OCT260515-0");
    ASSERT_TRUE(btc.has_value());
    EXPECT_EQ(btc->security_id, "240338655317590016");
    EXPECT_EQ(*btc->volume, 11200);  // 112 contracts, in hundredths
}

// O6: the full lifecycle of 45 rolling instruments, 21 -> 17 -> 18 -> 27 -> 28.
TEST_F(Captures, LifecycleAcrossManyInstruments) {
    const auto out = replay("capture-sandbox-20261023T1117.log");
    EXPECT_EQ(out->lifecycle_events, 225u);
    EXPECT_EQ(out->rejects, 8u);
    EXPECT_GT(out->state.symbols().size(), 40u);

    std::size_t settled = 0;
    std::size_t full_sequences = 0;
    for (const auto& view : out->state.instruments()) {
        EXPECT_EQ(view.amendments, 0u);
        EXPECT_EQ(view.unknown_status_events, 0u);
        if (view.status == cpm::md::LifecycleStatus::Settled) ++settled;
        if (view.history.size() != 5) continue;
        const std::vector<cpm::md::LifecycleStatus> expected{
            cpm::md::LifecycleStatus::Listed, cpm::md::LifecycleStatus::Open, cpm::md::LifecycleStatus::Closed,
            cpm::md::LifecycleStatus::Determined, cpm::md::LifecycleStatus::Settled};
        std::vector<cpm::md::LifecycleStatus> seen;
        for (const auto& [status, time] : view.history) seen.push_back(status);
        if (seen == expected) ++full_sequences;
    }
    EXPECT_GT(settled, 20u);
    EXPECT_GT(full_sequences, 20u) << "21 -> 17 -> 18 -> 27 -> 28, as O6 recorded";
}

// O4: the trade that fully filled a resting NO bid, and the one that reduced a
// YES bid to an absolute size.
TEST_F(Captures, TradesMoveTheBookTheWayO4Recorded) {
    const auto out = replay("capture-sandbox-20261023T1018.log");
    const auto view = out->state.instrument("BTCD15M-BTC26OCT231030-0");
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view->trades, 2u);
    EXPECT_EQ(view->duplicate_trades, 0u);  // one trade subscription only
    EXPECT_EQ(view->book_counters.anomalies, 0u) << first(out->anomaly_details);
    // O4: the window closed during the capture, through to Settled ~60 s later.
    EXPECT_EQ(view->status, cpm::md::LifecycleStatus::Settled);
    ASSERT_EQ(view->history.size(), 3u);
    EXPECT_EQ(view->history[0].first, cpm::md::LifecycleStatus::Closed);
    EXPECT_EQ(view->history[1].first, cpm::md::LifecycleStatus::Determined);
}

}  // namespace
