// Decoding W, X, f, Y and j into the plain structs of cpm/md/events.hpp.
// The fixtures are messages captured from the sandbox (docs/spec/*.log, listed
// as O3-O7 in docs/open-questions.txt), with the API key replaced by a test
// value and BodyLength/CheckSum recomputed; the layouts the sandbox could not
// show (multi-level L2, volume/OI) follow design §8 and §12.
#include "cpm/fix_common/session_settings.hpp"
#include "fix/md_decoder.hpp"

#include "fix_test_util.hpp"

#include <quickfix/DataDictionary.h>
#include <quickfix/Message.h>

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using cpm::fix::test::build_fix;
using cpm::md::Aggressor;
using cpm::md::BookAction;
using cpm::md::BookSide;
using cpm::md::LifecycleStatus;
using cpm::md::MdReject;
using cpm::md::fix::DecodedBatch;

using Fields = std::vector<std::pair<int, std::string>>;

const std::string kSymbol = "BTCD15M-BTC26OCT231030-0";
const std::string kSecurityId = "239330816306446336";

// A decoded timestamp written back out, so a fixture's 60 can be compared
// with the string it came from.
using cpm::md::to_utc_string;

class Decode : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        transport_ = new FIX::DataDictionary(std::string(CPM_DICT_DIR) + "/" + cpm::fix::kTransportDictionary);
        app_ = new FIX::DataDictionary(std::string(CPM_DICT_DIR) + "/" + cpm::fix::kAppDictionary);
    }
    static void TearDownTestSuite() {
        delete transport_;
        delete app_;
    }

    // One inbound message in the sandbox's header layout [OBS O3].
    static DecodedBatch decode(const std::string& type, const Fields& body) {
        Fields fields{{35, type},      {49, "CPMMD"}, {56, "test-key"}, {34, "2"},
                      {50, "TEST"},    {52, "20261023-10:18:46.363677"}, {369, "5"}};
        fields.insert(fields.end(), body.begin(), body.end());
        FIX::Message message(build_fix(fields), *transport_, *app_, true);
        return cpm::md::fix::decode_message(message, "test-key");
    }

    static Fields instrument() { return {{55, kSymbol}, {48, kSecurityId}}; }

    static inline FIX::DataDictionary* transport_ = nullptr;
    static inline FIX::DataDictionary* app_ = nullptr;
};

// --- snapshots -------------------------------------------------------------

TEST_F(Decode, EmptyBookSnapshot) {
    // 268=0, exactly as the sandbox answers a subscribe to an empty book (O3).
    const auto batch = decode("W", {{262, "book-1"},
                                    {55, kSymbol},
                                    {48, kSecurityId},
                                    {268, "0"},
                                    {60, "20261023-10:16:27.509849"}});
    ASSERT_EQ(batch.errors.size(), 0u) << batch.errors.front().reason;
    ASSERT_EQ(batch.book_snapshots.size(), 1u);
    const auto& book = batch.book_snapshots.front();
    EXPECT_EQ(book.md_req_id, "book-1");
    EXPECT_EQ(book.symbol, kSymbol);
    EXPECT_EQ(book.security_id, kSecurityId);
    EXPECT_TRUE(book.yes.empty());
    EXPECT_TRUE(book.no.empty());
    EXPECT_EQ(to_utc_string(book.transact_time), "20261023-10:16:27.509849");
}

TEST_F(Decode, BookSnapshotKeepsBothSidesBestFirst) {
    const auto batch = decode("W", {{262, "book-1"},
                                    {55, kSymbol},
                                    {48, kSecurityId},
                                    {268, "4"},
                                    {269, "0"}, {270, "0.58"}, {271, "3"}, {1023, "1"},
                                    {269, "0"}, {270, "0.57"}, {271, "10.5"}, {1023, "2"},
                                    {269, "1"}, {270, "0.4"}, {271, "2"}, {1023, "1"},
                                    {269, "1"}, {270, "0.395"}, {271, "7"}, {1023, "2"},
                                    {60, "20261023-10:18:46.357493"}});
    ASSERT_EQ(batch.errors.size(), 0u) << batch.errors.front().reason;
    ASSERT_EQ(batch.book_snapshots.size(), 1u);
    const auto& book = batch.book_snapshots.front();
    // Prices in thousandths, sizes in hundredths (design §8).
    EXPECT_EQ(book.yes, (std::vector<cpm::md::BookLevel>{{580, 300}, {570, 1050}}));
    EXPECT_EQ(book.no, (std::vector<cpm::md::BookLevel>{{400, 200}, {395, 700}}));
    // 0.580 YES bid + 0.400 NO bid = 0.980: not crossed (Q9, checked in Stage 4).
    EXPECT_LT(book.yes.front().price + book.no.front().price, cpm::kPriceOne);
}

TEST_F(Decode, VolumeAndOpenInterestSnapshotIsNotABook) {
    const auto batch = decode("W", {{262, "volume-oi-3"},
                                    {55, kSymbol},
                                    {48, kSecurityId},
                                    {268, "2"},
                                    {269, "B"}, {271, "30.72"},
                                    {269, "C"}, {271, "5"},
                                    {60, "20261023-10:18:46.357493"}});
    ASSERT_EQ(batch.errors.size(), 0u) << batch.errors.front().reason;
    EXPECT_TRUE(batch.book_snapshots.empty());
    ASSERT_EQ(batch.volumes.size(), 1u);
    ASSERT_EQ(batch.open_interest.size(), 1u);
    EXPECT_EQ(batch.volumes.front().contracts, 3072);
    EXPECT_TRUE(batch.volumes.front().snapshot);
    EXPECT_EQ(batch.volumes.front().symbol, kSymbol);
    EXPECT_EQ(batch.open_interest.front().contracts, 500);
    EXPECT_TRUE(batch.open_interest.front().snapshot);
}

// Q15: an empty or zero volume / open interest is not an error.
TEST_F(Decode, VolumeWithoutASizeIsZero) {
    const auto batch = decode("W", {{262, "volume-oi-3"},
                                    {55, kSymbol},
                                    {48, kSecurityId},
                                    {268, "1"},
                                    {269, "B"},
                                    {60, "20261023-10:18:46.357493"}});
    EXPECT_TRUE(batch.errors.empty());
    ASSERT_EQ(batch.volumes.size(), 1u);
    EXPECT_EQ(batch.volumes.front().contracts, 0);
}

// --- incrementals ----------------------------------------------------------

TEST_F(Decode, NewBookLevelFromSandbox) {
    const auto batch = decode("X", {{262, "book-1"},
                                    {268, "1"},
                                    {279, "0"}, {269, "1"}, {55, kSymbol}, {48, kSecurityId},
                                    {1023, "1"}, {270, "0.51"}, {271, "4"},
                                    {60, "20261023-10:18:46.357493"}});
    ASSERT_EQ(batch.errors.size(), 0u) << batch.errors.front().reason;
    ASSERT_EQ(batch.book_updates.size(), 1u);
    const auto& update = batch.book_updates.front();
    EXPECT_EQ(update.md_req_id, "book-1");
    EXPECT_EQ(update.symbol, kSymbol);
    EXPECT_EQ(update.security_id, kSecurityId);
    ASSERT_EQ(update.changes.size(), 1u);
    EXPECT_EQ(update.changes.front(),
              (cpm::md::BookLevelChange{BookSide::No, BookAction::Set, 510, 400, 1, /*was_new=*/true}));
    EXPECT_EQ(to_utc_string(update.transact_time), "20261023-10:18:46.357493");
}

TEST_F(Decode, DeleteCarriesNoPriceLevelAndZeroSize) {
    const auto batch = decode("X", {{262, "book-1"},
                                    {268, "1"},
                                    {279, "2"}, {269, "1"}, {55, kSymbol}, {48, kSecurityId},
                                    {270, "0.51"}, {271, "0"},
                                    {60, "20261023-10:18:48.509090"}});
    ASSERT_EQ(batch.errors.size(), 0u) << batch.errors.front().reason;
    ASSERT_EQ(batch.book_updates.size(), 1u);
    ASSERT_EQ(batch.book_updates.front().changes.size(), 1u);
    EXPECT_EQ(batch.book_updates.front().changes.front(),
              (cpm::md::BookLevelChange{BookSide::No, BookAction::Delete, 510, 0, 0}));
}

// O4: a Change carries the absolute size left at the level (3 -> 2 as 271=2).
TEST_F(Decode, ChangeIsAnAbsoluteSizeNotADelta) {
    const auto batch = decode("X", {{262, "book-1"},
                                    {268, "1"},
                                    {279, "1"}, {269, "0"}, {55, kSymbol}, {48, kSecurityId},
                                    {1023, "1"}, {270, "0.48"}, {271, "2"},
                                    {60, "20261023-10:28:35.501000"}});
    ASSERT_EQ(batch.errors.size(), 0u) << batch.errors.front().reason;
    ASSERT_EQ(batch.book_updates.front().changes.size(), 1u);
    const auto& change = batch.book_updates.front().changes.front();
    EXPECT_EQ(change.action, BookAction::Set);
    EXPECT_FALSE(change.was_new);  // 279=1
    EXPECT_EQ(change.side, BookSide::Yes);
    EXPECT_EQ(change.qty, 200);
}

TEST_F(Decode, OneUpdatePerInstrumentInTheSameMessage) {
    const auto batch = decode("X", {{262, "book-1"},
                                    {268, "3"},
                                    {279, "0"}, {269, "0"}, {55, kSymbol}, {48, kSecurityId},
                                    {1023, "1"}, {270, "0.58"}, {271, "3"},
                                    {279, "2"}, {269, "0"}, {55, "SOLD5M-SOL26OCT231120-0"},
                                    {48, "239345875581665280"}, {270, "0.3"}, {271, "0"},
                                    {279, "0"}, {269, "1"}, {55, kSymbol}, {48, kSecurityId},
                                    {1023, "1"}, {270, "0.4"}, {271, "1"},
                                    {60, "20261023-10:18:46.357493"}});
    ASSERT_EQ(batch.errors.size(), 0u) << batch.errors.front().reason;
    ASSERT_EQ(batch.book_updates.size(), 2u);
    EXPECT_EQ(batch.book_updates[0].symbol, kSymbol);
    EXPECT_EQ(batch.book_updates[0].changes.size(), 2u);
    EXPECT_EQ(batch.book_updates[1].symbol, "SOLD5M-SOL26OCT231120-0");
    EXPECT_EQ(batch.book_updates[1].changes.size(), 1u);
}

TEST_F(Decode, TradeFromSandbox) {
    // O4(a): the aggressor bought YES at 0.49 against a NO bid at 0.51.
    const auto batch = decode("X", {{262, "trades-2"},
                                    {268, "1"},
                                    {279, "0"}, {269, "2"}, {55, kSymbol}, {48, kSecurityId},
                                    {270, "0.49"}, {271, "4"}, {278, "239331719449157632"}, {2446, "1"},
                                    {60, "20261023-10:18:48.509090"}});
    ASSERT_EQ(batch.errors.size(), 0u) << batch.errors.front().reason;
    ASSERT_EQ(batch.trades.size(), 1u);
    const auto& trade = batch.trades.front();
    EXPECT_EQ(trade.md_req_id, "trades-2");
    EXPECT_EQ(trade.symbol, kSymbol);
    EXPECT_EQ(trade.yes_price, 490);
    EXPECT_EQ(cpm::kPriceOne - trade.yes_price, 510);  // the NO price (design §8)
    EXPECT_EQ(trade.qty, 400);
    EXPECT_EQ(trade.aggressor, Aggressor::YesTaker);
    EXPECT_EQ(trade.entry_id, "239331719449157632");
    EXPECT_EQ(to_utc_string(trade.transact_time), "20261023-10:18:48.509090");
    EXPECT_TRUE(batch.book_updates.empty());
}

TEST_F(Decode, TradeAggressorSides) {
    const auto side = [this](const std::string& aggressor) {
        const auto batch = decode("X", {{262, "trades-2"},
                                        {268, "1"},
                                        {279, "0"}, {269, "2"}, {55, kSymbol}, {48, kSecurityId},
                                        {270, "0.48"}, {271, "1"}, {278, "1"}, {2446, aggressor},
                                        {60, "20261023-10:28:35.501000"}});
        return batch.trades.size() == 1u ? batch.trades.front().aggressor : Aggressor::None;
    };
    EXPECT_EQ(side("1"), Aggressor::YesTaker);
    EXPECT_EQ(side("2"), Aggressor::NoTaker);  // O4(b): sold YES = bought NO
    EXPECT_EQ(side("0"), Aggressor::None);     // opening or uncrossing fill
}

TEST_F(Decode, VolumeAndOpenInterestUpdate) {
    const auto batch = decode("X", {{262, "volume-oi-3"},
                                    {268, "2"},
                                    {279, "1"}, {269, "B"}, {55, kSymbol}, {48, kSecurityId}, {271, "12"},
                                    {279, "1"}, {269, "C"}, {55, kSymbol}, {48, kSecurityId}, {271, "4"},
                                    {60, "20261023-10:18:48.509090"}});
    ASSERT_EQ(batch.errors.size(), 0u) << batch.errors.front().reason;
    ASSERT_EQ(batch.volumes.size(), 1u);
    ASSERT_EQ(batch.open_interest.size(), 1u);
    EXPECT_EQ(batch.volumes.front().contracts, 1200);
    EXPECT_FALSE(batch.volumes.front().snapshot);
    EXPECT_EQ(batch.open_interest.front().contracts, 400);
    EXPECT_FALSE(batch.open_interest.front().snapshot);
}

// --- lifecycle, rejects ----------------------------------------------------

TEST_F(Decode, LifecycleStatusesFromSandbox) {
    const auto status = [this](const std::string& code) {
        const auto batch = decode("f", {{55, "SOLD5M-SOL26OCT231120-0"},
                                        {48, "239345875581665280"},
                                        {326, code},
                                        {60, "20261023-11:20:00.272690"}});
        return batch.lifecycle.size() == 1u ? batch.lifecycle.front().status : LifecycleStatus::Unknown;
    };
    EXPECT_EQ(status("21"), LifecycleStatus::Listed);
    EXPECT_EQ(status("17"), LifecycleStatus::Open);
    EXPECT_EQ(status("2"), LifecycleStatus::Pause);
    EXPECT_EQ(status("18"), LifecycleStatus::Closed);
    EXPECT_EQ(status("27"), LifecycleStatus::Determined);
    EXPECT_EQ(status("28"), LifecycleStatus::Settled);
    EXPECT_EQ(status("26"), LifecycleStatus::Amended);
    EXPECT_EQ(status("29"), LifecycleStatus::Delisted);
}

TEST_F(Decode, LifecycleKeepsTheCodeAndTheInstrument) {
    const auto batch = decode("f", {{55, "SOLD5M-SOL26OCT231120-0"},
                                    {48, "239345875581665280"},
                                    {326, "18"},
                                    {60, "20261023-11:20:00.272690"}});
    ASSERT_EQ(batch.errors.size(), 0u) << batch.errors.front().reason;
    ASSERT_EQ(batch.lifecycle.size(), 1u);
    const auto& event = batch.lifecycle.front();
    EXPECT_EQ(event.symbol, "SOLD5M-SOL26OCT231120-0");
    EXPECT_EQ(event.security_id, "239345875581665280");
    EXPECT_EQ(event.status_code, 18);
    EXPECT_EQ(event.status, LifecycleStatus::Closed);
    EXPECT_TRUE(event.md_req_id.empty());  // the sandbox never sends 324 (O3/O6)
    EXPECT_TRUE(event.text.empty());
    EXPECT_EQ(to_utc_string(event.transact_time), "20261023-11:20:00.272690");
}

TEST_F(Decode, MarketDataRequestReject) {
    const auto batch = decode("Y", {{262, "book-1"},
                                    {281, "0"},
                                    {58, "unknown symbol=BTCD15M-BTC26OCT231115-0"}});
    ASSERT_EQ(batch.rejects.size(), 1u);
    const auto& reject = batch.rejects.front();
    EXPECT_EQ(reject.kind, MdReject::Kind::MarketDataRequest);
    EXPECT_EQ(reject.md_req_id, "book-1");
    EXPECT_EQ(reject.reason_code, "0");
    EXPECT_EQ(reject.text, "unknown symbol=BTCD15M-BTC26OCT231115-0");
}

TEST_F(Decode, BusinessMessageReject) {
    const auto batch = decode("j", {{372, "V"}, {380, "0"}, {58, "unsupported message"}});
    ASSERT_EQ(batch.rejects.size(), 1u);
    EXPECT_EQ(batch.rejects.front().kind, MdReject::Kind::BusinessMessage);
    EXPECT_TRUE(batch.rejects.front().md_req_id.empty());
    EXPECT_EQ(batch.rejects.front().reason_code, "0");
    EXPECT_EQ(batch.rejects.front().text, "unsupported message");
}

// --- strictness ------------------------------------------------------------

TEST_F(Decode, MillisecondTimestampsAreAccepted) {
    // Q16: the spec says milliseconds outbound, the sandbox sends microseconds.
    const auto batch = decode("f", {{55, kSymbol}, {48, kSecurityId}, {326, "17"}, {60, "20261023-11:20:00.272"}});
    ASSERT_EQ(batch.lifecycle.size(), 1u);
    EXPECT_EQ(to_utc_string(batch.lifecycle.front().transact_time), "20261023-11:20:00.272000");
}

TEST_F(Decode, MalformedPriceIsReportedAndDropsTheSnapshot) {
    // A snapshot replaces the book, so an unparseable level must not be
    // applied as a partial book.
    const auto batch = decode("W", {{262, "book-1"},
                                    {55, kSymbol},
                                    {48, kSecurityId},
                                    {268, "2"},
                                    {269, "0"}, {270, "0.58"}, {271, "3"}, {1023, "1"},
                                    {269, "0"}, {270, "0.5801"}, {271, "1"}, {1023, "2"},
                                    {60, "20261023-10:18:46.357493"}});
    EXPECT_TRUE(batch.book_snapshots.empty());
    ASSERT_EQ(batch.errors.size(), 1u);
    EXPECT_NE(batch.errors.front().reason.find("MDEntryPx(270)=0.5801"), std::string::npos)
        << batch.errors.front().reason;
    EXPECT_EQ(batch.errors.front().msg_type, "W");
    EXPECT_EQ(batch.errors.front().md_req_id, "book-1");
    // The raw text is kept for the log, with SOH shown as '|'.
    EXPECT_NE(batch.errors.front().raw.find("|270=0.5801|"), std::string::npos) << batch.errors.front().raw;
}

TEST_F(Decode, MalformedSizeDropsOnlyThatChange) {
    const auto batch = decode("X", {{262, "book-1"},
                                    {268, "2"},
                                    {279, "0"}, {269, "0"}, {55, kSymbol}, {48, kSecurityId},
                                    {1023, "1"}, {270, "0.58"}, {271, "3.001"},
                                    {279, "0"}, {269, "1"}, {55, kSymbol}, {48, kSecurityId},
                                    {1023, "1"}, {270, "0.4"}, {271, "1"},
                                    {60, "20261023-10:18:46.357493"}});
    ASSERT_EQ(batch.errors.size(), 1u);
    EXPECT_NE(batch.errors.front().reason.find("MDEntrySize(271)=3.001"), std::string::npos);
    ASSERT_EQ(batch.book_updates.size(), 1u);
    ASSERT_EQ(batch.book_updates.front().changes.size(), 1u);
    EXPECT_EQ(batch.book_updates.front().changes.front().side, BookSide::No);
}

// The spec marks 60 optional (MD:311/364/782), so its absence is not an error;
// the event's time stays at the epoch.
TEST_F(Decode, MissingTransactTimeIsAllowed) {
    const auto batch = decode("X", {{262, "book-1"},
                                    {268, "1"},
                                    {279, "0"}, {269, "0"}, {55, kSymbol}, {48, kSecurityId},
                                    {1023, "1"}, {270, "0.58"}, {271, "3"}});
    EXPECT_TRUE(batch.errors.empty());
    ASSERT_EQ(batch.book_updates.size(), 1u);
    EXPECT_EQ(batch.book_updates.front().transact_time, cpm::md::Clock::time_point{});
    EXPECT_EQ(batch.book_updates.front().changes.front().price, 580);
}

TEST_F(Decode, MalformedTransactTimeIsAnError) {
    const auto batch = decode("f", {{55, kSymbol}, {48, kSecurityId}, {326, "17"}, {60, "2026-10-23 11:20:00"}});
    EXPECT_TRUE(batch.lifecycle.empty());
    ASSERT_EQ(batch.errors.size(), 1u);
    EXPECT_NE(batch.errors.front().reason.find("malformed TransactTime(60)"), std::string::npos);
}

TEST_F(Decode, EntryTypeTheClientDoesNotSubscribeToIsAnError) {
    // 269=L is the lifecycle channel: it arrives as f, never as a W entry.
    const auto batch = decode("W", {{262, "book-1"},
                                    {55, kSymbol},
                                    {48, kSecurityId},
                                    {268, "1"},
                                    {269, "L"}, {271, "0"},
                                    {60, "20261023-10:18:46.357493"}});
    EXPECT_TRUE(batch.book_snapshots.empty());
    ASSERT_EQ(batch.errors.size(), 1u);
    EXPECT_NE(batch.errors.front().reason.find("MDEntryType(269)=L"), std::string::npos);
}

TEST_F(Decode, SnapshotWithoutASymbolIsAnError) {
    const auto batch = decode("W", {{262, "book-1"}, {268, "0"}, {60, "20261023-10:18:46.357493"}});
    EXPECT_TRUE(batch.book_snapshots.empty());
    ASSERT_EQ(batch.errors.size(), 1u);
    EXPECT_NE(batch.errors.front().reason.find("Symbol(55)"), std::string::npos);
}

TEST_F(Decode, UndefinedTradingStatusIsReportedAndKeptAsUnknown) {
    const auto batch = decode("f", {{55, kSymbol}, {48, kSecurityId}, {326, "99"}, {60, "20261023-11:20:00.272690"}});
    ASSERT_EQ(batch.lifecycle.size(), 1u);
    EXPECT_EQ(batch.lifecycle.front().status, LifecycleStatus::Unknown);
    EXPECT_EQ(batch.lifecycle.front().status_code, 99);
    ASSERT_EQ(batch.errors.size(), 1u);
    EXPECT_NE(batch.errors.front().reason.find("SecurityTradingStatus(326)=99"), std::string::npos);
}

TEST_F(Decode, UnhandledMessageTypeDecodesToNothing) {
    const auto batch = decode("V", {{262, "book-1"}, {263, "1"}});
    EXPECT_TRUE(batch.empty());
}

TEST_F(Decode, RawTextInErrorsHasTheApiKeyRedacted) {
    const auto batch = decode("W", {{262, "book-1"}, {268, "0"}, {60, "20261023-10:18:46.357493"}});
    ASSERT_EQ(batch.errors.size(), 1u);
    EXPECT_EQ(batch.errors.front().raw.find("test-key"), std::string::npos) << batch.errors.front().raw;
}

}  // namespace
