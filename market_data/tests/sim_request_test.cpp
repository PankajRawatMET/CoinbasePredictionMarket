// MarketDataRequest validation: spec §4 and the §4.3 reject table.
#include "cpm/md/sim/request.hpp"

#include <gtest/gtest.h>

namespace {

using namespace cpm::md::sim;

RequestFields book(std::optional<std::string> depth = "10") {
    return {"r1", "1", depth, {"0", "1"}, 1, {"SYM"}};
}

RequestOutcome run(const RequestFields& r, bool req_active = false) {
    return validate_request(
        r, [](const std::string& s) { return s == "SYM" || s == "SYM2"; },
        [&](const std::string&) { return req_active; });
}

void expect_reject(const RequestOutcome& out, char reason, const std::string& text) {
    const auto* rej = std::get_if<Reject>(&out);
    ASSERT_NE(rej, nullptr);
    EXPECT_EQ(rej->reason, reason);
    EXPECT_EQ(rej->text, text);
}

TEST(SimRequest, BookSubscribe) {
    const auto out = run(book());
    const auto* sub = std::get_if<Subscription>(&out);
    ASSERT_NE(sub, nullptr);
    EXPECT_EQ(sub->feed, Feed::Book);
    EXPECT_EQ(sub->depth, 10);
    EXPECT_EQ(sub->entry_types, (std::set<char>{'0', '1'}));
    EXPECT_TRUE(sub->covers("SYM"));
    EXPECT_FALSE(sub->covers("SYM2"));
}

TEST(SimRequest, AllInstrumentChannels) {
    for (const auto& [types, feed] : std::vector<std::pair<std::vector<std::string>, Feed>>{
             {{"2"}, Feed::Trades}, {{"B", "C"}, Feed::VolumeOi}, {{"L"}, Feed::Lifecycle}}) {
        const auto out = run({"r", "1", std::nullopt, types, 0, {}});
        const auto* sub = std::get_if<Subscription>(&out);
        ASSERT_NE(sub, nullptr);
        EXPECT_EQ(sub->feed, feed);
        EXPECT_TRUE(sub->covers("anything"));
    }
}

TEST(SimRequest, RejectTable) {
    // 281=0 texts are the sandbox's own [OBS O6].
    auto unknown = book();
    unknown.symbols = {"NOPE"};
    expect_reject(run(unknown), '0', "unknown symbol=NOPE");
    expect_reject(run({"r9", "2", "10", {"0", "1"}, 1, {"SYM"}}), '0', "no subscription for mdReqId=r9");
    expect_reject(run(book(), true), '1', "duplicate mdReqId=r1");

    auto bad_type = book();
    bad_type.subscription_type = "0";
    expect_reject(run(bad_type), '4', "unsupported SubscriptionRequestType=0");

    expect_reject(run(book(std::nullopt)), '5', "MarketDepth required for order book");
    expect_reject(run(book("0")), '5', "unsupported MarketDepth=0");  // design Q3
    expect_reject(run({"r", "1", "1", {"2"}, 0, {}}), '5', "MarketDepth not allowed for this channel");

    expect_reject(run({"r", "1", std::nullopt, {}, 0, {}}), '8', "no MDEntryType requested");
    expect_reject(run({"r", "1", std::nullopt, {"Z"}, 0, {}}), '8', "unsupported MDEntryType=Z");
    expect_reject(run({"r", "1", std::nullopt, {"2", "B"}, 0, {}}), '8',
                  "MDEntryTypes from different channels in one request");

    auto no_146 = book();
    no_146.no_related_sym.reset();
    expect_reject(run(no_146), '9', "NoRelatedSym required");
    auto all_book = book();
    all_book.no_related_sym = 0;
    all_book.symbols.clear();
    expect_reject(run(all_book), '9', "order book requires instruments");
    auto mismatch = book();
    mismatch.no_related_sym = 2;
    expect_reject(run(mismatch), '9', "NoRelatedSym does not match the instruments listed");
}

TEST(SimRequest, UnsubscribeActive) {
    const auto out = run({"r1", "2", "10", {"0", "1"}, 1, {"SYM"}}, true);
    const auto* unsub = std::get_if<Unsubscribe>(&out);
    ASSERT_NE(unsub, nullptr);
    EXPECT_EQ(unsub->req_id, "r1");
}

}  // namespace
