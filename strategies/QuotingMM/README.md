# QuotingMM

QuotingMM prices dated 15-minute up/down contracts that use 60-second strike
and settlement averages. It uses the configured Binance spot symbol as a proxy
for the contract's benchmark. The pricing document validates the model on BTC.
Other mapped 15-minute series should be checked in replay before trading.

## Configuration

Set the history API base URL in the trader process environment. HTTP and HTTPS
are supported. Use the address reachable from the trader host.

```sh
export CPM_QUOTING_HISTORY_URL=http://history-server:8000
```

Enable the Binance reference feed with the trader's existing `--binance-on`
option and configure `url = wss://stream.binance.com:9443/ws` in
`binance.conf` or supply that URL with `--binance-url`. The reference feed is
shared by strategies in this daemon, so this setting selects spot for CryptoMM
too. Check the tick sizes in the symbol mappings against the spot feed.
Each history symbol must come from the same market as its live symbol.
The history API's response does not identify whether the recorder used spot
or futures.

## Startup and recovery

At startup and on each window roll, QuotingMM fetches `/v1/features` off the
order loop. The response supplies the pre-open 60-second reference average,
120 completed minute variances, fast variance, partial-minute candle, and the
last sampled midpoint. A restart during the final minute also reads elapsed
settlement samples from `/v1/midpoints`.

Live sampling picks the latest observed midpoint at or before each UTC
second boundary, subject to the API's maximum quote age. The live spot
book-ticker stream is timestamped on receipt by the daemon. Historical
sampling uses recorder event time. Both sides need synchronized clocks.
QuotingMM uses the live history buffered while the request runs and continues
strictly after the API's last timestamp.

Incomplete or invalid history, stale live data, and gaps prevent quotes.
QuotingMM withdraws resting orders on data loss and retries initialization
when data is available. The operator row reports why it is waiting.

## Pricing

Before the final minute the model applies the Student-t CDF with four degrees
of freedom to `(ln(S/K) + delta) / (c * sigma * sqrt(seconds_left - 40))`.
Inside the final minute it uses the running settlement average, projects
the remaining samples at the current spot midpoint, and omits the offset.

Volatility updates each second from the 20- and 120-minute Garman?Klass
variance estimates and the 120-second-half-life EWMA of one-second log
returns. The quote is recomputed as live inputs change.

A valid, external, one-tick prediction book with a midpoint between 0.10 and
0.90 can update calibration before the final minute. Updates take effect
after that second's quote. The volatility correction starts at 0.95 and uses
the rolling two-hour median after 200 eligible samples. The offset starts at
zero. Calibration is scoped to the series and reference symbol, survives
window rolls in memory, and starts fresh after a daemon restart. A valid empty
or one-sided book can still be used for quoting.

## Order controls

QuotingMM uses the existing post-only reconciler, cancel-before-replace,
inventory caps, sharp-move guard, and operator arming checks. The default
withdrawal cutoff is 25 seconds before expiry. Change `withdraw-before` if
you intend to quote deeper into the final averaging minute.

`vol-lookback-minutes`, `correction-factor`, and
`fair-value-move-threshold` are accepted for compatibility with saved
strategies. The model fixes the volatility lookbacks, calibrates the
correction automatically, and updates orders when target grid prices change.

Run the test suite on the supported Linux toolchain with `./build.sh tests`.
