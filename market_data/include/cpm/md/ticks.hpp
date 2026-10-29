#pragma once

#include "cpm/common/fixed.hpp"

#include <optional>
#include <string>
#include <vector>

// The price grid an instrument trades on: which prices exist at all.
//
// This is a property of the instrument, not of any strategy - it decides what
// a quote may be priced at, and equally what rungs a ladder has. It lives here
// with the rest of the instrument's state rather than in a strategy, which is
// where it was first written (CryptoMM, design-crypto-mm.md) and where only
// one caller could see it.
//
// The values come from the REST PriceLevelStructure (docs/design.md O5). The
// client cannot read an instrument's structure over FIX, so it is configured
// rather than discovered.
namespace cpm::md {

enum class TickStructure {
    LinearCent,       // 0.01 everywhere
    DeciCent,         // 0.001 everywhere
    TaperedDeciCent,  // 0.001 below 0.100 and above 0.900, 0.01 between
    // Read as a percentage chance: 1 point everywhere, a tenth of a point
    // inside the first and last point - 0, 0.1, 0.2 ... 0.9, 1, 2 ... 98, 99,
    // 99.1 ... 100. Built to a description of the venue that REST later
    // contradicted: BTCD15M and ETHD15M both report TAPERED_DECI_CENT, so
    // that is the default and this is kept because every price on it is also
    // on that one (GUI-Q23). The same shape as TaperedDeciCent, but tapering a
    // tenth as far in from each end, which is why it is not that structure
    // and not any of the three the spec's PriceLevelStructure table lists
    // (docs/open-questions.txt O5) - see docs/open-questions-gui.txt GUI-Q23.
    TaperedCent,
};

// The tick size at a price, in thousandths: how far the next quotable price
// is from this one, which is what a strategy stepping through the grid needs.
Price tick_at(Price price, TickStructure structure);

// Every price the grid allows, from 0 to 1.000 inclusive, ascending. This is
// the ladder's axis: 281 rungs for the tapered grid, 101 for cents, 1001 for
// deci-cents. The ends are included because they are prices - whether anything
// ever rests there is a different question, and one the book answers.
//
// This is membership, not stepping, and the two differ at the boundaries: a
// strategy stepping up from 0.900 lands on 0.910 (tick_at says so), but 0.901
// is still a price an order can rest at, so the ladder has a rung for it.
//
// The ends are on the grid but are not order prices: "order prices exclude 0
// and 1; settlement values may equal either" (O5). is_quotable() is that rule.
std::vector<Price> price_grid(TickStructure structure);

// The "tapered" / "cent" / "deci" spelling the schema, the CLI and the GUI
// all share.
// False for 0 and 1.000, which are settlement values rather than prices an
// order may be placed at (O5).
bool is_quotable(Price price);

// The grid price at or below `price`, and at or above it. A price that is
// already on the grid is returned unchanged.
Price snap_down(Price price, TickStructure structure);
Price snap_up(Price price, TickStructure structure);

// The next grid price strictly below `price`, and strictly above it.
//
// Not `price - tick_at(price)`: that lands off the grid whenever `price`
// itself is off it, or when the step crosses a boundary where the tick size
// changes. One below 99% on the venue's grid is 98%, not 98.9% - and an order
// at 98.9% would be rejected, which halts the daemon (OE-Q6).
Price price_below(Price price, TickStructure structure);
Price price_above(Price price, TickStructure structure);

const char* to_string(TickStructure structure);
std::optional<TickStructure> tick_structure_from(const std::string& name);

}  // namespace cpm::md
