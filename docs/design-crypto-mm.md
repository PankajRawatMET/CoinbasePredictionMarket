# CryptoMM: a two-sided quoting strategy

`strategies/CryptoMM/`. It quotes a bid and an ask a fixed number of ticks
either side of the mid, each for a fixed quantity, on one instrument.

It is the first component that uses both services — the book from
`market_data/`, the order path from `order_management/` — and neither of them
depends on it, which is what the `shared/` split was for.

## The decision is a pure function

`desired_quotes(book, position, params)` takes the book, the position and the
parameters and returns the two targets. No orders, no clock, no state, so every
rule can be tested by inspection:

| | |
|---|---|
| **The mid** | `(best YES bid + implied YES ask) / 2`, where the implied ask is `1.000 − best NO bid`. Both sides are needed: a one-sided book has no mid and nothing is quoted. |
| **The width** | `half_width_ticks` either side, in **ticks**, not in price — and the tick depends on the price (O5): 0.001 below 0.100 and above 0.900, 0.01 between, for the tapered grid the crypto series use. |
| **Rounding** | a bid rounds **down** to the grid and an ask rounds **up**, so rounding never moves a quote towards the other side. |
| **Never through the touch** | a target that would cross the resting book is stepped back a tick. A post-only reject would halt the run (OE-Q6), so the strategy does not even offer the venue the chance. |
| **A crossed book** | is not quotable: both sides are bids on this venue (Q9), so `best YES bid + best NO bid >= 1.000` is possible, and Stage 4 has already flagged it. |
| **Inventory** | `max_inventory` stops the side that would extend the position: long at the limit quotes only the ask, short only the bid. |

## The driver

`QuoteManager` puts the targets on the venue through four hooks — `can_submit`,
`send`, `cancel`, `next_cl_ord_id` — so it is driven by fakes in the tests and
by the real clients in `crypto_mm`.

- **A move is a cancel and a re-submit**, because the venue has no amend. The
  replacement waits for the cancel's report, since a `ClOrdID` must be unique
  among live orders.
- **One cancel at a time per side.** A second cancel while one is outstanding
  is answered `ORDER_ALREADY_PENDING_CANCEL`, which would halt the run.
- **Quotes are post-only slide**, not reject: a quote that would cross is
  adjusted rather than refused, and a refusal is a halt.
- **The quotes follow the mid.** Re-quoting is decided against where the quote
  actually **rests**, so when the mid moves, both sides move with it: a cancel
  and a re-submit at the new target, each time the target moves by at least
  `requote_threshold_ticks`.
- **With one exception: it does not retry a target the venue slid.** A
  post-only slide means the venue would not rest the order at that price, so
  offering it again produces the same slide — cancel, replace, slide, cancel,
  without end. That one price is skipped; every other target still re-quotes,
  so a moving mid is followed as normal. The live-lock only appeared when the
  binary was run against both simulators, whose books are independent.
- **A fill retires the quote**; a partial fill leaves the remainder, which is
  then the wrong size and gets replaced on the next pass.
- **Nothing is submitted while `can_submit()` is false**, which is how
  reconciliation and the halt of OE-Q6 reach the strategy.
- `withdraw()` cancels both sides: used when the instrument stops being Open —
  these are 15-minute windows (O2/O6) — and on shutdown.

## It says what it sees

A run that quotes nothing is the one outcome that cannot be diagnosed
afterwards, so the strategy reports what it would do on every change:

```
market 0.62 / 0.63, mid 0.625 -> quoting 0.6 / 0.65 [not sending]
not quoting: one-sided book
```

One line per *change*, not per pass, because the loop runs four times a
second. `last_reason()`, `passes()` and `passes_without_quotes()` are carried
into the exit summary, so a run that placed nothing explains itself: no book
ever arrived, or a reason it could not quote, or that `--allow-orders` was
never given.

## `crypto_mm`

Two sessions with their own credentials and their own endpoint guards, a
`MarketState` for the book, an `OrderState` and a `Reconciler` for the orders,
and a 250 ms loop that feeds the current book and position to the strategy.
Without `--allow-orders` it is a dry run: it computes and logs the quotes and
sends nothing — and it does not need an order entry session at all, so the
book alone is enough to see what it would quote.

```
source creds.env && source creds-oe.env && \
  ./build/strategies/CryptoMM/crypto_mm --symbol BTCD15M-... \
    --width-ticks 2 --qty 1 --max-inventory 5 --allow-orders
```

Against the simulators, note that `md_sim` and `oe_sim` keep **independent**
books, so a cross-service run shows slides that would not happen in production,
where the book and the orders are the same market. `oe_sim --instrument SYMBOL`
makes it serve the market data symbol.

## What it is not

No skew, no inventory-weighted mid, no spread widening with volatility, no
hedging between the YES and NO legs of an event, no sizing by queue position,
and no cross-instrument logic. It quotes x ticks wide with y on each side, which
is what it was asked to do.

## Resting to the bell, and why that is the safe default

`withdraw-before` is the number of seconds before the window ends at which the
quotes are pulled. **It defaults to 0: quote to the bell, and let the venue
cancel at the close.**

That is not laziness, and it is the safer of the two mechanically. An
instrument that closes takes its resting orders with it: the venue sends an
unsolicited `ExecType=CANCELED(4)` carrying `ExecRestatementReason(378)=2`,
"instrument lifecycle". Every layer here already handles that report - the
decoder maps it to `CancelReason::InstrumentLifecycle`, the reconciler halts
only on `REJECTED`, and `QuoteManager::on_report` drops the quote whether or
not it asked.

What is *not* safe is cancelling into the close. A cancel that reaches the
gateway after the lifecycle cancel has terminated the order is answered with
`OrderCancelReject` / `CxlRejReason=1 UNKNOWN_ORDER` ("an order that was
already canceled ... reports UNKNOWN_ORDER(1)"), and any cancel reject halts
the daemon (OE-Q6). So the rule is **cancel early or not at all, never late**,
and `IStrategy` has two verbs for it:

| | |
|---|---|
| `withdraw(why)` | cancel what is resting. The operator stopped it, the daemon halted, or `withdraw-before` fired while the market is still open |
| `abandon(why)` | drop the quotes and send nothing. The instrument closed and the venue has already cancelled them |

The host calls `abandon` on a lifecycle stop and `withdraw` on every other,
which is the difference between a roll that costs nothing and one that halts
the daemon four times an hour.

What setting `withdraw-before` above 0 buys is not safety but **expiry risk**:
the last seconds of a 15-minute binary are where the informed flow is, a
resting quote then is an option about to expire written for a tick of spread,
and a fill there cannot be managed - windows do not overlap, so the position
goes to settlement at 0 or 100. Leaving it at 0 accepts that deliberately.

The window end comes from the instrument's own name (`BTCD15M-BTC26OCT260630-0`
ends at 06:30 UTC, [MD] O2), parsed by `cpm::md::parse_ticker`. A name that
carries no end - a sports market, a simulator symbol - has nothing to count
back from, so the parameter is inert there rather than acting on a time it
invented.
