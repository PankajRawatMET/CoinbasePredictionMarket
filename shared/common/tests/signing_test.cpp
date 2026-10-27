#include "cpm/common/signing.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

std::vector<std::uint8_t> bytes(std::string_view s) { return {s.begin(), s.end()}; }

TEST(Base64, EncodesRfc4648Vectors) {
    EXPECT_EQ(cpm::base64_encode(bytes("")), "");
    EXPECT_EQ(cpm::base64_encode(bytes("f")), "Zg==");
    EXPECT_EQ(cpm::base64_encode(bytes("fo")), "Zm8=");
    EXPECT_EQ(cpm::base64_encode(bytes("foo")), "Zm9v");
    EXPECT_EQ(cpm::base64_encode(bytes("foobar")), "Zm9vYmFy");
}

TEST(Base64, RoundTripsAllByteValues) {
    std::vector<std::uint8_t> all(256);
    for (int i = 0; i < 256; ++i) all[i] = static_cast<std::uint8_t>(i);
    EXPECT_EQ(cpm::base64_decode(cpm::base64_encode(all)), all);
}

TEST(Base64, RejectsMalformedInput) {
    EXPECT_THROW(cpm::base64_decode(""), cpm::SigningError);
    EXPECT_THROW(cpm::base64_decode("abc"), cpm::SigningError);    // length
    EXPECT_THROW(cpm::base64_decode("ab$="), cpm::SigningError);   // alphabet
    EXPECT_THROW(cpm::base64_decode("a=bc"), cpm::SigningError);   // padding position
}

// RFC 4231 test case 2 (key "Jefe").
TEST(HmacSha256, MatchesRfc4231) {
    EXPECT_EQ(cpm::hmac_sha256_b64("SmVmZQ==", "what do ya want for nothing?"),
              "W9zBRr9gdU5qBCQmCJV1x1oAPwidJzmDnexYuWTsOEM=");
}

// The spec's worked example (WebSocket "Example Signature Generation", PDF p.34):
// same HMAC-SHA256/Base64 construction as the FIX Logon signature.
TEST(HmacSha256, MatchesSpecWorkedExample) {
    EXPECT_EQ(cpm::hmac_sha256_b64("a2V5", "1717171717fixed-api-keyCPM_WEB_MARKETDATAfixed-passphrase"),
              "xGbr3oY2MKYu/HoFjzTX1i2jhltEx/jGFd//lF+49pw=");
}

TEST(FixLogonSignature, PayloadIsTimeKeyTargetPassphrase) {
    EXPECT_EQ(cpm::fix_logon_payload("20240701-12:00:00.000", "test-key", "CPMMD", "test-pass"),
              "20240701-12:00:00.000test-keyCPMMDtest-pass");
}

TEST(FixLogonSignature, RequiresMillisecondSendingTime) {
    EXPECT_THROW(cpm::fix_logon_payload("20240701-12:00:00.000000", "k", "CPMMD", "p"),
                 cpm::SigningError);
    EXPECT_THROW(cpm::fix_logon_payload("20240701-12:00:00", "k", "CPMMD", "p"), cpm::SigningError);
}

// Expected value computed independently with Python's hmac module.
TEST(FixLogonSignature, KnownVector) {
    const std::string sig = cpm::fix_logon_signature("20240701-12:00:00.000", "test-key", "CPMMD",
                                                     "test-pass", "dGVzdC1zZWNyZXQ=");
    EXPECT_EQ(sig, "G8fycaXicUN0zv3qTvXGXnCzSYzjywJTktUByxRoYzk=");
    EXPECT_EQ(sig.size(), 44u);  // spec: 44-char Base64 of a 32-byte digest
}

}  // namespace
