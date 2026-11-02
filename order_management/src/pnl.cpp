#include "cpm/oms/pnl.hpp"

namespace cpm::oms {
namespace {

// A quantity times a price, at the amount scale: qty is hundredths and price
// thousandths, so the product needs one more decade to reach six. The same
// conversion order_state.cpp does on a fill, and it holds its sign, so a
// short position marks negative.
Amount value_at(Qty quantity, Price price) { return quantity * price * 10; }

}  // namespace

Pnl pnl_of(const Position& position, std::optional<Price> mark) {
    Pnl out;

    // Settled: the payout is the whole of the rest, so nothing is open and no
    // mark is wanted. Everything paid in and taken out, against what came
    // back.
    if (position.settlement_payout) {
        out.realized = *position.settlement_payout - position.cash;
        out.unrealized = 0;
        out.total = out.realized;
        return out;
    }

    // Flat: everything bought was sold, so the cash is the answer and the
    // mark is irrelevant. This is the only closed case that needs no
    // division, and it is the common one at the end of a window.
    if (position.net == 0) {
        out.realized = -position.cash;
        out.unrealized = 0;
        out.total = out.realized;
        return out;
    }

    if (position.net > 0) {
        // Long: the sold quantity closed part of what was bought, at the
        // average price it was bought at. What that cost leaves behind is the
        // basis of the rest.
        const Amount closed_cost =
            position.bought > 0 ? position.sold * position.bought_value / position.bought : 0;
        const Amount open_basis = position.bought_value - closed_cost;
        out.realized = position.sold_value - closed_cost - position.fees;
        if (mark) out.unrealized = value_at(position.net, *mark) - open_basis;
    } else {
        // Short, which is the mirror: the bought quantity closed part of what
        // was sold, at the average price it was sold at.
        const Amount closed_proceeds =
            position.sold > 0 ? position.bought * position.sold_value / position.sold : 0;
        const Amount open_proceeds = position.sold_value - closed_proceeds;
        out.realized = closed_proceeds - position.bought_value - position.fees;
        // net is negative, so this subtracts what buying the rest back would
        // cost from what selling it brought in.
        if (mark) out.unrealized = open_proceeds + value_at(position.net, *mark);
    }
    if (out.unrealized) out.total = out.realized + *out.unrealized;
    return out;
}

}  // namespace cpm::oms
