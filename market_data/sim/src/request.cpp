#include "cpm/md/sim/request.hpp"

namespace cpm::md::sim {
namespace {

// Reject texts: "unknown symbol=" and "no subscription for mdReqId=" are the
// sandbox's own [OBS O6]. The others were never observed and are this
// simulator's wording.
Reject reject(char reason, std::string text) { return Reject{reason, std::move(text)}; }

}  // namespace

RequestOutcome validate_request(const RequestFields& r, const std::function<bool(const std::string&)>& known,
                                const std::function<bool(const std::string&)>& active) {
    if (r.subscription_type != "1" && r.subscription_type != "2") {
        return reject('4', "unsupported SubscriptionRequestType=" + r.subscription_type);
    }
    if (r.subscription_type == "2") {
        // The sandbox accepts an unsubscribe that repeats the original groups
        // and answers nothing [OBS O3]; only the MDReqID matters here.
        if (!active(r.req_id)) return reject('0', "no subscription for mdReqId=" + r.req_id);
        return Unsubscribe{r.req_id};
    }
    if (active(r.req_id)) return reject('1', "duplicate mdReqId=" + r.req_id);

    // 269 -> channel group (spec §4.1). One group per request (design Q4).
    if (r.entry_types.empty()) return reject('8', "no MDEntryType requested");
    Subscription sub;
    sub.req_id = r.req_id;
    std::optional<Feed> feed;
    for (const auto& type : r.entry_types) {
        Feed f;
        if (type == "0" || type == "1") {
            f = Feed::Book;
        } else if (type == "2") {
            f = Feed::Trades;
        } else if (type == "B" || type == "C") {
            f = Feed::VolumeOi;
        } else if (type == "L") {
            f = Feed::Lifecycle;
        } else {
            return reject('8', "unsupported MDEntryType=" + type);
        }
        if (feed && *feed != f) return reject('8', "MDEntryTypes from different channels in one request");
        feed = f;
        sub.entry_types.insert(type[0]);
    }
    sub.feed = *feed;

    // 264: required on book requests (1 or 10), forbidden elsewhere (spec §4.3).
    if (sub.feed == Feed::Book) {
        if (!r.depth) return reject('5', "MarketDepth required for order book");
        if (*r.depth != "1" && *r.depth != "10") return reject('5', "unsupported MarketDepth=" + *r.depth);
        sub.depth = *r.depth == "1" ? 1 : 10;
    } else if (r.depth) {
        return reject('5', "MarketDepth not allowed for this channel");
    }

    // 146: required; 0 = all instruments, not allowed for the book (spec §4).
    if (!r.no_related_sym || *r.no_related_sym < 0) return reject('9', "NoRelatedSym required");
    if (*r.no_related_sym == 0) {
        if (sub.feed == Feed::Book) return reject('9', "order book requires instruments");
        return sub;
    }
    const std::set<std::string> distinct(r.symbols.begin(), r.symbols.end());
    if (static_cast<int>(distinct.size()) != *r.no_related_sym) {
        return reject('9', "NoRelatedSym does not match the instruments listed");
    }
    for (const auto& symbol : r.symbols) {
        if (!known(symbol)) return reject('0', "unknown symbol=" + symbol);
    }
    sub.symbols = distinct;
    return sub;
}

}  // namespace cpm::md::sim
