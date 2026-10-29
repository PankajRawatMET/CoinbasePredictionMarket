#include "cpm/md/ticks.hpp"

#include <algorithm>

namespace cpm::md {

Price tick_at(Price price, TickStructure structure) {
    switch (structure) {
        case TickStructure::LinearCent: return 10;  // 0.01
        case TickStructure::DeciCent: return 1;     // 0.001
        case TickStructure::TaperedDeciCent:
            // 0.001 ticks below 0.100 and above 0.900, 0.01 between, with
            // 0.100 and 0.900 themselves on the coarse grid (O5).
            return (price < 100 || price > 900) ? 1 : 10;
        case TickStructure::TaperedCent:
            // A tenth of a percentage point inside the first and last point,
            // a whole one between, with 1% and 99% themselves on the coarse
            // grid - the same convention as the tapered structure above.
            return (price < 10 || price > 990) ? 1 : 10;
    }
    return 1;
}

bool is_quotable(Price price) { return price > 0 && price < kPriceOne; }

namespace {

// Each structure's grid, built once. The largest is 1001 entries, so this is
// a few kilobytes against a binary search on every quote.
const std::vector<Price>& cached_grid(TickStructure structure) {
    static const std::vector<Price> linear = price_grid(TickStructure::LinearCent);
    static const std::vector<Price> deci = price_grid(TickStructure::DeciCent);
    static const std::vector<Price> tapered = price_grid(TickStructure::TaperedDeciCent);
    static const std::vector<Price> tapered_cent = price_grid(TickStructure::TaperedCent);
    switch (structure) {
        case TickStructure::LinearCent: return linear;
        case TickStructure::DeciCent: return deci;
        case TickStructure::TaperedDeciCent: return tapered;
        case TickStructure::TaperedCent: return tapered_cent;
    }
    return tapered_cent;
}

}  // namespace

Price snap_down(Price price, TickStructure structure) {
    const auto& grid = cached_grid(structure);
    const auto above = std::upper_bound(grid.begin(), grid.end(), price);
    if (above == grid.begin()) return grid.front();
    return *(above - 1);
}

Price snap_up(Price price, TickStructure structure) {
    const auto& grid = cached_grid(structure);
    const auto at = std::lower_bound(grid.begin(), grid.end(), price);
    return at == grid.end() ? grid.back() : *at;
}

Price price_below(Price price, TickStructure structure) {
    return price <= 0 ? 0 : snap_down(price - 1, structure);
}

Price price_above(Price price, TickStructure structure) {
    return price >= kPriceOne ? kPriceOne : snap_up(price + 1, structure);
}

std::vector<Price> price_grid(TickStructure structure) {
    std::vector<Price> grid;
    const auto region = [&grid](Price from, Price to, Price step) {
        for (Price price = from; price <= to; price += step) {
            // The regions share their boundaries, so 0.100 and 0.900 are not
            // added twice.
            if (grid.empty() || grid.back() != price) grid.push_back(price);
        }
    };

    switch (structure) {
        case TickStructure::LinearCent: region(0, kPriceOne, 10); break;
        case TickStructure::DeciCent: region(0, kPriceOne, 1); break;
        case TickStructure::TaperedDeciCent:
            // By region, not by walking tick_at: the two answer different
            // questions and differ at exactly the boundaries. tick_at(0.900)
            // is the coarse tick, because that is the distance to the next
            // quotable price *upward from* 0.900 for a strategy stepping
            // through the grid - but 0.901 is still a price an order can rest
            // at, and a ladder that left it out would have nowhere to show
            // one. Same, mirrored, at 0.099 below 0.100.
            region(0, 100, 1);           // 0.000 - 0.100, deci-cent
            region(100, 900, 10);        // 0.100 - 0.900, cent
            region(900, kPriceOne, 1);   // 0.900 - 1.000, deci-cent
            break;
        case TickStructure::TaperedCent:
            region(0, 10, 1);            //   0 -   1 %, a tenth of a point
            region(10, 990, 10);         //   1 -  99 %, one point at a time
            region(990, kPriceOne, 1);   //  99 - 100 %, a tenth of a point
            break;
    }
    return grid;
}

const char* to_string(TickStructure structure) {
    switch (structure) {
        case TickStructure::LinearCent: return "cent";
        case TickStructure::DeciCent: return "deci";
        case TickStructure::TaperedDeciCent: return "tapered";
        case TickStructure::TaperedCent: return "tapered-cent";
    }
    return "tapered-cent";
}

std::optional<TickStructure> tick_structure_from(const std::string& name) {
    if (name == "tapered") return TickStructure::TaperedDeciCent;
    if (name == "cent") return TickStructure::LinearCent;
    if (name == "deci") return TickStructure::DeciCent;
    if (name == "tapered-cent") return TickStructure::TaperedCent;
    // What this structure was called for the few hours before the bottom of
    // the grid was described too. Accepted so a saved command line still runs.
    if (name == "cent-top") return TickStructure::TaperedCent;
    return std::nullopt;
}

}  // namespace cpm::md
