# The Binance reference feed, and the pricing speed bump

Stage B0. Written 2026-10-29 from: "add binance feed ... If the change in Mid
on binance exceeds a certain number of ticks in the algo, we cancel orders on
the prediction market algo and reprice when our market moves. This should
enable the speed bump to protect us", and the operator's amendment: "binance
mid check will be done on each update from binance, it should not be
dependent on quoting market update".

Open questions and the decisions behind this: `docs/open-questions-binance.txt`.

## 1. The problem this solves

`BTCD15M` is a fifteen-minute binary on BTC. Its fair value is a function of
one number - where BTC is now, against the strike, with minutes left - and
that number is discovered on Binance, not on the prediction market. CPM's
book follows; it does not lead.

So a market maker resting two-sided quotes on CPM is short an option it never
priced. BTC moves on Binance; for as long as CPM's book has not caught up,
our bid is too high or our ask too low by the whole of that move, and the
first person to notice takes it. We find out when the fill arrives.

Nothing in the strategy today sees this. `desired_quotes()` prices off the
CPM book alone, and the CPM book is exactly the thing that is late.

**The guard is not a pricing model.** It does not try to say what the binary
is worth given BTC - that would be a different strategy. It says only: *the
world has moved more than X since we priced, so our price is no longer ours.
Leave, and do not come back until this market has repriced.* Leaving is
always right when the quote is stale, whichever way it moved.

## 2. What Binance actually does

Confirmed 2026-10-29 against `developers.binance.com`, not from memory,
because B1 and B2 are written to these rules.

**The stream.** `<symbol>@bookTicker` pushes the best bid and ask on every
change to either:

```json
{"u":400900217,"s":"BNBUSDT","b":"25.35190000","B":"31.21000000",
 "a":"25.36520000","A":"40.66000000"}
```

`u` update id, `s` symbol, `b`/`B` best bid and its quantity, `a`/`A` best
ask and its quantity. Prices and sizes are decimal **strings**, which is what
lets this stay in scaled integers end to end - they are never parsed to a
double (the market data rule, [MD] design §4, applies here too).

Endpoints: `wss://stream.binance.com:9443` and `:443` for production;
`wss://demo-stream.binance.com` and `wss://stream.testnet.binance.vision` for
the demo and test environments, whose books are not the real market (B-Q6).

**The connection.**
- The server sends a **ping frame every 20 seconds**. A client that does not
  pong within **one minute** is disconnected. The pong must copy the ping's
  payload. Unsolicited pongs are allowed - empty payload - but do not stop
  the disconnect, so they are not a substitute for answering.
- **A connection lasts 24 hours.** Expect to be disconnected and reconnect.
  A `serverShutdown` event may arrive first.
- 300 connections per 5 minutes per IP. One connection, reconnecting on a
  backoff, is nowhere near it.
- Public market data: **no API key, nothing signed, nothing sent but the
  handshake.** This feed cannot trade and holds no credential.

**The tick size.** Not asked for: **this project makes no REST call to
Binance at all** (B-Q17). The tick size is part of the symbol mapping in
`config/binance.conf`, because it is a fact about the venue that changes
about never and a lookup for it was one more thing to go wrong - and did
(B-Q16).

**The venue is futures.** `wss://fstream.binance.com/ws`, where BTCUSDT
quotes to two decimals, so a tick is **0.1** and not spot's 0.01. A
`PricingMidMoveThreshold` of 2500 is therefore a **$250** move here.

## 3. The shape

```
 Binance                        cpm_trader (one thread, as today)
 ───────                        ─────────────────────────────────
 wss bookTicker                 ┌──────────────────────────────┐
      │                         │ poll({stdin, waker}, 250ms)  │
      ▼                         │            │                 │
 ┌──────────┐  latest mid       │            ▼                 │
 │ WsClient │ ───────────────►  │      trader.tick()           │
 │  thread  │  (mutex)          │            │                 │
 └────┬─────┘                   │            ▼                 │
      │ one byte                │   host.tick() ──► on_reference(ref)   ◄── the guard
      └────────────────────────►│                └► drive() ──► on_market(book)
         (self-pipe waker)      └──────────────────────────────┘
```

Three pieces, each testable on its own:

**`cpm::ws::Client`** (B1) - a WebSocket client over a `TlsStream` extracted
from `https_get`. RFC 6455: the handshake with `Sec-WebSocket-Key` and the
`Accept` check, client-masked frames out, text frames in, ping answered with
the payload copied, close handled, reconnect with backoff. It knows nothing
about Binance.

**`cpm::binance::ReferenceFeed`** (B2) - owns the client on its own thread,
parses `bookTicker` into a scaled mid per symbol, keeps the latest behind a
mutex with the time it arrived, and signals the waker. It knows nothing about
strategies.

**One connection, streams on and off it** (B-Q14). The connection is opened
to `/stream` with nothing in the URL and lives as long as the daemon;
`want(symbols)` says which streams should be up and the reader thread sends
the difference as `SUBSCRIBE` and `UNSUBSCRIBE`. The trader recomputes that
set every pass from the strategies the host reports as **Running**, so
starting a strategy on `BTCD15M` subscribes `btcusdt@bookTicker` and stopping
it drops the stream. Declared rather than counted: nothing can leak a
subscription, and a reconnect is "want everything again".

The venue allows **five incoming messages a second** on a connection and
counts our pongs among them, and an IP repeatedly disconnected for exceeding
it may be banned - so control messages are capped at two a second and N
symbols cost one message, not N.

**The guard** (B3) - in `QuoteManager`, where the live legs already are. It
is the only piece that knows what `PricingMidMoveThreshold` means.

`market_data/` does not depend on any of this, and none of it depends on
`order_management/`. The trader wires them, as it wires everything else.

## 4. The guard, step by step

State, per strategy, beside the live legs:

| | |
|---|---|
| `reference` | the Binance mid when the current quotes were placed |
| `held` | true while sitting out after a breach |
| `mid_at_withdraw` | the CPM mid when we pulled them |

**On every Binance update** (`on_reference`, which the host calls whether or
not there is a book):

1. If `PricingMidMoveThreshold` is 0, do nothing. The guard is off and the
   strategy is exactly what it was.
2. If nothing is live and we are not quoting, record the mid and return.
3. If `|mid - reference| >= threshold x tickSize`: **cancel both legs now**,
   set `held`, record `mid_at_withdraw` from the last CPM book we saw, and
   log it once with the size of the move.

**On every pass with a book** (`on_market`, unchanged otherwise):

4. If the reference is stale - nothing from Binance for `--binance-stale-after`
   (default 5s) - withdraw and quote nothing. No reference means no
   protection, and quoting unprotected is the thing this exists to stop.
5. If `held`: quote nothing until the CPM mid differs from `mid_at_withdraw`
   by at least one tick. Then clear `held` and quote again.
6. Whenever a quote is placed or replaced, `reference` becomes the current
   Binance mid. So the measurement is always "since we priced", never a drift
   accumulated across re-quotes.

Step 6 is why a normal re-quote resets the guard: the re-quote happened
because our market moved, which is the same event that releases a hold. The
two agree by construction.

**Inside `withdraw-before`**, the guard abandons instead of cancelling. A
cancel of ours racing the venue's own close-cancel is an
`OrderCancelReject`, and that halts the daemon (R-Q1). The lifecycle path
already abandons for this reason; the guard follows it.

**Disarmed**, the guard runs and logs exactly as it would armed, and places
and cancels nothing - like the rest of a dry run.

## 5. Why the loop has to change

The strategy runs on a 250 ms cadence: `cpm_trader`'s loop polls stdin for
250 ms, sleeps 250 ms, calls `trader.tick()`. And `StrategyHost::drive()`
skips a strategy whose book is missing - `continue`, with no call into the
strategy at all.

Both are wrong for this. A guard on that cadence reacts up to a quarter
second late, which is roughly the window it exists to close; and a guard
inside `on_market` would not fire when our book has gone quiet or closed
while our orders still rest, which is precisely when we are most exposed.

So:

- **The feed wakes the loop.** The poll already waits on stdin; it gains a
  self-pipe the feed thread writes one byte to on each update. A Binance tick
  returns from `poll` at once. The 250 ms stays as a floor, so everything
  that depended on a regular tick still gets one.
- **The guard gets its own entry point.** `IStrategy::on_reference(const
  ReferenceView&, now)`, virtual with an empty default so no existing
  strategy or test double changes. The host calls it for every **Running**
  strategy on every pass, before `drive()`, regardless of book state.
  Cancelling needs no book: `QuoteManager` has the legs and the cancel hook.

Breach to cancel becomes a frame parse plus a wake, instead of up to 250 ms.

**Updates coalesce.** The waker is a flag, not a queue, and the feed keeps
only the latest mid. A burst during one pass collapses into one pass at the
newest price, so the pass rate is bounded by how fast the loop runs rather
than by how fast Binance talks.

## 6. Threading

The Binance thread stores a number and writes a byte. It never touches
strategy state, never sends an order, never takes the host's lock.

This is deliberate and it is the one rule not to break here. `host.hpp`
records why: `crypto_mm` used to call `on_report` from the session thread
while its own loop called `on_market`, with nothing between them, and the
host exists partly to end that race. A guard that cancelled from the feed
thread would put it straight back - two threads mutating the same legs.

So every strategy call still happens on the thread that calls `tick()`,
including `on_reference`.

## 7. What the operator sees

- A new column, **`PricingMidMoveThreshold`**, in the Strategies grid. It
  appears by itself: the header is built from the union of the registered
  kinds' parameters (GUI-Q29), so adding it to CryptoMM's schema is the whole
  change. Default **0, which is off** - a strategy that does not set it
  behaves exactly as it does today.
- The row's line says why it is not quoting: *binance moved 2,500 ticks,
  waiting for our market*, or *binance feed stale*. That is `status.reason`,
  which the row already shows.
- The log panel gets the breach, the release and the staleness, once each -
  they are `Daemon` events (GUI-Q28). The feed's own per-update traffic is
  `Wire` and stays in the file.
- A header pill for the reference feed, beside `md` and `oe`.

## 7a. Where the URLs live

`config/binance.conf`, not the source (B-Q15):

```
url = wss://fstream.binance.com/ws

map.BTCD15M = { BTCUSDT, tickSize: 0.1 }
map.ETHD15M = { ETHUSDT, tickSize: 0.01 }

stale_after_ms = 5000
```

That is the whole of it. The URL is taken apart into scheme, host, port and
path; a mapping key is matched against a ticker's series and its asset; the
tick size is required, because without a REST lookup a mapping that lacked
one would silently disable the guard.

Read from `<dict-dir>/binance.conf`, or from `--binance-config PATH`, with
`--binance-url` overriding the one line for a single run. An unknown key
stops the daemon rather than being ignored. The file holds no credential and
the feed has none: it signs nothing and asks the venue for nothing over
HTTP.

## 8. Stages

| | |
|---|---|
| **B0** | This document and the open questions. No code. |
| **B1** | `TlsStream` out of `https_get`, then `cpm::ws::Client` on it. Tested against a loopback server, including a server that pings, one that fragments a frame across reads, one that closes mid-message and one that never pongs. |
| **B2** | `cpm::binance::ReferenceFeed`: bookTicker to a scaled mid, staleness clock, reconnect, the waker. `binance_sim`, a real local WebSocket server with `--stall-after`, `--drop-after`, `--jump N`, so every test and every sandbox run works offline. `tickSize` from `exchangeInfo` at startup. |
| **B3** | `IStrategy::on_reference`, the host calling it unconditionally for Running strategies, the waker in the main loop, `PricingMidMoveThreshold` in CryptoMM's schema, the guard in `QuoteManager`. Tests driven by `binance_sim`. |
| **B4** | Done 2026-10-29. See below. |

### What B4 measured

**Breach to cancel: 1.0 ms.** `binance_sim --jump 2500@80` moves the
reference $25 while md_sim's book carries on as it was; the guard notices
and the cancel is on the wire in the same millisecond. Before the waker this
would have been anywhere up to 250 ms, which is the whole reason the loop
changed (B-Q5).

**The stale path**, with `--stall-after`: the feed stays connected and goes
silent, the pill turns from `up` to `stale`, both legs come off, and the row
says *the reference feed is stale (11724ms)*. The daemon stays up throughout.

**Production Binance, read-only**, 20 seconds: 1405 updates over TLS on the
combined stream, BTCUSDT and ETHUSDT both usable, `tickSize` 0.01 for each
from `exchangeInfo`. No key, nothing signed.

**The column arrives by itself**, as designed: the header read
`Buttons | Kind | Symbol | Spread | Qty | Max Inventory | Requote Threshold |
Withdraw Before | PricingMidMoveThreshold | Ticks` with no change to the
page, because the columns come from the schema (GUI-Q29).

Not measured, and worth saying: nothing has run across Binance's 24-hour
disconnect yet. The reconnect is tested against a server that drops the
connection, which is the same code path, but the real thing takes a day.

## 9. What this deliberately leaves out

No orders on Binance, no hedging, no position there: this is a price and
nothing else. No trade stream, no depth beyond the touch, no candles, no
history. One reference venue, not a composite - if Binance is down we stop
quoting rather than fall back to a second opinion we have not tested. The
series-to-symbol map is configuration, not a second column. Nothing from
Binance reaches the books page, the ladder or positions. And no pricing
model: the guard decides when to leave, never what to quote.
