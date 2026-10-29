# Auto-roll: quoting a series, not an instrument

`BTCD15M` and `ETHD15M` are series, not instruments. Each exists as a chain of
15-minute windows, and quoting one means following that chain: finding the
window that is open now, and moving to the next one when it is not.

Written 2026-10-28, Stage R0. No code. The plan below is what R1-R4 build.

## 1. What the venue actually does

All of this is in the captures already in `docs/spec/`, not assumed.

**The ticker encodes the window.** `<SERIES>-<ASSET><YY><MON><DD><HHMM>-0`, where
HHMM is the window **end** in UTC ([MD] O2/O6). So
`BTCD15M-BTC26OCT260630-0` is the BTC 15-minute window ending 06:30 UTC on
2026-10-26. The end time is therefore known the moment the ticker is known -
which is what makes an orderly roll possible rather than a surprise.

**A roll takes about a second.** From `capture-sandbox-20261026T0607-roll.log`,
the 06:15 boundary for both of the series in question:

```
06:15:00.406  BTCD15M-BTC26OCT260615-0  Closed      (326=18)
06:15:00.455  ETHD15M-ETH26OCT260615-0  Closed
06:15:00.590  BTCD15M-BTC26OCT260615-0  Determined  (326=27)
06:15:00.674  ETHD15M-ETH26OCT260615-0  Determined
06:15:00.757  BTCD15M-BTC26OCT260630-0  Listed      (326=21)   <- the next window
06:15:00.773  ETHD15M-ETH26OCT260630-0  Listed
06:15:00.997  BTCD15M-BTC26OCT260630-0  Open        (326=17)   <- tradeable
06:15:00.998  ETHD15M-ETH26OCT260630-0  Open
06:16:00.281  both ...0615-0            Settled     (326=28)   <- ~60 s later
```

Windows do not overlap: the next one is listed ~0.35 s after the previous
closes and opens ~0.6 s after. There is nothing to choose between two live
windows, and nothing to hedge across them.

**Instruments announce themselves.** An all-instrument lifecycle subscription
(`269=L`, `146=0`) carries every window's `Listed` and `Open` as it happens
([MD] O6). No REST call and no name guessing is needed to *follow* a chain.

**But nothing announces the present.** The subscription is changes-only. In
`capture-sandbox-20261023T1117.log` it was sent at 11:17:17 and the first
message arrived at 11:20:00 - the next boundary. Per-symbol lifecycle behaves
the same way, which is why a book's status is `Unknown` until something
changes ([MD] Q13). Neither FIX session offers a SecurityList: there is no
message in `docs/spec/fix-market-data.txt` or `fix-order-entry.txt` that asks
"what is listed right now". **So a daemon that starts mid-window cannot learn
the current ticker over FIX at all.** See R-Q2.

The resolver that fills that hole is a REST call, and its credentials reach
the daemon the same way the two FIX sets do - from the environment, never from
a file the code reads. So `creds-rest.env` has to be sourced **alongside** the
other two at launch:

```
source creds.env && source creds-oe.env && source creds-rest.env && \
  ./build/trader/cpm_trader --allow-orders
```

Miss it and nothing fails at startup - the daemon warns and runs, full tickers
and rolls that it watches happen still work - but the first series target is
refused with "REST is not configured (creds-rest.env)". `source creds-rest.env
&& python3 Temp/rest/current-window.py` checks the credentials themselves
without the daemon.

**A dead symbol is an error.** An instrument that closed about two minutes ago
is already "unknown symbol" to the gateway ([MD] O6), and *any* reject halts
the daemon - market data (Q27) and order entry (OE-Q6) alike. Rolling badly
therefore does not degrade; it stops the daemon. Section 4 is about that.

## 2. The shape

Three pieces, each testable on its own:

```
all-instrument lifecycle (269=L, 146=0)
        |
        v
  SeriesWatcher            market_data/, pure: no FIX, no sockets
   series -> { current window, its end time, the one before it }
        |
        v
  Trader                   subscribes the new window, unsubscribes the old
        |
        v
  StrategyHost             swaps each rolling strategy's instance at the boundary
```

**`SeriesWatcher`** (R1, built) takes `InstrumentLifecycle` events and answers
one question: what is the open instrument for series X? It parses the ticker
into series, asset, window end and suffix, and keeps the previous window until
it is evicted. It is a pure state machine over events, so every case in
section 1 is a unit test - the capture's own roll sequence, two series 50 ms
apart, a duplicate event, an `Amended` that is an event rather than a state,
and two windows briefly open at once.

Two rules in it are worth naming. A window whose **end has passed** is not
current whatever the feed last said: the feed is changes-only, so a `Closed`
that never arrived - a dropped message, a reconnect across the boundary -
would otherwise leave a dead window looking live, and quoting one is a
rejected order. And when two windows are somehow open at once it answers with
the one that **ends later**, because that is the one with trading left in it
and the other may be the `Closed` that was missed.

**The daemon** (R2) opens one all-instrument lifecycle subscription at
startup, feeds the watcher, and acts on a change: subscribe the new window's
book, unsubscribe the old one. Per-symbol lifecycle subscriptions go away -
the all-instrument one already carries every status, and overlapping
subscriptions only duplicate messages ([MD] O7).

**The host** (R2) gains a *target* per strategy: either a series (`BTCD15M`)
or an explicit ticker, which is what it takes today. On a roll it replaces the
strategy **instance** with a fresh one built from the same parameters and the
new symbol.

Replacing the instance rather than re-pointing it is the decision that keeps
`IStrategy` as it is: `symbols()` stays fixed for an instance's life, so
nothing about the existing seam changes, and no per-instrument state can
survive into a window it does not belong to. A market maker has nothing worth
carrying across a boundary - its live quotes are gone with the window, and its
counters are cosmetic and stay on the `Entry`, which survives. The strategy id
(`cryptomm-1`) survives too, so the GUI row, the operator's arming and the
claim all persist across rolls.

Claims move with it: GUI-Q6 gives a symbol to one strategy, and R2 makes that
a claim on the **target** instead. Two strategies on `BTCD15M` stay refused;
one on `BTCD15M` and one on `ETHD15M` are fine, which is exactly what is
being asked for.

## 3. The roll, step by step

The window end is known from the ticker, so the roll is scheduled rather than
discovered:

| When | What |
|---|---|
| end - `--withdraw-before` (default 5 s) | Withdraw the quotes **while the market is still open**, so the cancels are ordinary cancels |
| `Closed` seen | Stop quoting. Unsubscribe the old book at once, long before the symbol goes unknown |
| `Open` seen on the new window | Subscribe its book; build the new strategy instance; quote when a book arrives |
| ~60 s later, `Settled` | The old window's position settles to cash on its own (OMS stage 6). Nothing to do |

The early withdrawal is the important line, and it is worth being exact about
why. It is **not** that the orders must be got out - the venue cancels
anything still resting when the window closes, as an ordinary unsolicited
`Canceled` carrying `ExecRestatementReason=2`, and every layer here already
handles that report (R-Q1). It is that a cancel of *ours* arriving in the same
instant gets `UNKNOWN_ORDER`, and that halts the daemon. So the rule is cancel
early or not at all, never late.

The trading reason is the one that remains, and it is the stronger of the two:
the last seconds of a 15-minute binary are where the informed flow is, and a
resting quote then is an option about to expire, written for a tick of
spread.

## 4. What can go wrong, and what is done about it

| Risk | Why it would halt | What R2 does |
|---|---|---|
| Cancelling into a close | The venue cancels resting orders itself at `Closed` (`378=2`), so a cancel of ours that crosses it gets `UNKNOWN_ORDER`, and any `OrderCancelReject` halts (OE-Q6) | Cancel early, never late: withdraw ~5 s before the end, and send no cancel at all after that or once `Closed` is seen. Anything left is the venue's to cancel (R-Q1) |
| Unsubscribing too late | "no subscription for mdReqId" / "unknown symbol" rejects halt (Q27) | Unsubscribe on `Closed`, not on `Settled` |
| Subscribing too early | A symbol that is only `Listed` may not accept a book request | Subscribe on `Open`, never on `Listed` |
| Quoting a window that is not open | A `NewOrder` for a closed instrument is rejected, and a rejected order halts | The host already withdraws when an instrument is not tradable; the new instance quotes only once its book is valid |
| Starting mid-window | Nothing on FIX says what is open now | **R-Q2** |
| A position row per window | 96 windows a day per D15M series | **R-Q3** |

Two existing mechanisms already cover what looks like a third and fourth risk.
A closed window's book goes quiet, but `FeedMonitor` exempts a subscription
whose instruments have all finished, so the quiet does not raise a stale-feed
halt. And `MarketState::evict_finished` already drops settled instruments on a
timer, which is what keeps a day of rolling out of the heap ([MD] O2/O6,
GUI-N3).

## 5. What the operator sees

The strategy row shows the target and the window it is on - `BTCD15M` /
`BTC26OCT260630-0`, with the time left in the window, because "4 minutes to
the roll" is the thing an operator wants to know before pressing anything. The
ladder follows the new symbol by itself: the page already drops a ladder whose
symbol has gone and fetches the axis for one that appears.

Arming survives a roll. A strategy armed before the boundary is armed after
it, which is the point of rolling automatically - the alternative is an
operator re-arming every 15 minutes, and that is the behaviour GUI-Q13 was
written to avoid in a different form.

## 6. Stages

| Stage | Deliverable |
|---|---|
| **R0** | This document and `docs/open-questions-roll.txt`. No code. |
| **R1** | Done. `parse_ticker` and `SeriesWatcher` in `market_data/`: per-series current window from the lifecycle feed, 16 tests driven by the roll capture's own sequence. Pure - no FIX, no sockets, no clock of its own. |
| **R1b** | The REST resolver: one `GET /api/v1/instruments?series_ticker=X&status=OPEN` per series at startup and on reconnect. Proven by hand against the sandbox (`docs/spec/capture-rest-20261028T1205-instruments.txt`); needs an HTTPS GET client on the OpenSSL already linked, and the JSON reader moved from `gui/` to `shared/`. |
| **R2** | Done. The daemon and the host: the all-instrument lifecycle subscription, a series resolved at create, `StrategyHost::roll` replacing the instance, `ControlApi::roll` moving the subscriptions in the safe order. Verified against the sandbox: both series rolled at the 12:45 boundary on 2026-10-28, 1.5 s after it, with the daemon live throughout. |
| **R3** | Part done: the strategy row shows the series and the window it is on, and the ladder follows by itself. Time-to-roll and the positions policy (R-Q3) remain. |
| **R4** | A sandbox run across at least two real boundaries, with R-Q1 answered by observation rather than assumption. |

## 7. What this deliberately leaves out

No cross-window inventory: each window starts flat, and a position left in a
closed window settles to cash by itself. No trading through the gap between
windows - there is nothing listed to trade. No choosing between windows, no
calendar spreads, no hedging one window with the next. No REST client: if
R-Q2 is answered by waiting, the project's dependency list does not change.
