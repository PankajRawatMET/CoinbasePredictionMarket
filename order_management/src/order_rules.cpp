#include "cpm/oms/order_rules.hpp"

#include <algorithm>
#include <stdexcept>
#include <thread>

namespace cpm::oms {
namespace {

// Order prices exclude 0 and 1: only a settlement value may equal either.
bool price_in_range(Price price) { return price > 0 && price < kPriceOne; }

bool printable(const std::string& text) {
    return std::all_of(text.begin(), text.end(),
                       [](unsigned char c) { return c >= 0x20 && c <= 0x7E; });
}

bool alnum_or_dash(const std::string& text) {
    return std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-';
    });
}

bool lower_hex(const std::string& text) {
    return std::all_of(text.begin(), text.end(),
                       [](unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

// 8-4-4-4-12 lowercase hex. The spec also says "variant 2"; the variant bits
// are not checked here, because a wrong variant is the venue's business and we
// would rather not refuse a name it would accept.
bool canonical_uuid(const std::string& text) {
    static constexpr std::size_t kDashes[] = {8, 13, 18, 23};
    for (const auto position : kDashes) {
        if (text[position] != '-') return false;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) continue;
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

}  // namespace

std::optional<std::string> check_subaccount(const std::string& subaccount) {
    const auto size = subaccount.size();
    if (size == 0) return "PartyID(448) sub-account must not be empty";
    if (size <= 18) {
        if (!printable(subaccount)) return "a sub-account of 18 or fewer characters must be printable ASCII";
        return std::nullopt;
    }
    if (size <= 20) {
        if (!alnum_or_dash(subaccount)) {
            return "a sub-account of 19-20 characters may only use alphanumerics and '-'";
        }
        return std::nullopt;
    }
    if (size <= 31) {
        if (!lower_hex(subaccount)) {
            return "a sub-account of 21-31 characters must be lowercase hex (0-9, a-f)";
        }
        if (subaccount.front() == '0') {
            return "a sub-account of 21-31 characters must not have a leading '0'";
        }
        return std::nullopt;
    }
    if (size == 36) {
        if (!canonical_uuid(subaccount)) {
            return "a 36-character sub-account must be a lowercase canonical UUID";
        }
        return std::nullopt;
    }
    return "a sub-account of " + std::to_string(size) +
           " characters is invalid: valid lengths are 1-31 and exactly 36";
}

std::optional<std::string> check_cl_ord_id(const std::string& cl_ord_id) {
    if (cl_ord_id.empty()) return "ClOrdID(11) is required";
    if (cl_ord_id.size() > 16) {
        return "ClOrdID(11) is limited to 16 bytes, got " + std::to_string(cl_ord_id.size());
    }
    for (const unsigned char c : cl_ord_id) {
        if (c < 0x20 || c > 0x7E) return "ClOrdID(11) must be printable ASCII";
    }
    return std::nullopt;
}

std::optional<std::string> check_order(const NewOrder& order, const OeConfig& config) {
    if (auto bad = check_cl_ord_id(order.cl_ord_id)) return bad;
    if (order.symbol.empty()) return "Symbol(55) is required";
    // Parties with PartyRole=24 is required on every order, and the gateway
    // answers a missing one with INVALID_SUBACCOUNT.
    if (config.subaccount.empty()) {
        return "no sub-account configured: Parties/PartyRole=24 is required on every order "
               "(set CPM_OE_SUBACCOUNT). The venue creates one lazily on first use, so any "
               "well-formed name works";
    }
    if (auto bad = check_subaccount(config.subaccount)) return *bad;
    if (order.quantity <= 0) return "OrderQty(38) must be positive";

    if (order.type == OrderType::Limit) {
        if (!price_in_range(order.price)) {
            return "Price(44) must be between 0.001 and 0.999 for a limit order, got " +
                   format_price(order.price);
        }
    } else {
        // A market order takes the worst admissible price and is forced to IOC,
        // so a price on it is a rejection, not a hint.
        if (order.price != 0) return "Price(44) must be omitted for a market order";
        // Aggressive by construction, so post-only can never be satisfied.
        if (order.post_only != PostOnly::None) {
            return "a market order cannot be post-only (POST_ONLY_WOULD_CROSS)";
        }
    }

    if (order.max_execution_cost) {
        if (order.time_in_force != TimeInForce::Fok) {
            return "MaxExecutionCost(21009) requires TimeInForce(59)=FOK";
        }
        if (*order.max_execution_cost <= 0) return "MaxExecutionCost(21009) must be positive";
    }
    if (order.smp_id && *order.smp_id <= 0) {
        return "SelfMatchPreventionId(7928) must be positive; omit it to opt out";
    }
    return std::nullopt;
}

std::optional<std::string> check_cancel(const CancelOrder& cancel) {
    if (auto bad = check_cl_ord_id(cancel.cl_ord_id)) return bad;
    if (cancel.orig_cl_ord_id.empty() && cancel.order_id.empty()) {
        return "a cancel needs OrigClOrdID(41) or OrderID(37)";
    }
    if (!cancel.orig_cl_ord_id.empty()) {
        if (auto bad = check_cl_ord_id(cancel.orig_cl_ord_id)) return "Orig" + *bad;
    }
    if (cancel.symbol.empty()) return "Symbol(55) is required on a cancel";
    return std::nullopt;
}

std::optional<std::string> check_mass_cancel(const MassCancel& request) {
    if (auto bad = check_cl_ord_id(request.cl_ord_id)) return bad;
    if (request.scope == MassCancel::Scope::Security && request.symbol.empty()) {
        return "Symbol(55) is required for a per-security mass cancel";
    }
    return std::nullopt;
}

std::string make_cl_ord_id(const std::string& prefix, std::uint64_t counter) {
    static constexpr char kDigits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    std::string suffix;
    do {
        suffix.insert(suffix.begin(), kDigits[counter % 36]);
        counter /= 36;
    } while (counter > 0);
    const std::string id = prefix + suffix;
    if (auto bad = check_cl_ord_id(id)) {
        throw std::invalid_argument("cannot build a ClOrdID from prefix '" + prefix + "': " + *bad);
    }
    return id;
}

bool wait_for(const std::function<bool()>& ready, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (ready()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return ready();
}

}  // namespace cpm::oms
