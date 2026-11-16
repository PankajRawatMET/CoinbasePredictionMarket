#include "cpm/md/ticks.hpp"

#include <algorithm>

namespace cpm::md {

Price tick_at(Price price) {
    // 0.001 ticks below 0.100 and above 0.900, 0.01 between, with 0.100 and
    // 0.900 themselves on the coarse grid (O5).
    return (price < 100 || price > 900) ? 1 : 10;
}

bool is_quotable(Price price) { return price > 0 && price < kPriceOne; }

namespace {

// Built once: 281 entries against a binary search on every quote.
const std::vector<Price>& grid() {
    static const std::vector<Price> prices = price_grid();
    return prices;
}

}  // namespace

Price snap_down(Price price) {
    const auto& prices = grid();
    const auto above = std::upper_bound(prices.begin(), prices.end(), price);
    if (above == prices.begin()) return prices.front();
    return *(above - 1);
}

Price snap_up(Price price) {
    const auto& prices = grid();
    const auto at = std::lower_bound(prices.begin(), prices.end(), price);
    return at == prices.end() ? prices.back() : *at;
}

Price price_below(Price price) { return price <= 0 ? 0 : snap_down(price - 1); }

Price price_above(Price price) { return price >= kPriceOne ? kPriceOne : snap_up(price + 1); }

std::vector<Price> price_grid() {
    std::vector<Price> prices;
    const auto region = [&prices](Price from, Price to, Price step) {
        for (Price price = from; price <= to; price += step) {
            // The regions share their boundaries, so 0.100 and 0.900 are not
            // added twice.
            if (prices.empty() || prices.back() != price) prices.push_back(price);
        }
    };
    // By region, not by walking tick_at: the two answer different questions
    // and differ at exactly the boundaries. tick_at(0.900) is the coarse
    // tick, because that is the distance to the next quotable price *upward
    // from* 0.900 for a strategy stepping through the grid - but 0.901 is
    // still a price an order can rest at, and a ladder that left it out
    // would have nowhere to show one. Same, mirrored, at 0.099 below 0.100.
    region(0, 100, 1);          // 0.000 - 0.100, deci-cent
    region(100, 900, 10);       // 0.100 - 0.900, cent
    region(900, kPriceOne, 1);  // 0.900 - 1.000, deci-cent
    return prices;
}

}  // namespace cpm::md
