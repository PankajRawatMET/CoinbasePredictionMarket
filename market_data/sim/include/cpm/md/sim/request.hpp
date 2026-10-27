#pragma once

#include <functional>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

// MarketDataRequest(V) validation for md_sim: spec §4 and the §4.3 reject table.
// Plain C++; the FIX layer fills RequestFields from the message.
namespace cpm::md::sim {

enum class Feed { Book, Trades, VolumeOi, Lifecycle };

struct RequestFields {
    std::string req_id;                  // 262
    std::string subscription_type;       // 263
    std::optional<std::string> depth;    // 264
    std::vector<std::string> entry_types;  // 267 group, 269 values
    std::optional<int> no_related_sym;   // 146 (absent = nullopt)
    std::vector<std::string> symbols;    // 146 group, 55 values
};

struct Subscription {
    std::string req_id;
    Feed feed = Feed::Book;
    std::set<char> entry_types;    // e.g. {'0','1'} or {'B','C'}
    std::set<std::string> symbols;  // empty = all instruments (146=0)
    int depth = 0;                 // Book only: 1 or 10

    bool covers(const std::string& symbol) const { return symbols.empty() || symbols.count(symbol) != 0; }
};

struct Unsubscribe {
    std::string req_id;
};

struct Reject {
    char reason = '0';  // MDReqRejReason(281)
    std::string text;   // Text(58)
};

using RequestOutcome = std::variant<Subscription, Unsubscribe, Reject>;

// known(symbol): the instrument exists. active(req_id): a subscription with
// that MDReqID is active on the session.
RequestOutcome validate_request(const RequestFields& request,
                                const std::function<bool(const std::string&)>& known,
                                const std::function<bool(const std::string&)>& active);

}  // namespace cpm::md::sim
