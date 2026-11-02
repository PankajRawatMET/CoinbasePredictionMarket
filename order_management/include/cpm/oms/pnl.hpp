#pragma once

#include "cpm/oms/order_state.hpp"

#include <optional>

// The P&L of one position (GUI-Q15). A pure function of the position and a
// mark, with no book and no clock in it, so the whole of it can be tested by
// inspection - the same division the quoting decision keeps.
namespace cpm::oms {

struct Pnl {
    // What the closed part of the position made, fees included. Always known:
    // it needs no mark.
    Amount realized = 0;
    // What the open part is worth at the mark, and the two added together.
    //
    // Nullopt rather than zero when a position is open and there is no mark.
    // A zero would show a flat P&L where the truth is "not known", and the
    // only honest mark here is the mid, which is absent exactly when the book
    // is one-sided or invalid.
    std::optional<Amount> unrealized;
    std::optional<Amount> total;
};

// `mark` is a YES price - the mid, in practice. Pass nullopt when there is
// none.
//
// The average cost method, on the two values the position keeps: the closed
// quantity is valued at the average of the other side, and whatever is left
// carries the remainder of its own side's value as its basis. Realized and
// unrealized therefore always add up to `net x mark - cash` exactly, with no
// rounding dust between them.
//
// A settled position (Stage 6) is a special case: the payout closed it, so
// all of it is realized and none of it needs a mark.
Pnl pnl_of(const Position& position, std::optional<Price> mark);

}  // namespace cpm::oms
