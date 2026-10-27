# CPM Order Management: Plan

Scope: the CPM **FIX order entry** service (`CPMOE`) and the order state
machine above it. Market data stays untouched in `market_data/`; this is
`order_management/`, and `market_data/` must never depend on it.

References: `docs/spec/fix-order-entry.txt` (the Order Entry section of
`docs/spec/cpm-api-spec.pdf`, cited as OE:L), `docs/spec/fix-dropcopy.txt`
(the read-only drop-copy feed, out of scope), and the market data design in
`docs/design.md`.

## 1. What the spec gives us

| | |
|---|---|
| Session | FIXT.1.1 / FIX 5.0 SP2, `DefaultApplVerID=9`, **port 6110**, `TargetCompID=CPMOE`, microseconds both ways |
| Heartbeat | default 10 s, client-specified, capped at 300 s |
| Clock | session tolerance ±60 s two-sided; **signature** tolerance ±30 s (tighter, separate) |
| Logon | `98`, `108`, `141`, `1137`, `554`, signature in `58` (or `96`), plus `20127` settlement opt-in and `8013` cancel-on-disconnect |
| Signature | HMAC-SHA256 over `SendingTime(ms, 21 chars) + ApiKey + TargetCompID + Password` — the same recipe `shared/common/signing` already implements, with `CPMOE` |
| Client → server | `D` NewOrderSingle, `F` OrderCancelRequest, `q` OrderMassCancelRequest, `F1` LastExecIDRequest, `F3` EventResendRequest |
| Server → client | `8` ExecutionReport, `9` OrderCancelReject, `r` OrderMassCancelReport, `UMS` MarketSettlementReport, `F2` LastExecID, `F4`/`F5` resend complete/reject, `j` BusinessMessageReject |
| Not supported | `G` amend (cancel and re-submit instead), `H` order status (use `F3`), quote/RFQ |
| Encoding | ordinary FIX decimals on the wire: price 3 dp (0.001–0.999), qty 2 dp, **amount 6 dp** (fees, caps, payouts) |
| Identifiers | `17`, `37`, `1369`, `20105` are opaque 64-bit integer text; one shared increasing space for `17`/`1369`/`20105`; `0` is a sentinel |
| Sides | only `1`=Buy and `2`=Sell, both against the **YES** contract. `BUY_NO at P ≡ SELL YES at 1.000 − P` |
| Required per order | `1028` ManualOrderIndicator (CFTC 1.35) and a `453` Parties group with exactly one `452=24` sub-account |

Order types: `40=1` Market (forced to the worst admissible price and to IOC,
no `44`) or `40=2` Limit (`44` required). `59` = GTC(1, default) / IOC(3) /
FOK(4). `18` post-only slide(6) or reject(A), mutually exclusive with market
orders. `21009` MaxExecutionCost is FOK-only. SMP via `7928`/`8000`.

**Two rejection paths, one outcome.** Anything detectable on receipt comes
back as `j` BusinessMessageReject with no ExecutionReport at all (bad
`40`/`44` combination, post-only market order, over-long `ClOrdID`, missing
sub-account, `21009` without FOK). Everything else comes back as
`8` with `150=8` Rejected and `103` OrdRejReason. The state machine has to
treat both as terminal for that `ClOrdID`.

**Terminal states that are easy to get wrong.** An IOC remainder and an
unfillable FOK both terminate as `150=C`/`39=C` **Expired**, not Canceled, and
carry no `103`. A `MaxExecutionCost` breach is **Rejected** with `103=3`, from
the same pre-match check. A cancel you asked for always reports Canceled, even
for an order you have not yet seen an ExecutionReport for.

## 2. The hard constraint: there is no position or balance API

The REST surface is read-only reference data (series, events, instruments).
Nothing in the documented API returns open orders, positions or balances. So:

- **Order and position state exist only in our process**, built from
  ExecutionReports.
- After a restart or a disconnect the only way to rebuild is `F1` (newest
  ExecID the exchange holds) plus `F3` replay by ExecID, over a retention
  window that is **24 hours today and explicitly not a contract**.
- Therefore the ExecID bookmark must be **durable** — written to disk as events
  are processed. This breaks the market data rule that nothing is persisted,
  and it is the one place where order management needs storage.
- `UMS` settlement reports are the only server-side position figures we ever
  see, at firm-instrument granularity, and only if `20127=Y` at Logon.

## 3. Stages

Same shape as the market data build: one stage, its tests, a summary, then stop.

**Stage 0 — Spec ingest and dictionaries. DONE 2026-10-26.** This document,
the open questions in `docs/open-questions-oms.txt`, and the two dictionaries:

- `config/FIXT11-CPMOE.xml` (51 fields): standard FIXT.1.1 plus the three CPM
  additions on Logon — the signature in `Text(58)` or `RawData(96)`,
  `ReceiveSettlementReports(20127)` and `CancelOrdersOnDisconnect(8013)` — and
  the order entry `MsgType` catalog.
- `config/FIX50SP2-CPMOE.xml` (58 fields): `D F q F1 F3` outbound, `8 9 r UMS
  F2 F4 F5 j` inbound, with `1028`, `7928`, `8000`, `21009`, `20105`,
  `21052`–`21056`, `22003`–`22006`, `1369`. `G` and `H` are deliberately absent,
  so our own engine refuses to send what the venue does not support.

Conditional spec fields are `required="N"` and enforced in application code, so
the gateway's reject path answers a bad request instead of a session-level
`Reject(3)` from our engine. Values the spec calls "defined but never emitted"
(`150`=5/6, `39`=6, `434`=2, `531`=1) are declared anyway, so a message
carrying one parses rather than failing.

Verified by 14 tests in `order_management/tests/dictionary_test.cpp`: no
duplicate tag numbers or names and no undefined references in either file;
every worked example from the spec validates; one message of every type in the
catalog validates, including the 5-entry Parties group on an ExecutionReport,
the fee group with a negative maker rebate, a replayed event with
`PossResend=Y`, and the `0` identifier sentinels; and the shapes that must fail
do fail — `G`/`H`, a missing `ClOrdID`/`Symbol`/`Side`/`OrderQty`/`OrdType`/
`ManualOrderIndicator`, a missing Parties group, a group count that disagrees
with its entries, `Side=3`, an unknown `ExecType`, and market data messages on
this session.

Two things the dictionary work turned up. `RawData(96)` is a `DATA` field, so
`RawDataLength(95)` must be its exact byte count or the parse runs off the end
of the field — one reason to prefer `Text(58)` for the signature, which is also
what the spec says takes precedence. And the spec's own Logon example sends
`20001=my-access-key`, which its Logon field table and its own sentence "not in
a Logon-body field" both exclude; the dictionaries follow the normative text,
and a test pins that the example as printed is refused (OE-Q3).

**Stage 1 — Session. DONE 2026-10-26.** `OeClient` (`cpm/oms/oe_client.hpp`)
and `OeApplication` (`src/fix/`), on the shared session code: a signed Logon
with `TargetCompID=CPMOE` — the same HMAC recipe as market data, which takes
the target from the header — `ResetSeqNumFlag=Y`, microsecond `SendingTime`
with zero sub-millisecond digits so the signed value and the wire value agree,
heartbeats, a clean logout, and raw logging with the API key redacted. No
message store: order state is rebuilt from the durable bookmark, not from FIX.

The Logon carries the two session options on **every** connection rather than
relying on the gateway's defaults, because neither can be changed afterwards:
`CancelOrdersOnDisconnect(8013)` and `ReceiveSettlementReports(20127)`, both
from `creds-oe.env`, and the choice is logged.

`Logout(5)` carrying `"replaced by newer connection"` is its own condition
(`SessionEvent::Kind::Replaced`, `OeClient::replaced()`, exit code 5). It is
not a disconnect: the API key is still live, so cancel-on-disconnect does
**not** fire and this session's resting orders stay in the market under the new
connection. That gets logged at error level, because it is the one logout where
orders are left behind.

**`TargetCompID` is configuration, not a constant.** `CPM_OE_TARGET_COMP_ID`
(default `CPMOE`) and `CPM_FIX_TARGET_COMP_ID` (default `CPMMD`) set it per
service, and it is validated as printable ASCII without `=` or spaces, at most
64 characters, since it goes on the wire. It is also part of the Logon
signature payload, so the value a session is configured with is the value that
gets signed: `apply_logon_auth` reads tag 56 back out of the header QuickFIX
filled in, which is why one signing routine serves both gateways. Two tests
pin that end to end — a non-default comp id logs on against an acceptor playing
the same value, and a client signing for `CPMOE` cannot log on to a gateway
expecting something else. `md_sim` reads the same variable so a loopback pair
stays in step.

**The endpoint guard is now per service.** `validate_endpoint` takes the host
it will accept: a market data config accepts only `kMdSandboxHost` and an order
entry config only `kOeSandboxHost` (`fix-order.cpm-sandbox.fairx.net`, OE-Q1).
That is stricter than a shared allowlist — neither client can be pointed at the
other gateway, which is also why the environment variables are `CPM_OE_*`.

`oe_client` logs on, heartbeats and logs out. It places no orders.

**Verified against the live gateway on 2026-10-26** (**OE-O1**,
`docs/spec/capture-oe-20261026T0845-logon.log`): accepted on the first attempt.
That settles OE-Q1 (the endpoint) and OE-Q3 (the API key travels in
`SenderCompID(49)` alone; the spec's `20001` example is wrong), confirms the
signature recipe against `CPMOE` with no change to the shared signing code, and
confirms the key carries the `TRADE` permission — so from Stage 3 it can place
real sandbox orders. The session also shows the same ~30 s `TestRequest` race as
market data (O3/Q17), which matters for Stage 5 timing.

Tested by 8 loopback tests against an in-process `CPMOE` acceptor that verifies
the signature with the spec's recipe: logon and clean logout with the right
session events, the Logon body field by field, all three cancel-on-disconnect
scopes sent verbatim, a bad signature rejected with the server's reason,
displacement by a newer connection, every host but its own sandbox refused,
heartbeats keeping the session up, and `stop()` idempotent. Plus 6 config tests
covering the per-service guard, the two variable sets coexisting in one
environment, and every session option parsed or refused.

**Stages 2 and 3 are swapped (2026-10-26).** The plan had the simulator first,
but market data's quality came from the opposite order: capture real traffic,
then build the simulator to copy it. `md_sim` reproduces layouts observed in
O3-O7 field for field, which is why its decoder worked first time against the
live gateway. Order entry has no inbound captures at all — OE-O1 saw only a
Logon ack — so a simulator written from the spec would encode my reading of a
document that has already been wrong twice. Stage 3 therefore comes first, then
one real order is captured, and `oe_sim` is built to match what was observed.

**Stage 3 — Submit and decode. DONE 2026-10-26.**

Plain structs in `cpm/oms/orders.hpp`: `NewOrder`, `CancelOrder`, `MassCancel`
outbound; `ExecutionReport` (with a nested `Fill`), `CancelReject`,
`MassCancelReport`, `SettlementReport`, `LastExecId`, `ResendComplete`,
`ResendReject`, `BusinessReject` and `DecodeError` inbound. No QuickFIX in the
header, as with `cpm/md/events.hpp`.

*Decimals.* Amounts are a third scale (6 dp) and may be **negative**, because
`MiscFeeAmt(137)` is a maker rebate when it is. And order entry **truncates**
excess precision toward zero where market data rejects it (OE-Q8), so
`parse_signed_truncating` sits beside the strict `parse_scaled` rather than
replacing it: a client that refused `0.6505` would refuse a value the venue
itself accepts as `0.650`.

*Rules before the wire* (`cpm/oms/order_rules.hpp`). The checks the gateway
makes on receipt are made here first, so a request that could only come back as
`BusinessMessageReject(j)` is never sent: `ClOrdID` within 16 bytes of printable
ASCII, a sub-account configured, positive quantity, `OrdType`/`Price` agreeing
both ways, price inside 0.001–0.999, post-only refused on a market order,
`MaxExecutionCost` only on an FOK and only positive. They are not a substitute
for the gateway: price bands, tick sizes, balances, position limits, instrument
state and the registered SMP id are all server-side.

The sub-account name is checked too. A sub-account does not need to exist
beforehand — the venue creates it lazily on the first order that names it — but
its allowed character set depends on its length, in five bands (OE-Q2), and an
off-charset name comes back as `BusinessMessageReject(j)` "invalid PartyID",
which under OE-Q6 would halt the run. `check_subaccount` enforces the bands
locally instead.

`make_cl_ord_id` builds ids as a prefix plus a base36 counter (OE-Q9) and
**throws** rather than emitting one that would not fit the venue's 16 bytes — a
prefix of six characters or fewer leaves room for 36^10 orders.

*Client-side limits* (`OrderLimits`): an `allow_orders` switch that keeps a
read-only process from ever placing an order, a maximum quantity, a maximum
notional per order (a market order is bounded at the worst admissible price,
since the real one is unknown), and an orders-per-second cap. **Cancels bypass
all of it**, including the switch: stopping must always be possible, which is
also how the gateway treats cancels against its own throttle.

*Tested* by 36 tests: 5 on the signed and truncating decimals, 11 on the rules
and the id generator, and 20 that take both directions of the wire — every
message the builder produces is re-parsed and validated against
`config/*-CPMOE.xml`, so what we would send is known to be dictionary-valid,
and the spec's own worked reports decode field for field, including the maker
rebate, the Expired-versus-Rejected distinction, every unsolicited cancel
reason, a replayed event, the settlement payout identity, and what happens to a
report missing `LeavesQty` or carrying a malformed timestamp.

`oe_client` gained `--test-order SYMBOL --price P [--qty Q] [--side] 
[--cancel-after SECS]`, which places one post-only limit order and cancels it,
and `--mass-cancel`, the kill switch. The order path needs `--allow-orders`
as well; the kill switch does not.

**Verified against the live gateway on 2026-10-26** (**OE-O2**,
`docs/spec/capture-oe-20261026T1018-order.log`): one resting limit buy and its
cancel, both reports decoded with zero errors, and now replayed through the
decoder by `tests/capture_replay_test.cpp` on every build. Two things that were
assumptions are now facts: the Parties group really is `PartyID(448)` then
`PartyRole(452)` — our dictionary's delimiter choice, which would have broken
parsing if the gateway emitted them the other way round — and a sub-account is
indeed created by the first order that names it. One finding for Stage 4: a
cancel's report carries the **cancel's** `ClOrdID` with the original in
`OrigClOrdID`, so order lookup must resolve by `OrigClOrdID` when it is set.

**Stage 2 — Simulator (`oe_sim`). DONE 2026-10-26**, built after the capture so
the layouts it emits copy OE-O2 where that observed them.

`order_management/sim/`: a matching engine in plain C++ (`engine.hpp`) and a
QuickFIX acceptor over it, the same split as `md_sim`. Unlike the market data
book, order entry is one-sided in price terms — `Side(54)` Buy and Sell both
mean the YES contract — so this is an ordinary two-sided book that crosses when
bid >= ask, with price-then-time priority.

What the engine implements:
- resting GTC orders, and partial fills that leave the remainder resting;
- **two fills per trade**, taker first, sharing one `TrdMatchID`, with a taker
  fee and a maker rebate (the fee schedule is not in the spec, so it is basis
  points of notional and configurable — an assumption);
- IOC remainders that **Expire** with `TIME_IN_FORCE`, never Cancel;
- FOK checked **before anything executes**, expiring with `FOK_NOT_FILLED` and
  leaving the book untouched;
- `MaxExecutionCost` from the same pre-match check, but **Rejected** with
  `OrdRejReason=3`, not Expired;
- post-only reject and slide, the latter stepping just inside the opposite best;
- market orders forced to the worst admissible price and to IOC;
- speed-bumped instruments accepting as `PendingNew` until released on a timer;
- the two rejection paths kept distinct: what the gateway detects on receipt
  comes back as `BusinessMessageReject(j)` with no report at all, and
  everything else as `150=8` with an `OrdRejReason`;
- cancels that name the cancel in `ClOrdID` and the order in `OrigClOrdID`
  (**OE-O2**), with `USER_REQUEST` and no `ExecRestatementReason`;
- mass cancel with its filters ANDed, each order reported individually as
  `USER_MASS_CANCEL` before the summary, whose `TotalAffectedOrders` is the
  only honest signal of what ran;
- `F1`/`F2` and `F3`/`F4`/`F5` replay from the events already sent, with
  `PossResend(97)=Y` and the original `ExecID` and `TransactTime` preserved.

**Orders are owned.** A session may cancel only its own, which is what keeps a
mass cancel from flattening the simulator's seeded liquidity — found by a test
that expected 2 cancels and got 5. Another session's order is simply not found,
reported as `UNKNOWN_ORDER`.

**Not simulated:** self-match prevention (the tags are accepted and echoed but
nothing fires), cancel-on-disconnect, `UMS` settlement pushes, and the fault
flags. Those arrive with the stages that need them.

Tested by 29 tests: 18 on the engine directly, covering every rule above, and
11 driving `oe_client` against `oe_sim` end to end — a resting order and its
cancel, a crossing fill with its fee, an IOC remainder, a duplicate `ClOrdID`
rejection, a cancel of an unknown order, mass cancel with per-order reports and
the summary count, `F1`/`F3` replay marked as replays, a speed-bumped order,
and one that runs the whole path and asserts nothing failed to decode.

**Stage 3 — Submit and decode.** Build `D`/`F`/`q` from plain structs, decode
`8`/`9`/`r`/`j`/`F2`/`F4`/`F5`/`UMS` into plain structs with no QuickFIX in the
headers, exactly as `cpm/md/events.hpp` does. Strict field validation that
reports instead of throwing, and `Amount` as a third scale (6 dp).

**Stage 4 — The order state machine. DONE 2026-10-26.**
`cpm/oms/order_state.hpp`: one `OrderRecord` per order and one `Position` per
instrument, built from the reports, plain C++ and mutex-guarded with copy-out
reads like `MarketState`.

- **Resolution is by `OrigClOrdID` first** (OE-O2): a cancel's report names the
  *cancel* in `ClOrdID`, so keying on that alone would invent a second order
  and leave the real one looking live. Failing that it tries the `ClOrdID`, the
  cancel we recorded when we sent it, and finally the `OrderID`.
- **Orders are recorded when sent**, not when first reported, so a
  `BusinessMessageReject` — which carries no report at all — still lands on the
  order it refers to and is terminal for it.
- **ExecIDs are deduplicated.** Event Resend delivers the same events again
  with the same `ExecID`; applying one twice would double a fill into the
  position. A repeat is counted (`duplicate_reports()`) and otherwise ignored.
- **The average price is ours.** The gateway omits `AvgPx` on a cancel of an
  order that had fills, so the record keeps a running quantity-weighted value —
  and it is computed after the fill is accumulated, which a test caught when it
  briefly used the previous total.
- **Positions are derived from fills** (OE-Q11), signed in YES contracts with
  buys adding and sells subtracting, carrying cash and fees. They are marked as
  derived, because the venue publishes nothing until settlement.
- **Anomalies are reported, not thrown**: a report for an order we never sent,
  a report after a terminal state, `CumQty + LeavesQty` disagreeing with
  `OrderQty`, a trade with no fill fields, a status going backwards, an
  `OrderID` changing, and the two settlement checks of Stage 6.

**Stage 5 — Reconciliation and recovery. DONE 2026-10-26.**

`cpm/oms/bookmark.hpp` is the one durable file the order manager writes
(amendment 6): the newest `ExecID` processed and the `ClOrdID` counter, two
lines of `key=value`, written to a temporary and renamed into place so a crash
leaves one whole version. A **corrupt bookmark throws** rather than starting
from zero, which would silently replay nothing.

`cpm/oms/reconciler.hpp` drives the sequence OE-Q6 chose:
- **Reconcile before submitting anything.** Every logon sends `F1`; if the
  exchange's newest `ExecID` is ahead of the bookmark, `F3` replays the gap and
  submission stays blocked until `F4`. `OeClient::set_submission_gate` is what
  enforces it, and cancels and the recovery requests are never gated.
- **Flatten, reconcile, halt.** A gap fill, any reject, a disconnect, a
  displacement or an order-flow anomaly raises one `Halt`: a mass cancel goes
  out, the bookmark is saved, and the run stops. Submission is never reopened —
  a reconcile that completes after a halt finishes in `Halted`, not `Live`.
- Two exceptions that matter. A session **displaced by a newer connection** is
  not flattened: its orders now belong to that session, and cancelling from
  here would cancel theirs. And a halt raised while the session is **down**
  cannot cancel anything, so the mass cancel is sent on the next reconnect
  instead.
- **A shutdown the owner asked for is not a halt.** `stop()` silences the
  reconciler first, because the client's own logout would otherwise arrive as a
  Disconnect — the identical trap the market data monitor hit, found again here
  by running the binary rather than by reading the code.

Gap detection mirrors market data's: an inbound `SequenceReset` with
`GapFillFlag=Y`, plus `MsgSeqNum` tracking adopted from each Logon. Order entry
does support `ResendRequest(2)`, so the session layer repairs the sequence —
but the business events in the gap are gone, and `F3` is what brings them back.

**Stage 6 — Settlement and payouts. DONE 2026-10-26.** `MarketSettlementReport`
updates the instrument's position with its payout and settlement price, and
checks two things the report must satisfy:
- the identity `payout = price x long + (1 - price) x short`;
- under net aggregation, that the settled quantities agree with the position
  our own fills imply. **A disagreement means we missed a fill**, which is
  exactly the condition worth stopping on, so it is an anomaly and therefore a
  halt. Under gross aggregation the legs are per sub-account and need not match.

`oe_sim` can push a settlement (`Simulator::settle`) so both paths are tested:
one that agrees with the fills and raises nothing, and one that does not and
halts the run.

`oe_client` ties it all together: `--bookmark PATH`, reconcile-before-submit,
a halt that exits 4, and a summary of every order and position at the end.

## 4. Safety rails, proposed for Stage 1 rather than last

Market data could only read. These are cheap now and expensive to retrofit:

- **Sandbox-only endpoint guard**, as in market data, but now it protects
  against sending real orders.
- **Client-side limits before the wire**: max order quantity, max notional per
  order, max open orders, max orders per second (the spec says a session rate
  throttle exists but never gives its value), and a global kill switch that
  mass-cancels and refuses new orders.
- **`ManualOrderIndicator(1028)` is a compliance field**, not a formality: `N`
  for automated. It must be set from configuration, never defaulted silently.
- **`ClOrdID` scheme** that is unique among live orders, ≤16 printable ASCII,
  and reconstructable after a restart, so a cancel-by-`ClOrdID` still works.
- **Every order logged before it is sent**, with the decoded reply joined to it
  by `ClOrdID` and `ExecID` — the `seq=`/logfmt work already discussed.

## 5. What this plan deliberately leaves out

Drop copy (`CBPMDC`, `docs/spec/fix-dropcopy.txt`) is the read-only audit feed
and is a separate container if it is ever wanted. No REST client. No strategy,
no signal generation, no risk engine beyond the client-side caps above. No
amend, because the venue has none.
