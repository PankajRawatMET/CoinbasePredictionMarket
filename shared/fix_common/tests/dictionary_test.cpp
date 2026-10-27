// Loads config/ dictionaries into QuickFIX and validates real message shapes:
// the spec's own examples (docs/spec/fix-market-data.txt) and a message observed
// on the sandbox. Credentials in observed messages are replaced with test values.
#include "cpm/fix_common/session_settings.hpp"

#include "fix_test_util.hpp"

#include <quickfix/DataDictionary.h>
#include <quickfix/Message.h>

#include <gtest/gtest.h>

namespace {

using cpm::fix::test::build_fix;

class Dictionaries : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        transport_ = new FIX::DataDictionary(std::string(CPM_DICT_DIR) + "/" + cpm::fix::kTransportDictionary);
        app_ = new FIX::DataDictionary(std::string(CPM_DICT_DIR) + "/" + cpm::fix::kAppDictionary);
    }
    static void TearDownTestSuite() {
        delete transport_;
        delete app_;
    }

    // Parses with both dictionaries and validates the way FIX::Session does:
    // admin messages against the transport dictionary only, app messages against both.
    static void parse_and_validate(const std::string& wire) {
        FIX::Message msg(wire, *transport_, *app_, true);
        if (msg.isAdmin()) {
            transport_->validate(msg);
        } else {
            FIX::DataDictionary::validate(msg, transport_, app_);
        }
    }
    static void expect_valid(const std::string& wire) {
        SCOPED_TRACE(wire);
        EXPECT_NO_THROW(parse_and_validate(wire));
    }
    static void expect_invalid(const std::string& wire) {
        SCOPED_TRACE(wire);
        EXPECT_ANY_THROW(parse_and_validate(wire));
    }

    static inline FIX::DataDictionary* transport_ = nullptr;
    static inline FIX::DataDictionary* app_ = nullptr;
};

const std::vector<std::pair<int, std::string>> kToClient = {{49, "CPMMD"}, {56, "test-key"}};
const std::vector<std::pair<int, std::string>> kToServer = {{49, "test-key"}, {56, "CPMMD"}};

std::vector<std::pair<int, std::string>> msg(const std::string& type,
                                             const std::vector<std::pair<int, std::string>>& route,
                                             std::vector<std::pair<int, std::string>> body) {
    std::vector<std::pair<int, std::string>> f{{35, type}};
    f.insert(f.end(), route.begin(), route.end());
    f.push_back({34, "2"});
    f.push_back({52, "20240701-12:00:02.001000"});
    f.insert(f.end(), body.begin(), body.end());
    return f;
}

TEST_F(Dictionaries, LoadWithExpectedVersions) {
    EXPECT_EQ(transport_->getVersion(), "FIXT.1.1");
    // QuickFIX 1.16 builds the version from type/major/minor and ignores servicepack.
    EXPECT_EQ(app_->getVersion(), "FIX.5.0");
}

// Sandbox Logon ack observed 2026-10-23 (carries SenderSubID 50 and 369).
TEST_F(Dictionaries, ObservedSandboxLogonAck) {
    expect_valid(build_fix(msg("A", kToClient, {{50, "TEST"}, {369, "1"}, {98, "0"}, {108, "30"},
                                                {141, "Y"}, {1137, "9"}})));
}

// Spec §3 Logon (with Text(58), non-standard for Logon).
TEST_F(Dictionaries, SpecLogon) {
    expect_valid(build_fix(msg("A", kToServer, {{98, "0"}, {108, "30"}, {141, "Y"}, {1137, "9"},
                                                {554, "my-passphrase"}, {58, "my-signature"}})));
}

// Spec §4 "Subscribe - Order book, top 10" (PDF p.105).
TEST_F(Dictionaries, SpecMarketDataRequestBook) {
    expect_valid(build_fix(msg("V", kToServer, {{262, "req-book-l2-1"}, {263, "1"}, {264, "10"},
                                                {266, "Y"}, {267, "2"}, {269, "0"}, {269, "1"},
                                                {146, "1"}, {55, "NFL-GAMEWINNER-26NOV28LARSF-LAR"}})));
}

// Spec "Subscribe - Public trades", all instruments (146=0, PDF p.110).
TEST_F(Dictionaries, SpecMarketDataRequestAllTrades) {
    expect_valid(build_fix(msg("V", kToServer, {{262, "req-trade-1"}, {263, "1"}, {267, "1"},
                                                {269, "2"}, {146, "0"}})));
}

// Spec "Subscribe - Instrument lifecycle" with the CPM custom MDEntryType L (PDF p.113).
TEST_F(Dictionaries, SpecMarketDataRequestLifecycle) {
    expect_valid(build_fix(msg("V", kToServer, {{262, "req-lifecycle-1"}, {263, "1"}, {267, "1"},
                                                {269, "L"}, {146, "1"},
                                                {55, "NFL-GAMEWINNER-26NOV28LARSF-LAR"}})));
}

// Spec "Order book L2 snapshot" (PDF p.105).
TEST_F(Dictionaries, SpecBookSnapshot) {
    expect_valid(build_fix(msg("W", kToClient,
                               {{262, "req-book-l2-1"}, {55, "NFL-GAMEWINNER-26NOV28LARSF-LAR"},
                                {48, "598471892347"}, {268, "4"},
                                {269, "0"}, {270, "0.580"}, {271, "300.00"}, {1023, "1"},
                                {269, "0"}, {270, "0.560"}, {271, "333.00"}, {1023, "2"},
                                {269, "1"}, {270, "0.360"}, {271, "20.00"}, {1023, "1"},
                                {269, "1"}, {270, "0.340"}, {271, "146.00"}, {1023, "2"}})));
}

// Spec "Order book L2 delta" (PDF p.106): Symbol/SecurityID inside the entry.
TEST_F(Dictionaries, SpecBookDelta) {
    expect_valid(build_fix(msg("X", kToClient,
                               {{262, "req-book-l2-1"}, {268, "1"}, {279, "1"}, {269, "0"},
                                {55, "NFL-GAMEWINNER-26NOV28LARSF-LAR"}, {48, "598471892347"},
                                {270, "0.580"}, {271, "246.00"}, {1023, "2"},
                                {60, "20240701-12:00:05.001800"}})));
}

// Spec "Public trade" (PDF p.108): Symbol/SecurityID at message level (Q6).
TEST_F(Dictionaries, SpecTradeIncremental) {
    expect_valid(build_fix(msg("X", kToClient,
                               {{262, "req-trade-1"}, {55, "NBA-25JAN15-LAL-BOS"}, {48, "398471892345"},
                                {268, "1"}, {279, "0"}, {269, "2"}, {270, "0.620"}, {271, "136.00"},
                                {278, "398471892346"}, {2446, "1"},
                                {60, "20240701-12:01:00.004800"}})));
}

// Spec "Trade volume & open interest snapshot" (PDF p.109): no MDEntryPx.
TEST_F(Dictionaries, SpecVolumeOpenInterestSnapshot) {
    expect_valid(build_fix(msg("W", kToClient,
                               {{262, "req-trade-vol-oi-1"}, {55, "NFL-GAMEWINNER-26NOV28LARSF-LAR"},
                                {48, "598471892347"}, {268, "2"}, {269, "B"}, {271, "333145.00"},
                                {269, "C"}, {271, "152480.00"}})));
}

// Spec vol/OI delta (PDF p.110) in corrected form - see open question Q6(b).
TEST_F(Dictionaries, SpecVolumeOpenInterestDeltaCorrected) {
    expect_valid(build_fix(msg("X", kToClient,
                               {{262, "req-trade-vol-oi-1"}, {268, "2"},
                                {279, "1"}, {269, "B"}, {55, "NFL-GAMEWINNER-26NOV28LARSF-LAR"},
                                {48, "598471892347"}, {271, "200134.00"},
                                {279, "1"}, {269, "C"}, {55, "NFL-GAMEWINNER-26NOV28LARSF-LAR"},
                                {48, "598471892347"}, {271, "152511.00"},
                                {60, "20240701-12:00:05.001800"}})));
}

// Spec SecurityStatus (PDF p.113) for every CPM SecurityTradingStatus value.
TEST_F(Dictionaries, SpecSecurityStatusAllCpmValues) {
    for (const char* status : {"21", "29", "17", "2", "18", "27", "28", "26"}) {
        expect_valid(build_fix(msg("f", kToClient,
                                   {{55, "NFL-GAMEWINNER-26NOV28LARSF-LAR"}, {48, "200001"},
                                    {326, status}, {60, "20240701-12:30:00.001000"}})));
    }
}

TEST_F(Dictionaries, MarketDataRequestRejectAndBusinessReject) {
    expect_valid(build_fix(msg("Y", kToClient, {{262, "req-1"}, {281, "9"}, {58, "146 absent"}})));
    expect_valid(build_fix(msg("j", kToClient, {{45, "3"}, {372, "D"}, {380, "3"}})));
}

TEST_F(Dictionaries, RejectsWhatTheSpecDoesNotDefine) {
    // Order Entry message types are not part of the MD gateway (Q1).
    expect_invalid(build_fix(msg("D", kToServer, {{11, "c1"}})));
    // Unknown SecurityTradingStatus value.
    expect_invalid(build_fix(msg("f", kToClient, {{55, "S"}, {48, "1"}, {326, "99"}})));
    // Missing required MDReqID on a snapshot.
    expect_invalid(build_fix(msg("W", kToClient, {{55, "S"}, {48, "1"}, {268, "1"}, {269, "B"},
                                                  {271, "1.00"}})));
}

}  // namespace
