// Loads config/FIXT11-CPMOE.xml and config/FIX50SP2-CPMOE.xml into QuickFIX and
// validates the order entry message catalog: the spec's own worked examples
// (docs/spec/fix-order-entry.txt "Message Examples"), one message of every type
// in the catalog, and the shapes the spec says must be refused.
// Credentials in the examples are replaced with test values.
#include "cpm/fix_common/session_settings.hpp"

#include "fix_test_util.hpp"

#include <quickfix/DataDictionary.h>
#include <quickfix/Message.h>

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using cpm::fix::test::build_fix;
using Fields = std::vector<std::pair<int, std::string>>;

class OeDictionaries : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        transport_ = new FIX::DataDictionary(std::string(CPM_DICT_DIR) + "/" + cpm::fix::kOeTransportDictionary);
        app_ = new FIX::DataDictionary(std::string(CPM_DICT_DIR) + "/" + cpm::fix::kOeAppDictionary);
    }
    static void TearDownTestSuite() {
        delete transport_;
        delete app_;
    }

    // Parses and validates the way FIX::Session does: admin messages against
    // the transport dictionary, application messages against both.
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

// A 44-character base64 string, the shape the spec requires of the signature
// (base64 of a 32-byte HMAC-SHA256 digest). The length matters for RawData(96):
// it is a DATA field, so RawDataLength(95) must be its exact byte count or the
// parse runs off the end of the field.
const std::string kSignature = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=";

const Fields kToServer = {{49, "test-key"}, {56, "CPMOE"}};
const Fields kToClient = {{49, "CPMOE"}, {56, "test-key"}};

std::string msg(const std::string& type, const Fields& route, Fields body, int seq = 2) {
    Fields f{{35, type}};
    f.insert(f.end(), route.begin(), route.end());
    f.push_back({34, std::to_string(seq)});
    f.push_back({52, "20240701-12:00:01.000000"});
    f.insert(f.end(), body.begin(), body.end());
    return build_fix(f);
}

// The sub-account group every order must carry: PartyID first, since it is the
// group delimiter, then PartyRole=24.
const Fields kSubAccount = {{453, "1"}, {448, "my-subaccount"}, {452, "24"}};

// --- the spec's worked examples -------------------------------------------

TEST_F(OeDictionaries, LogonAsTheSpecDescribesIt) {
    // Spec Logon table: 98, 108, 141, 1137, 554, signature in 58, plus the two
    // CPM session options. The API key travels in SenderCompID(49).
    expect_valid(msg("A", kToServer,
                     {{98, "0"}, {108, "30"}, {141, "Y"}, {1137, "9"}, {554, "test-passphrase"},
                      {58, kSignature}, {20127, "N"}, {8013, "A"}},
                     1));
    // The signature may arrive in RawData(96) instead.
    expect_valid(msg("A", kToServer,
                     {{98, "0"}, {108, "30"}, {1137, "9"}, {554, "test-passphrase"},
                      {95, std::to_string(kSignature.size())}, {96, kSignature}},
                     1));
    // All three cancel-on-disconnect scopes.
    for (const char* scope : {"N", "Y", "A"}) {
        expect_valid(msg("A", kToServer,
                         {{98, "0"}, {108, "30"}, {1137, "9"}, {554, "p"}, {58, "s"}, {8013, scope}}, 1));
    }
}

// OE-Q3: the spec's Logon example sends 20001=my-access-key, which its own
// Logon field table and the text "not in a Logon-body field" exclude. Our
// dictionaries follow the normative text, so the example as printed is refused.
TEST_F(OeDictionaries, TheSpecsLogonExampleCarriesAnUndefinedTag) {
    expect_invalid(msg("A", kToServer,
                       {{98, "0"}, {108, "30"}, {141, "Y"}, {1137, "9"}, {20001, "my-access-key"},
                        {554, "test-passphrase"}, {58, "sig"}, {20127, "N"}},
                       1));
}

TEST_F(OeDictionaries, NewOrderSingleExample) {
    // 11=order-001|55=MKT-A|54=1|38=1.00|44=0.650|40=2|59=1|1028=N|453=1|448=..|452=24
    Fields body{{11, "order-001"}, {55, "MKT-A"},  {54, "1"}, {38, "1.00"},
                {44, "0.650"},     {40, "2"},      {59, "1"}, {1028, "N"}};
    body.insert(body.end(), kSubAccount.begin(), kSubAccount.end());
    expect_valid(msg("D", kToServer, body));
}

TEST_F(OeDictionaries, ExecutionReportNewExample) {
    expect_valid(msg("8", kToClient,
                     {{11, "order-001"}, {37, "1001"}, {17, "150"}, {150, "0"}, {39, "0"}, {55, "MKT-A"},
                      {54, "1"}, {38, "1.00"}, {44, "0.650"}, {40, "2"}, {59, "1"}, {151, "1.00"},
                      {14, "0.00"}, {6, "0.000"}, {60, "20240701-12:00:01.001200"}, {1028, "N"}},
                     1));
}

TEST_F(OeDictionaries, ExecutionReportTradeFillExample) {
    // Includes the fee group and a maker rebate (negative MiscFeeAmt).
    expect_valid(msg("8", kToClient,
                     {{11, "order-001"}, {37, "1001"}, {17, "181"}, {150, "F"}, {39, "2"}, {55, "MKT-A"},
                      {54, "1"}, {32, "1.00"}, {31, "0.650"}, {151, "0.00"}, {14, "1.00"}, {6, "0.650"},
                      {880, "3391027"}, {1057, "N"}, {1028, "N"},
                      {136, "1"}, {137, "-0.325000"}, {138, "USDC"}, {139, "4"},
                      {60, "20240701-12:01:00.004800"}}));
}

TEST_F(OeDictionaries, OrderCancelRequestExample) {
    expect_valid(msg("F", kToServer,
                     {{11, "cancel-001"}, {41, "order-001"}, {55, "MKT-A"}, {54, "1"}, {1028, "N"}}, 3));
}

// --- one message of every type in the catalog ------------------------------

TEST_F(OeDictionaries, EveryClientMessageType) {
    Fields order{{11, "o1"}, {55, "MKT-A"}, {54, "2"}, {38, "2.50"}, {44, "0.400"}, {40, "2"},
                 {59, "4"},  {18, "A"},     {21009, "1.250000"},     {7928, "7"},   {8000, "O"},
                 {1028, "N"}};
    order.insert(order.end(), kSubAccount.begin(), kSubAccount.end());
    expect_valid(msg("D", kToServer, order));  // FOK, post-only reject, SMP, spend cap

    // A market order: no Price(44), forced IOC by the gateway.
    Fields market{{11, "o2"}, {55, "MKT-A"}, {54, "1"}, {38, "1.00"}, {40, "1"}, {1028, "Y"}};
    market.insert(market.end(), kSubAccount.begin(), kSubAccount.end());
    expect_valid(msg("D", kToServer, market));

    // Cancel by exchange OrderID instead of OrigClOrdID.
    expect_valid(msg("F", kToServer, {{11, "c1"}, {37, "1001"}, {55, "MKT-A"}, {54, "1"}, {1028, "N"}}));

    // Mass cancel: per security, and everything.
    expect_valid(msg("q", kToServer, {{11, "m1"}, {530, "1"}, {55, "MKT-A"}, {54, "1"}}));
    expect_valid(msg("q", kToServer, {{11, "m2"}, {530, "7"}}));
    Fields scoped{{11, "m3"}, {530, "6"}};
    scoped.insert(scoped.end(), kSubAccount.begin(), kSubAccount.end());
    expect_valid(msg("q", kToServer, scoped));

    expect_valid(msg("F1", kToServer, {{112, "ignored"}}));
    expect_valid(msg("F1", kToServer, {}));
    expect_valid(msg("F3", kToServer, {{22003, "120"}}));
    expect_valid(msg("F3", kToServer, {{22003, "120"}, {22004, "150"}}));
}

TEST_F(OeDictionaries, EveryServerMessageType) {
    // Rejected order: OrdRejReason and a reason label.
    expect_valid(msg("8", kToClient,
                     {{11, "o1"}, {37, "0"}, {17, "200"}, {150, "8"}, {39, "8"}, {55, "MKT-A"}, {54, "1"},
                      {151, "0.00"}, {14, "0.00"}, {103, "3"},
                      {58, "MAX_EXECUTION_COST_EXCEEDED - insufficient liquidity"}, {1028, "N"},
                      {60, "20240701-12:00:02.000000"}}));
    // Unsolicited cancel from self-match prevention.
    expect_valid(msg("8", kToClient,
                     {{11, "o1"}, {37, "1001"}, {17, "201"}, {150, "4"}, {39, "4"}, {55, "MKT-A"}, {54, "1"},
                      {151, "0.00"}, {14, "0.00"}, {378, "7"}, {58, "SELF_MATCH_PREVENTION"}, {1028, "N"},
                      {60, "20240701-12:00:03.000000"}}));
    // Speed-bumped order, and an IOC remainder expiring.
    expect_valid(msg("8", kToClient,
                     {{11, "o2"}, {37, "1002"}, {17, "202"}, {150, "A"}, {39, "A"}, {55, "MKT-A"}, {54, "1"},
                      {151, "1.00"}, {14, "0.00"}, {1028, "N"}, {60, "20240701-12:00:04.000000"}}));
    expect_valid(msg("8", kToClient,
                     {{11, "o3"}, {37, "1003"}, {17, "203"}, {150, "C"}, {39, "C"}, {55, "MKT-A"}, {54, "2"},
                      {151, "0.00"}, {14, "0.50"}, {58, "TIME_IN_FORCE"}, {1028, "N"},
                      {60, "20240701-12:00:05.000000"}}));
    // The 5-entry Parties group the exchange populates on every report.
    expect_valid(msg("8", kToClient,
                     {{11, "o4"}, {37, "1004"}, {17, "204"}, {150, "0"}, {39, "0"}, {55, "MKT-A"}, {54, "1"},
                      {151, "1.00"}, {14, "0.00"}, {1028, "N"}, {60, "20240701-12:00:06.000000"},
                      {453, "5"}, {448, "FCM1"}, {452, "4"}, {448, "FIRM1"}, {452, "1"},
                      {448, "sub-1"}, {452, "24"}, {448, "trader-1"}, {452, "11"},
                      {448, "test-key"}, {452, "55"}}));

    // Cancel reject: OrdStatus is always U.
    expect_valid(msg("9", kToClient,
                     {{11, "c1"}, {41, "o1"}, {37, "0"}, {39, "U"}, {102, "1"}, {434, "1"},
                      {58, "UNKNOWN_ORDER"}}));
    // Mass cancel report, accepted and rejected.
    expect_valid(msg("r", kToClient,
                     {{11, "m1"}, {1369, "900"}, {530, "7"}, {531, "7"}, {533, "4"},
                      {60, "20240701-12:00:07.000000"}}));
    expect_valid(msg("r", kToClient,
                     {{11, "m2"}, {1369, "0"}, {530, "1"}, {531, "0"}, {532, "1"}, {533, "0"},
                      {60, "20240701-12:00:08.000000"}, {58, "unknown security"}}));
    // Settlement report: 2-entry Parties group, outcome, payout.
    expect_valid(msg("UMS", kToClient,
                     {{20105, "1000"}, {55, "MKT-A"}, {453, "2"}, {448, "FCM1"}, {452, "4"},
                      {448, "FIRM1"}, {452, "1"}, {21052, "Y"}, {730, "1.000"}, {21053, "N"},
                      {21054, "3.00"}, {21055, "0.00"}, {21056, "3.000000"},
                      {60, "20240701-13:00:00.000000"}}));
    // Replay plumbing.
    expect_valid(msg("F2", kToClient, {{45, "7"}, {17, "150"}}));
    expect_valid(msg("F2", kToClient, {{45, "7"}, {17, "0"}}));  // sentinel: nothing retained
    expect_valid(msg("F4", kToClient, {{45, "9"}, {22005, "2"}}));
    expect_valid(msg("F5", kToClient, {{45, "9"}, {22006, "1"}, {58, "BEGIN_EXEC_ID_TOO_SMALL"}}));
    expect_valid(msg("j", kToClient, {{45, "4"}, {372, "D"}, {379, "o9"}, {380, "0"},
                                      {58, "INVALID_FIELD_VALUE: ..."}}));
}

// A replayed event carries PossResend(97)=Y with a fresh MsgSeqNum, and is not
// a PossDupFlag(43) gap fill.
TEST_F(OeDictionaries, ReplayedEventsCarryPossResend) {
    const auto wire = build_fix({{35, "8"}, {49, "CPMOE"}, {56, "test-key"}, {34, "40"}, {97, "Y"},
                                 {52, "20240701-14:00:00.000000"}, {11, "o1"}, {37, "1001"}, {17, "181"},
                                 {150, "F"}, {39, "2"}, {55, "MKT-A"}, {54, "1"}, {32, "1.00"},
                                 {31, "0.650"}, {151, "0.00"}, {14, "1.00"}, {1028, "N"},
                                 {60, "20240701-12:01:00.004800"}});
    expect_valid(wire);
    FIX::Message parsed(wire, *transport_, *app_, true);
    EXPECT_TRUE(parsed.getHeader().isSetField(FIX::FIELD::PossResend));
}

// --- what must be refused -------------------------------------------------

TEST_F(OeDictionaries, MessagesTheSpecSaysAreNotSupported) {
    // No amend: OrderCancelReplaceRequest(G) and OrderStatusRequest(H) are not
    // in the catalog, so our engine rejects them rather than sending them.
    expect_invalid(msg("G", kToServer, {{11, "o1"}, {41, "o0"}, {55, "MKT-A"}, {54, "1"}, {38, "1.00"},
                                        {44, "0.650"}, {40, "2"}, {1028, "N"}}));
    expect_invalid(msg("H", kToServer, {{11, "o1"}, {55, "MKT-A"}, {54, "1"}}));
}

TEST_F(OeDictionaries, StructurallyInvalidOrdersAreRefused) {
    // Missing ClOrdID, Symbol, Side, OrderQty, OrdType or ManualOrderIndicator.
    Fields base{{11, "o1"}, {55, "MKT-A"}, {54, "1"}, {38, "1.00"}, {44, "0.650"}, {40, "2"}, {1028, "N"}};
    for (const int drop : {11, 55, 54, 38, 40, 1028}) {
        Fields body;
        for (const auto& f : base) {
            if (f.first != drop) body.push_back(f);
        }
        body.insert(body.end(), kSubAccount.begin(), kSubAccount.end());
        expect_invalid(msg("D", kToServer, body));
    }
    // The Parties group itself is required.
    expect_invalid(msg("D", kToServer, base));
    // A group count that disagrees with the entries present.
    Fields bad_count{{11, "o1"}, {55, "MKT-A"}, {54, "1"}, {38, "1.00"}, {44, "0.650"}, {40, "2"},
                     {1028, "N"}, {453, "2"}, {448, "sub-1"}, {452, "24"}};
    expect_invalid(msg("D", kToServer, bad_count));
}

TEST_F(OeDictionaries, ValuesOutsideTheEnumsAreRefused) {
    Fields body{{11, "o1"}, {55, "MKT-A"}, {54, "3"}, {38, "1.00"}, {44, "0.650"}, {40, "2"}, {1028, "N"}};
    body.insert(body.end(), kSubAccount.begin(), kSubAccount.end());
    expect_invalid(msg("D", kToServer, body));  // Side=3: there is no NO-side order entry

    expect_invalid(msg("8", kToClient,
                       {{11, "o1"}, {37, "1"}, {17, "1"}, {150, "Z"}, {39, "0"}, {55, "MKT-A"}, {54, "1"},
                        {151, "0.00"}, {14, "0.00"}, {1028, "N"}, {60, "20240701-12:00:00.000000"}}));
    expect_invalid(msg("q", kToServer, {{11, "m1"}, {530, "9"}}));          // unsupported scope
    expect_invalid(msg("F5", kToClient, {{45, "9"}, {22006, "77"}}));       // unknown reject reason
    expect_invalid(msg("A", kToServer, {{98, "0"}, {108, "30"}, {1137, "9"}, {554, "p"}, {58, "s"},
                                        {8013, "Z"}}, 1));                  // bad COD scope
}

TEST_F(OeDictionaries, ExecutionReportNeedsItsQuantities) {
    // LeavesQty(151), CumQty(14), ExecID(17), OrderID(37) and TransactTime(60)
    // are all required: the state machine cannot work without them.
    for (const int drop : {17, 37, 151, 14, 60, 1028}) {
        Fields base{{11, "o1"}, {37, "1001"}, {17, "150"}, {150, "0"}, {39, "0"}, {55, "MKT-A"},
                    {54, "1"},  {151, "1.00"}, {14, "0.00"}, {1028, "N"}, {60, "20240701-12:00:00.000000"}};
        Fields body;
        for (const auto& f : base) {
            if (f.first != drop) body.push_back(f);
        }
        expect_invalid(msg("8", kToClient, body));
    }
}

// Market data tags have no business on this session, and vice versa.
TEST_F(OeDictionaries, TheTwoServicesDictionariesDoNotOverlap) {
    expect_invalid(msg("V", kToServer, {{262, "book-1"}, {263, "1"}}));
    expect_invalid(msg("W", kToClient, {{262, "book-1"}, {55, "MKT-A"}, {268, "0"}}));
}

}  // namespace
