# QuotingMM

QuotingMM is a model-based market maker for dated 15-minute prediction
instruments. It runs beside CryptoMM through the same strategy registry,
host, arming controls, order path, parameter form and operator page.

## Pricing

The model follows `Pred Quoting Summary.pdf`:

```text
z        = log(underlying / opening) / (correction * sigma * sqrt(seconds_left))
fair YES = StudentT4CDF(z)
bid/ask  = the grid pair `width-ticks` apart straddling fair YES
```

`sigma` is the sample standard deviation of completed one-minute log
returns over the 20 minutes ending at the window's open, in return per
square root second rather than annualized. A gap in those minutes is
refused rather than stepped over, and zero variance is not accepted. See
"Where sigma and the opening come from" below.

`StudentT4CDF` is the ordinary Student t CDF with four degrees of freedom,
applied to z exactly as the PDF writes it. There is no alternative scaling:
a knob whose only effect is to multiply z by a constant would silently
invalidate a correction factor fitted without it.

The correction factor defaults to **1**, which prices straight off the
measured sigma. That is a starting point, not a calibration: the PDF asks
for a factor fitted over a one-to-two-week lookback and held constant until
the next one. This version does not infer it, recalibrate it automatically,
or reproduce the PDF's historical backtest. Set it explicitly once you have
fitted it; normal disarm/edit rules apply.

## Empty and quiet books

A valid empty snapshot or one-sided book is sufficient. Fair value does not
depend on the prediction-market midpoint or other participants. Any visible
opposing order is used only to avoid crossing. Invalid snapshots and crossed
books prevent quoting. There must still be a healthy prediction-market
session and subscription: a missing snapshot is not an empty market.

The host reports whether a strategy requires continuing book updates.
QuotingMM tolerates a silent, unchanged book; CryptoMM retains its existing
book-silence timeout. Disconnects, sequence gaps and subscription rejects
still halt both strategies through the existing monitor.

`width-ticks` is a **hard cap**, not a minimum: the market shown is never
wider than the one configured. Fair value is almost never on a grid point,
so the pair of grid prices exactly `width-ticks` steps apart that straddles
fair most evenly is used - at most half a step of skew either way, and
neither leg is ever rounded away from the other to reach the grid. Width is
counted in grid *steps*, not in a tick size multiplied out, so it stays
right where the venue's taper changes the step (0.100 and 0.900).

A leg may not rest at or inside the opposing touch - that is taking, not
making. Because the quote may not be widened past the cap, and sliding the
pair clear would post prices the model does not believe, the strategy
**stands aside** while the book leaves no room, and the row says which side
conflicted. It resumes as soon as the touch moves.

Orders share CryptoMM's extracted QuoteReconciler: GTC limit, post-only
slide, unique IDs, cancel-before-replace, actual resting-price tracking and
execution reports.

## Reference history and opening price

The existing mapping in `config/binance.conf` selects the underlying feed.
The shipped configuration uses Binance futures. Confirm that this source
and the chosen opening price match your intended model and the contract's
benchmark; a Binance observation is not an official Coinbase settlement
strike.

## Where sigma and the opening come from

Both are read **once**, from Binance's own kline history, when the instance
is built - at create, and again at every window roll. Neither is recomputed
while the window runs.

For a window that opened at 06:00, one public GET of
`/fapi/v1/klines?symbol=BTCUSDT&interval=1m&limit=22&endTime=<06:00>` returns
the 21 closes before the window plus the window's own bar. Sigma is the
sample standard deviation of those 20 one-minute log returns, divided by
`sqrt(60)` to put it per `sqrt(second)`; the opening is the window bar's
`open`, which is the first price the venue traded at or after 06:00.

That means an instance created at 06:12 prices exactly as one created at
06:00:02 would: the bars behind a window are closed and never change again.
It also means there is no warmup state at all - a strategy either has its
inputs when it is built, or it is not built.

The call is refused rather than guessed around. If the venue is unreachable,
if any of the 21 bars is missing, or if the window's own bar has not printed
yet, `create` fails with the reason and a roll keeps the old instance while
the daemon's next tick tries again. Results are cached per window and a
failure is not retried more than once every two seconds, so a roll retried
on every tick cannot put a blocking HTTPS call in the daemon's loop.

`rest_url` in `binance.conf` says where REST is (default
`https://fapi.binance.com`). No credential is involved: klines are public
market data. This reverses B-Q17's "no REST call of any kind"; see
`docs/open-questions-binance.txt`.

There is no `opening-price` parameter any more. The strike comes from the
venue the model prices against, like the spot and the volatility do, so
there is nothing left for a typed-in opening to correct. If the contract
ever has to be struck against something other than Binance's window-open
print - the catalog's own `metadata.market.price_strike`, say - that is a
change to where the strike is read from, not a box on a form.

A sigma taken from 20 one-minute returns is a coarser estimate than one
taken from 1200 one-second returns, and a different one: minute closes do
not see intra-minute reversal. `binance_klines --symbol BTCUSDT --minutes 20`
prints the bars and the same sigma figure, for comparing the two.

The live one-second history is no longer read for pricing. It is still what
the PricingMidMoveThreshold guard reads, and the daemon still keeps and
caches it.

## Refresh, withdrawal and inventory

Quote targets change when fair value moves by `fair-value-move-threshold`
prediction ticks from the last target. There is no five-second quoting timer.
The model is evaluated on host passes, including reference-triggered wakes;
time remaining can also move fair value. Fills replenish through the shared
reconciler.

There is no `ticks` parameter: every instrument this build quotes trades on
the venue's `TAPERED_DECI_CENT` grid (`cpm/md/ticks.hpp`), and the daemon
says so loudly if the catalog ever reports an instrument on another one.

`PricingMidMoveThreshold` counts **reference** ticks, not prediction ticks:
how far the reference venue's mid may move in a *single update* before the
quotes standing behind it are stale. Two tests share the one threshold -
this update against the last, and the high-low range of any one second just
passed. The second test is not redundant: the feed hands the strategy only
the latest touch at each wake, so a spike that reverts before the next wake
is invisible to the first. For the shipped BTC mapping (0.1), 2,500 ticks
means a **$250** move - wide enough that it has never fired, so pick a
number that suits the market you are quoting.

It deliberately does not measure how far the mid has drifted from where a
resting order was priced. That asks whether a quote has aged, not whether
the market just jumped, and it is what CryptoMM's parameter of the same name
measures. The two strategies answer different questions under one name.

A breach cancels both quotes. Re-entry requires fresh inputs,
`PricingMidBreachResumeSec` seconds of quiet, and confirmed completion of
the old orders.
It does not wait for other prediction-market participants to move their
quotes. Reference loss also withdraws. The existing wake pipe is reused;
the PDF's 60 ms objective is not an end-to-end latency guarantee.

The default cutoff withdraws 25 seconds before expiry. At expiry the venue
owns lifecycle cancellation; local orders are abandoned to avoid racing it.
No late-window skew mode is included.

Each side's quantity is capped by remaining net inventory capacity assuming
that side fills without the opposite side filling. The reconciler has at
most one outstanding order per side. Reaching the inventory limit requests
a host stop and cancels both sides; it does not flatten the position.
Asynchronous fills and cancels remain subject to the existing OMS rules.

## Parameters

| Name | Default | Unit / purpose |
|---|---|---|
| `symbol` | required | Dated 15-minute instrument, or series through the existing control API |
| `correction-factor` | 1 | Positive multiplier of sigma; 1 is uncalibrated |
| `width-ticks` | 4 | Whole spread in grid steps; a hard cap, never exceeded |
| `qty` | 1 | Contracts per side |
| `max-inventory` | 5 | Contracts, absolute net; 0 disables |
| `vol-lookback-minutes` | 20 | Minutes of 1m klines before the window's open; 2–1440 |
| `fair-value-move-threshold` | 1 | Prediction ticks from the last target |
| `pricing-mid-move-threshold` | 2500 | `PricingMidMoveThreshold` — reference ticks in one update; 0 disables |
| `pricing-mid-breach-resume-sec` | 1 | `PricingMidBreachResumeSec` — seconds to stand aside after a breach |
| `withdraw-before` | 25 | Seconds before expiry; 0 rests until close |

Select QuotingMM in the existing strategy form; the correction factor
defaults to 1 and can be edited before arming. The expandable Pricing model section shows fair
YES probability, sigma and where it came from, the underlying, remaining
time and readiness/hold state. The opening price is **not** on the row: it
comes from Binance like every other input, and is written on every
`strategy.log` line instead.

## What a run leaves behind

Each time the quoting changes, `strategy.log` gets the model's whole
working, so a finished run can be re-priced from its own log:

```
fair 0.528114, z 0.075058, ln(S/K) 0.00018158, spot 64440.3,
  open 64428.6, left 834.0s, sigma 0.00008377 -> 0.51 / 0.55
```

These lines do **not** go to the console. A strategy quoting puts out some
thirty lines a second, which buries the daemon's own events, so the terminal
keeps the narrative and `strategy.log` keeps the commentary; the page's log
panel still shows both. `--console-strategy` puts them back on stdout. A
Warn or an Error from a strategy is never silenced.

`open` is the strike this window is settled against, `spot` the underlying
at that instant, `left` the seconds remaining, and `sigma` the figure frozen
at the window's open. `ln(S/K)` is the distance the model is defined on and
`z` is that divided by `correction * sigma * sqrt(left)` - so `fair` is
reproducible from the line, and a correction factor can be fitted against
it afterwards. When spot equals the strike the line reads `z 0.000000` and
`fair 0.500000`, whatever sigma says. Feed, Start, Arm, Stop and parameter edits use the same
controls as CryptoMM. Test disarmed and in the simulators/replay environment
before arming on a live venue.

Build and run the tests on the supported Linux toolchain:

```sh
./build.sh tests
```
