#include "cpm/fix_common/logon_auth.hpp"

#include "cpm/common/signing.hpp"

#include <quickfix/FixFieldNumbers.h>
#include <quickfix/Message.h>

#include <gtest/gtest.h>

namespace {

using cpm::fix::align_sending_time;

TEST(AlignSendingTime, TruncatesToMillisecondsAtEveryPrecision) {
    EXPECT_EQ(align_sending_time("20261023-05:41:10.414987").signed_, "20261023-05:41:10.414");
    EXPECT_EQ(align_sending_time("20261023-05:41:10.414987").wire, "20261023-05:41:10.414000");
    EXPECT_EQ(align_sending_time("20261023-05:41:10.414987123").signed_, "20261023-05:41:10.414");
    EXPECT_EQ(align_sending_time("20261023-05:41:10.414").wire, "20261023-05:41:10.414000");
    EXPECT_EQ(align_sending_time("20261023-05:41:10.4").signed_, "20261023-05:41:10.400");
    EXPECT_EQ(align_sending_time("20261023-05:41:10").signed_, "20261023-05:41:10.000");
}

TEST(AlignSendingTime, SignedFormIsExactly21Chars) {
    EXPECT_EQ(align_sending_time("20261023-05:41:10.414987").signed_.size(), 21u);
}

TEST(AlignSendingTime, RejectsMalformed) {
    for (const char* bad : {"", "2026-10-23 05:41:10", "20261023-05:41:10.", "20261023-05:41:1x.123",
                            "20261023-05:41:10,123"}) {
        EXPECT_THROW(align_sending_time(bad), std::invalid_argument) << bad;
    }
}

TEST(ApplyLogonAuth, SetsPasswordSignatureAndAlignedTime) {
    FIX::Message logon;
    auto& h = logon.getHeader();
    h.setField(FIX::FIELD::MsgType, "A");
    h.setField(FIX::FIELD::SenderCompID, "test-key");
    h.setField(FIX::FIELD::TargetCompID, "CPMMD");
    h.setField(FIX::FIELD::SendingTime, "20240701-12:00:00.000987");

    const cpm::Credentials creds{"test-key", "test-pass", "dGVzdC1zZWNyZXQ="};
    cpm::fix::apply_logon_auth(logon, creds);

    EXPECT_EQ(h.getField(FIX::FIELD::SendingTime), "20240701-12:00:00.000000");
    EXPECT_EQ(logon.getField(FIX::FIELD::Password), "test-pass");
    // Same vector as common/tests/signing_test.cpp (computed independently in Python).
    EXPECT_EQ(logon.getField(FIX::FIELD::Text), "G8fycaXicUN0zv3qTvXGXnCzSYzjywJTktUByxRoYzk=");
}

}  // namespace
