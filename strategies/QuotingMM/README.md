# QuotingMM

QuotingMM is a model-based market maker for dated 15-minute prediction
instruments. It runs beside CryptoMM through the same strategy registry,
host, arming controls, order path, parameter form and operator page.

## Pricing

The model follows `Pred Quoting Summary.pdf`:

```text
z        = log(underlying / opening) / (correction * sigma * sqrt(seconds_left))
fair YES = StudentT4CDF(z)
bid/ask  = fair YES +/- half the configured spread
```

`sigma` is the sample standard deviation of completed one-second log
returns over the last 1,200 seconds by default. It is in return per square
root second, not annualized. Missing seconds are not forward-filled and
zero variance is not accepted. A complete 1,200-return estimate requires
1,201 consecutive observed second buckets. The current partial bucket is
excluded.

The `raw` t scale implements the PDF formula directly. `unit-variance`
multiplies z by sqrt(2), accounting for the ordinary t4 distribution's
variance of two. Choose the convention used in your calibration.

The correction factor must be supplied. This version does not infer it,
recalibrate it automatically, or reproduce the PDF's historical backtest.
Update it explicitly using your calibrated value; normal disarm/edit rules
apply.

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

Prices are snapped outwards to the configured instrument grid. The spread
is a minimum before price-bound and touch adjustments. Orders share
CryptoMM's extracted QuoteReconciler: GTC limit, post-only slide, unique IDs,
cancel-before-replace, actual resting-price tracking and execution reports.

## Reference history and opening price

The existing mapping in `config/binance.conf` selects the underlying feed.
The shipped configuration uses Binance futures. Confirm that this source
and the chosen opening price match your intended model and the contract's
benchmark; a Binance observation is not an official Coinbase settlement
strike.

`opening-price = 0` uses the first observed mapped reference price within
the window's first UTC second. A strategy started mid-window can recover
that observation from shared history, but never substitutes its startup
price for an unknown opening. Supply the known official opening using
`opening-price` when needed. That override is reset to zero on a series roll
so it cannot silently carry an old strike into a new window.

Reference history is bounded to one hour and shared across strategies and
window rolls. It is built from the live stream; there is no new HTTP data
dependency. Pin the mapped stream in `binance.conf` to warm history before
starting a strategy. A cold start waits for the required contiguous history.

Warmup is shared across window rolls: a new strategy instance uses the same
underlying history and does not start another 20-minute collection period.
The Pricing model section shows `Volatility returns` and the minimum time
still needed. Gaps in the reference observations can extend that minimum.

For a shorter simulator experiment, set `vol-lookback-seconds` to `60` in
the strategy form before starting. That uses one minute of returns and
changes the volatility estimate; it is not a fast way to obtain the same
20-minute model. Keep the default `1200` to preserve the intended lookback,
and run the reference feed ahead of the trading window. This version does
not automatically download historical observations at startup.

The daemon saves a source-tagged history cache on clean shutdown beside
its bookmark (`<bookmark>.reference-history`). `history_file` in the Binance
configuration overrides that location. Cache loading never makes the live
feed fresh. Restart gaps still require fresh contiguous returns, and a crash
may lose history since the last clean save. The cache is not a historical
data downloader or a substitute for observing an opening.

## Refresh, withdrawal and inventory

Quote targets change when fair value moves by `fair-value-move-threshold`
prediction ticks from the last target. There is no five-second quoting timer.
The model is evaluated on host passes, including reference-triggered wakes;
time remaining can also move fair value. Fills replenish through the shared
reconciler.

The sharp-move guard counts **reference** ticks, not prediction ticks. It
checks movement from each live order's pricing reference, successive
reference observations, and observed second-bucket high/low ranges since the
previous pass. The ranges preserve a jump followed by a reversal between
passes. For the shipped BTC mapping (0.1), 2,500 ticks means a $250 move.

A breach cancels both quotes. Re-entry requires fresh inputs, a quiet period
after the last detected breach, and confirmed completion of the old orders.
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
| `correction-factor` | required | Positive calibrated multiplier |
| `width-ticks` | 4 | Whole spread in prediction ticks |
| `qty` | 1 | Contracts per side |
| `max-inventory` | 5 | Contracts, absolute net; 0 disables |
| `vol-lookback-seconds` | 1200 | Completed one-second returns; 2–3600 |
| `t-scale` | raw | `raw` or `unit-variance` |
| `fair-value-move-threshold` | 1 | Prediction ticks from the last target |
| `sharp-move-threshold` | 2500 | Reference ticks; 0 disables |
| `resume-quiet-ms` | 1000 | Quiet period after the last detected breach |
| `withdraw-before` | 25 | Seconds before expiry; 0 rests until close |
| `opening-price` | 0 | Underlying price override for this window only |
| `ticks` | tapered | Existing prediction-market tick grid |

Select QuotingMM in the existing strategy form and enter the correction
factor before creating it. The expandable Pricing model section shows fair
YES probability, sigma, underlying and opening prices, remaining time and
readiness/hold state. Feed, Start, Arm, Stop and parameter edits use the same
controls as CryptoMM. Test disarmed and in the simulators/replay environment
before arming on a live venue.

Build and run the tests on the supported Linux toolchain:

```sh
./build.sh tests
```
