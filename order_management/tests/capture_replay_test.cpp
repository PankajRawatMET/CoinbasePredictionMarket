// Replays the real order entry captures in docs/spec/ through the decoder.
// Every message here was actually sent by the gateway, which makes this the
// only check that the inbound layouts in the dictionary and the decoder match
// the venue rather than my reading of the spec (OE-O1, OE-O2).
//
// The captures have the API key redacted, so BodyLength and CheckSum no longer
// match and the messages are parsed without session validation. Field content
// and group structure are untouched.
#include "cpm/fix_common/session_settings.hpp"
#include "fix/oe_decoder.hpp"

#include <quickfix/DataDictionary.h>
#include <quickfix/Message.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr char kSoh = '\x01';

using cpm::oms::ExecType;
using cpm::oms::OrdStatus;
using cpm::oms::Side;

struct Replay {
    std::vector<cpm::oms::ExecutionReport> reports;
    std::vector<cpm::oms::CancelReject> cancel_rejects;
    std::vector<cpm::oms::BusinessReject> business_rejects;
    std::vector<std::string> errors;
    std::size_t messages = 0;
};

class OeCaptures : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        transport_ = new FIX::DataDictionary(std::string(CPM_DICT_DIR) + "/" + cpm::fix::kOeTransportDictionary);
        app_ = new FIX::DataDictionary(std::string(CPM_DICT_DIR) + "/" + cpm::fix::kOeAppDictionary);
    }
    static void TearDownTestSuite() {
        delete transport_;
        delete app_;
    }

    static Replay replay(const std::string& file) {
        std::ifstream log(std::string(CPM_CAPTURE_DIR) + "/" + file);
        EXPECT_TRUE(log.is_open()) << "cannot open capture " << file;
        Replay out;
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
            const auto batch = cpm::oms::fix::decode_message(message, "mQlk****");
            for (const auto& error : batch.errors) out.errors.push_back(error.reason + " in " + error.raw);
            out.reports.insert(out.reports.end(), batch.reports.begin(), batch.reports.end());
            out.cancel_rejects.insert(out.cancel_rejects.end(), batch.cancel_rejects.begin(),
                                      batch.cancel_rejects.end());
            out.business_rejects.insert(out.business_rejects.end(), batch.business_rejects.begin(),
                                        batch.business_rejects.end());
        }
        return out;
    }

    static inline FIX::DataDictionary* transport_ = nullptr;
    static inline FIX::DataDictionary* app_ = nullptr;
};

TEST_F(OeCaptures, EveryCaptureDecodesCleanly) {
    for (const char* file : {"capture-oe-20261026T0845-logon.log", "capture-oe-20261026T1018-order.log"}) {
        SCOPED_TRACE(file);
        const auto out = replay(file);
        EXPECT_GT(out.messages, 0u);
        EXPECT_TRUE(out.errors.empty()) << out.errors.front();
    }
}

// OE-O2: one resting limit order, then a cancel. The only live confirmation of
// the ExecutionReport layout we have.
TEST_F(OeCaptures, TheRestingOrderLifecycle) {
    const auto out = replay("capture-oe-20261026T1018-order.log");
    ASSERT_TRUE(out.errors.empty()) << out.errors.front();
    EXPECT_TRUE(out.business_rejects.empty());
    EXPECT_TRUE(out.cancel_rejects.empty());
    ASSERT_EQ(out.reports.size(), 2u);

    // The order was accepted straight to New: no speed bump on this instrument.
    const auto& accepted = out.reports[0];
    EXPECT_EQ(accepted.exec_type, ExecType::New);
    EXPECT_EQ(accepted.status, OrdStatus::New);
    EXPECT_EQ(accepted.cl_ord_id, "t-7nt");
    EXPECT_TRUE(accepted.orig_cl_ord_id.empty());
    EXPECT_EQ(accepted.symbol, "BTCD15M-BTC26OCT261030-0");
    EXPECT_EQ(accepted.side, Side::Buy);
    EXPECT_EQ(accepted.price, 400);             // 44=0.4 at price scale 3
    EXPECT_EQ(accepted.order_quantity, 100);    // 38=1 at quantity scale 2
    EXPECT_EQ(accepted.leaves_quantity, 100);
    EXPECT_EQ(accepted.cumulative_quantity, 0);
    EXPECT_EQ(accepted.average_price, 0) << "AvgPx is emitted as zero before any fill";
    EXPECT_EQ(accepted.post_only, cpm::oms::PostOnly::Reject) << "18=A echoed back";
    EXPECT_EQ(accepted.time_in_force, cpm::oms::TimeInForce::Gtc);
    EXPECT_EQ(accepted.type, cpm::oms::OrderType::Limit);
    EXPECT_FALSE(accepted.manual_order);
    EXPECT_FALSE(accepted.fill.has_value());
    EXPECT_FALSE(accepted.reject_reason.has_value());
    EXPECT_EQ(accepted.cancel_reason, cpm::oms::CancelReason::None);
    EXPECT_FALSE(accepted.replayed);

    // Identifiers are far beyond 32 bits, which is why they stay strings.
    EXPECT_EQ(accepted.order_id, "240418888163000320");
    EXPECT_EQ(accepted.exec_id, "480837776594436096");
    EXPECT_GT(accepted.order_id.size(), 10u);

    // The cancel response names the CANCEL as ClOrdID and the original order as
    // OrigClOrdID, so a state machine has to resolve by OrigClOrdID when set.
    const auto& canceled = out.reports[1];
    EXPECT_EQ(canceled.exec_type, ExecType::Canceled);
    EXPECT_EQ(canceled.status, OrdStatus::Canceled);
    EXPECT_EQ(canceled.cl_ord_id, "c-8");
    EXPECT_EQ(canceled.orig_cl_ord_id, "t-7nt");
    EXPECT_EQ(canceled.order_id, accepted.order_id) << "same order, new ExecID";
    EXPECT_NE(canceled.exec_id, accepted.exec_id);
    EXPECT_EQ(canceled.leaves_quantity, 0);
    EXPECT_EQ(canceled.cumulative_quantity, 0);
    EXPECT_EQ(canceled.text, "USER_REQUEST");
    // 378 is only on unsolicited cancels, so a cancel we asked for has none.
    EXPECT_EQ(canceled.cancel_reason, cpm::oms::CancelReason::None);
    EXPECT_FALSE(canceled.reject_reason.has_value());

    // ExecIDs share one increasing space, so ordering is meaningful.
    EXPECT_LT(std::stoull(accepted.exec_id), std::stoull(canceled.exec_id));
    EXPECT_GT(canceled.transact_time, accepted.transact_time);
}

}  // namespace
