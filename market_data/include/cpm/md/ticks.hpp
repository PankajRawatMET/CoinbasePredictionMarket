#pragma once

#include "cpm/common/fixed.hpp"

#include <vector>

// The price grid: which prices exist at all.
//
// There is one, and it is the venue's: TAPERED_DECI_CENT, from the REST
// spec's PriceLevelStructure table (section 6.4, docs/open-questions.txt O5).
//
//     0.000 - 0.100   tick 0.001     tenths of a point
//     0.100 - 0.900   tick 0.01      whole points
//     0.900 - 1.000   tick 0.001     tenths of a point
//
// 0.100 and 0.900 are valid boundary prices and belong to both regions.
//
// The three structures the spec lists and the fourth this project once had
// are gone. BTCD15M and ETHD15M both report TAPERED_DECI_CENT in the
// instrument catalog, the live book prices on it, and a configurable grid
// was a way to be wrong about a fact the venue states (GUI-Q23).
//
// The catalog is still read: an instrument the venue says trades on some
// other structure is reported, loudly, because this build cannot price it.
namespace cpm::md {

// The venue's own spelling, for comparing against what the catalog reports.
inline constexpr const char* kPriceLevelStructure = "TAPERED_DECI_CENT";

// The tick size at a price, in thousandths: how far the next quotable price
// is from this one, which is what a strategy stepping through the grid needs.
Price tick_at(Price price);

// Every price the grid allows, from 0 to 1.000 inclusive, ascending. This is
// the ladder's axis: 281 rungs. The ends are included because they are
// prices - whether anything ever rests there is a different question, and one
// the book answers.
//
// This is membership, not stepping, and the two differ at the boundaries: a
// strategy stepping up from 0.900 lands on 0.910 (tick_at says so), but 0.901
// is still a price an order can rest at, so the ladder has a rung for it.
//
// The ends are on the grid but are not order prices: "order prices exclude 0
// and 1; settlement values may equal either" (O5). is_quotable() is that rule.
std::vector<Price> price_grid();

// False for 0 and 1.000, which are settlement values rather than prices an
// order may be placed at (O5).
bool is_quotable(Price price);

// The grid price at or below `price`, and at or above it. A price that is
// already on the grid is returned unchanged.
Price snap_down(Price price);
Price snap_up(Price price);

// The next grid price strictly below `price`, and strictly above it.
//
// Not `price - tick_at(price)`: that lands off the grid whenever `price`
// itself is off it, or when the step crosses a boundary where the tick size
// changes. One below 99% on the venue's grid is 98%, not 98.9% - and an order
// at 98.9% would be rejected, which halts the daemon (OE-Q6).
Price price_below(Price price);
Price price_above(Price price);

}  // namespace cpm::md
