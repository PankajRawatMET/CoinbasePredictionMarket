#include "cpm/fix_common/raw_log.hpp"

#include "fix_test_util.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <sstream>

namespace {

using cpm::fix::format_raw;
using cpm::fix::test::build_fix;

constexpr const char* kKey = "abcdEFGH12345678";

TEST(FormatRaw, ShowsSohAsPipe) {
    const auto msg = build_fix({{35, "0"}, {49, "CPMMD"}, {56, "x"}, {34, "2"}});
    const auto out = format_raw(msg, "");
    EXPECT_EQ(out.find('\x01'), std::string::npos);
    EXPECT_EQ(out.rfind("8=FIXT.1.1|", 0), 0u);
    EXPECT_EQ(out.back(), '|');
}

TEST(FormatRaw, MasksLogonSecretsAndApiKey) {
    const auto msg = build_fix({{35, "A"}, {49, kKey}, {56, "CPMMD"}, {34, "1"},
                                {52, "20261023-05:41:10.414000"}, {98, "0"}, {108, "30"},
                                {141, "Y"}, {1137, "9"}, {554, "my-passphrase"},
                                {58, "G8fycaXicUN0zv3qTvXGXnCzSYzjywJTktUByxRoYzk="}});
    const auto out = format_raw(msg, kKey);
    EXPECT_NE(out.find("|554=***|"), std::string::npos);
    EXPECT_NE(out.find("|58=***|"), std::string::npos);
    EXPECT_NE(out.find("|49=abcd****|"), std::string::npos);
    EXPECT_EQ(out.find("my-passphrase"), std::string::npos);
    EXPECT_EQ(out.find("G8fycaX"), std::string::npos);
    EXPECT_EQ(out.find(kKey), std::string::npos);
    EXPECT_NE(out.find("|108=30|"), std::string::npos);  // non-secret fields untouched
}

TEST(FormatRaw, KeepsTextOnNonLogonMessages) {
    // Logout reasons in Text(58) are diagnostics, not secrets.
    const auto msg = build_fix({{35, "5"}, {49, "CPMMD"}, {56, kKey}, {34, "3"},
                                {58, "invalid signature"}});
    const auto out = format_raw(msg, kKey);
    EXPECT_NE(out.find("|58=invalid signature|"), std::string::npos);
    EXPECT_NE(out.find("|56=abcd****|"), std::string::npos);
}

TEST(RawLogFactory, PrefixesDirectionAndRedactsEvents) {
    std::ostringstream sink;
    cpm::Logger logger(sink);
    cpm::fix::RawLogFactory factory(logger, kKey);
    std::unique_ptr<FIX::Log, std::function<void(FIX::Log*)>> log(
        factory.create(), [&](FIX::Log* l) { factory.destroy(l); });

    log->onOutgoing(build_fix({{35, "0"}, {49, kKey}}));
    log->onIncoming(build_fix({{35, "0"}, {56, kKey}}));
    log->onEvent(std::string("Created session FIXT.1.1:") + kKey + "->CPMMD");

    const auto out = sink.str();
    EXPECT_NE(out.find(" >> 8=FIXT.1.1|"), std::string::npos);
    EXPECT_NE(out.find(" << 8=FIXT.1.1|"), std::string::npos);
    EXPECT_NE(out.find(" -- Created session FIXT.1.1:abcd****->CPMMD"), std::string::npos);
    EXPECT_EQ(out.find(kKey), std::string::npos);
}

}  // namespace
