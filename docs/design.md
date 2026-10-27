# CPM FIX Market Data Client: Design

Scope: **market data only**. No order entry, execution reports, OMS or risk.

Sources:
- `docs/spec/cpm-api-spec.pdf`: full CPM API spec (REST, WebSocket, FIX OE, FIX MD).
- `docs/spec/fix-market-data.txt`: text extract of the *FIX Market Data* section. Section numbers (§) below refer to it.
- WebSocket section of the PDF: the FIX MD spec names the WebSocket channels as the "equivalents" of its FIX channels, so it is used for semantics the FIX section leaves implicit. Each such use is marked **[WS]**.
- Sandbox observations from a hand-written prototype logon on 2026-10-23, marked **[OBS]**.

---

## 1. Endpoints

| Environment | Host | Port | Transport |
|---|---|---|---|
| Sandbox | `fix-marketdata.cpm-sandbox.fairx.net` | 6120 | TLS **[OBS]** |
| Production | **not used by this project** | | |

The spec gives the MD port as "TBD" and no host (§1). The sandbox host and port came from the user, and TLS was confirmed by a successful TLS logon **[OBS]**. The spec says only "Transport: TCP" and never mentions TLS.

The client will **refuse to connect to any host that is not the sandbox host**. A plain hostname allowlist in config enforces this.

## 2. Session layer (§1, §2, §6)

| Property | Value |
|---|---|
| BeginString (8) | `FIXT.1.1` |
| DefaultApplVerID (1137) | `9` (FIX50SP2) |
| SenderCompID (49) | API key (must carry the `MARKET_DATA` permission) |
| TargetCompID (56) | `CPMMD` |
| HeartBtInt (108) | 30s recommended, set on Logon |
| ResetSeqNumFlag (141) | `Y` on every Logon. Sequence numbers are not persisted across restarts. |
| SendingTime | Spec: µs inbound, ms outbound. **[OBS]** the server sends µs (`20261023-05:41:10.530768`). We send µs and accept both. |
| Admin messages | Heartbeat 0, TestRequest 1, ResendRequest 2, Reject 3 (with 373), SequenceReset 4, Logout 5 |

Observed server header extras **[OBS]**: `SenderSubID(50)=TEST` and `LastMsgSeqNumProcessed(369)`. Both are standard FIXT.1.1 header fields.

Heartbeat behaviour **[OBS]**: the server sent `TestRequest(1)` exactly 30s after Logon, because the client had sent nothing in that time. The client must send its own heartbeats on the HeartBtInt cadence, regardless of what it receives. QuickFIX does this.

## 3. Logon and signing (§3)

Logon (35=A) body:

| Tag | Field | Value |
|---|---|---|
| 98 | EncryptMethod | `0` |
| 108 | HeartBtInt | `30` |
| 141 | ResetSeqNumFlag | `Y` |
| 1137 | DefaultApplVerID | `9` |
| 554 | Password | API passphrase (plaintext, non-empty) |
| 58 | Text | HMAC signature (non-empty) |

The API key travels in header `SenderCompID(49)`. The Logon body has no separate key field.

**Signing recipe.** The MD section (§3) only says "an HMAC signature (Text 58)". The recipe is taken from the *FIX Order Entry* section, with `TargetCompID` changed to `CPMMD`:

```
payload   = SendingTime_ms + ApiKey + "CPMMD" + Passphrase      (UTF-8, no separators)
            SendingTime_ms = SendingTime(52) re-encoded as yyyyMMdd-HH:mm:ss.SSS (exactly 21 chars)
key       = base64_decode(signing_secret)
signature = base64(HMAC_SHA256(key, payload))                     (44 chars)
```

This is **verified on the sandbox [OBS]**: the server answered Logon(A). To keep the signed time and the sent time identical, the client generates a ms-precision time and sends it in tag 52 with `000` appended as the µs digits.

The Order Entry section adds a ±30s signature clock window. We assume the MD gateway applies the same rule, so the host clock must be NTP-synced.

Failure behaviour: the server sends Logout(5), usually with Text(58), and closes the connection.

Credentials come only from the environment variables `CPM_API_KEY`, `CPM_PASSPHRASE` and `CPM_SECRET`. They are never logged: the raw message logger masks `554=` and `58=` on outbound Logon. `creds.env` stays local and is read by the user's shell (`source creds.env`); the code never reads or prints that file.

## 4. Message catalog (§2)

| MsgType | Name | Dir | Purpose |
|---|---|---|---|
| A, 0, 1, 2, 4, 3, 5 | session | ⇄ / ← | see §2 |
| V | MarketDataRequest | → | subscribe (263=1) / unsubscribe (263=2) |
| W | MarketDataSnapshotFullRefresh | ← | snapshot: book, volume, open interest |
| X | MarketDataIncrementalRefresh | ← | deltas: book, trades, volume, OI |
| Y | MarketDataRequestReject | ← | subscription rejected |
| f | SecurityStatus | ← | instrument lifecycle |
| j | BusinessMessageReject | ← | app-level reject of invalid or unknown inbound messages (380) |

## 5. CPM extensions inventory

The spec's header claims extensions "in the 20000+ / U-prefixed / F1–F5 ranges". Every such item is defined in the **Order Entry** section only:

| Item | Section | Used by MD? |
|---|---|---|
| Tags 20105, 20127, 21009, 21052–21056, 22003–22006 | Order Entry | **No** |
| MsgType `UMS` (MarketSettlementReport) | Order Entry | **No** |
| MsgTypes `F1`–`F5` (exec-ID event replay) | Order Entry | **No** |
| `MDEntryType(269)=L` (InstrumentLifecycle) | MD §4.1 | **Yes** |
| `SecurityTradingStatus(326)` CPM values | MD §6 | **Yes** |
| `Text(58)` on Logon | MD §3 | **Yes** (non-standard placement) |

So the MD dictionaries contain only the last three. See **Q1**.

## 6. Dictionaries (`config/`)

- `FIXT11-CPMMD.xml`: transport dictionary. Standard FIXT.1.1 header, trailer and admin messages, plus `Text(58)` on Logon.
- `FIX50SP2-CPMMD.xml`: application dictionary. Only V, W, X, Y, f, j are defined.

Decisions:
- Spec "C" (conditional) fields are `required="N"` in the dictionary and checked in application code. This lets the simulator answer bad requests with `Y`/281 as the real gateway does, rather than with a session-level `Reject(3)`.
- `SubscriptionRequestType(263)=0` is in the enum even though CPM rejects it (281=4), for the same reason.
- X carries Symbol and SecurityID both at message level and per entry. The spec's table puts them per entry, but its trade examples put them at message level (**Q6**).
- The group delimiters, which QuickFIX requires to be the first field of each entry, are: V/267 → 269, V/146 → 55, W/268 → 269, X/268 → 279.

Verification: a script checks that the XML parses, that no tag number is duplicated, that every referenced field is defined, and that every tag appearing in the spec's MD tables and examples is defined. All checks pass.

## 7. Subscriptions (§4)

MarketDataRequest (V): `262 MDReqID`, `263` (1 = subscribe, 2 = unsubscribe), `264 MarketDepth` (book only), `266 AggregatedBook=Y` (the only supported mode), `267/269` entry types, `146/55|48` instruments.

| 269 | Channel | Scope | On subscribe | Then |
|---|---|---|---|---|
| 0 | Book: YES bids | per-instrument only | W snapshot | X deltas |
| 1 | Book: NO bids ("Ask") | per-instrument only | W snapshot | X deltas |
| 2 | Public trades | per-instrument or all (146=0) | nothing | X |
| B | Trade volume | per-instrument or all | W snapshot per §4.2, but the live gateway sends none (**O8**) | X updates |
| C | Open interest | per-instrument or all | as above (**O8**) | X updates |
| L | Instrument lifecycle | per-instrument or all | nothing | f |

Rules from §4 and §4.3:
- `146` must always be present: 0 means all instruments, and is invalid when 269 includes 0 or 1. The distinct Symbol count must equal 146 (281=9).
- `264` is required on book requests and forbidden on non-book requests (281=5). Accepted values are 1 (L1) and 10 (L2, which is capped at 10 levels per side). **Q3** covers 264=0.
- 281=1 is returned when an MDReqID is already active on the session.
- MDReqID is echoed on every W, X and Y, and is the handle for unsubscribe.

Client policy (pending **Q4** and **Q5**): one V per channel group: book (0+1), trade (2), volume and OI (B+C), lifecycle (L). MDReqIDs are fresh on every session.

## 8. Market data semantics (§5, §6)

### Encoding: fixed-point, never double

| Quantity | Wire | Internal |
|---|---|---|
| Price (270) | 3 dp, range 0.001–0.999 | `int64` in thousandths (`0.580` → `580`) |
| Size, volume, OI (271) | 2 dp (e.g. `30.72` **[WS]**) | `int64` in hundredths (`300.00` → `30000`) |

The parser is a strict decimal-string parser. It rejects more decimal places than the scale allows, and it never goes through `double`. See **Q11**.

### Order book
- Both sides are **bids**. 269=0 holds the YES bids and 269=1 holds the NO bids. The NO price is the NO contract's own price. The implied YES ask is `1.000 − best_no_bid` (§5 L1 example: "best NO bid 0.380 → yes_ask = 0.620").
- W snapshot: at most 10 levels per side, and it is authoritative, so the client replaces the local book with it **[WS]**. Entries are best-first.
- X delta: **absolute** quantity at the price level. Replace the level, never add or subtract. Size 0 or 279=2 removes the level. 279=0 and 279=1 both mean "set level".
- The book is keyed by **price**. MDPriceLevel(1023) is the price's **position after the change**, confirmed on 969 of 969 observed entries (**O9**); the spec's own example contradicts that and is wrong. It stays out of book state (the book is keyed by price, not position), but Stage 4 can use it as a cross-check.
- L1 (`264=1`) sends one level per side: a size change at the same best price is a single `279=1`, and a new best price is a `279=2` delete of the old best followed by a `279=0` new (**Q8**, confirmed by **O9**).
- Crossed or locked check: `best_yes_bid + best_no_bid ≥ 1.000` (**Q9**). Still unobserved: over 1142 captured entries the sum never passed 0.990 (**O9**), so the rule has never been violated and never been exercised.

### Trades (X, 269=2)
- 270 is the YES price, and the NO price is `1.000 − px`. 271 is the contract count.
- 278 MDEntryID is unique per execution and is used for deduplication.
- 2446 AggressorSide: 1 = Buy, 2 = Sell, 0 = NoAggressor (opening or uncrossing fill). The mapping to YES/NO taker is **Q10**.
- No snapshot is sent on subscribe.

### Volume and open interest (W then X; 269=B and 269=C)
- Only 271 is set: cumulative contracts traded (B) or open contracts (C). 270 is absent.
- Live on 2026-10-26 (**O8**): both are populated and update about once a second per instrument, as `X` with `279=0` and `55`/`48` per entry. The gateway sends **no `W`** for this channel, although §4.2 says a snapshot is sent on subscribe, so a client must not wait for one. This supersedes **O1** and the **[WS]** "implementation pending" note.

### Instrument lifecycle (f)
Fields: `55`, `48`, `326`, optional `324` (echo of MDReqID), `60`, `58`. No snapshot is sent on subscribe.

| 326 | CPM meaning | Standard FIX meaning of the same code |
|---|---|---|
| 21 | Listed | Pre-open |
| 17 | Open | Ready to trade |
| 2 | Pause | Trading halt |
| 18 | Closed | Not available for trading |
| 27 | Determined | *(CPM-defined)* |
| 28 | Settled | *(CPM-defined)* |
| 26 | Amended | Post-close |
| 29 | Delisted | *(CPM)* |

The spec defines no allowed transitions (**Q14**). Initial state is unknown until the first `f` arrives (**Q13**).

### Rejects
- Y: `262`, `281` (0 unknown symbol, 1 duplicate MDReqID, 4 bad 263, 5 bad 264, 8 bad 269, 9 bad scoping), and `58`, which distinguishes cases that share a 281 code.
- j: `380`, plus `58`.

## 9. Recovery model (project constraint, consistent with spec)

- Store: in-memory or null. No resend is relied upon, and nothing is persisted.
- The spec says nothing about whether the MD gateway honours ResendRequest(2). **[WS]**: "If you detect a gap… unsubscribe and resubscribe."
- Triggers: a MsgSeqNum gap, a disconnect, or a stale feed (no data for N seconds).
- Action: mark the affected books invalid, re-establish the session (logon with 141=Y), resubscribe with fresh MDReqIDs, and rebuild state from the new W snapshot. Trades and lifecycle events have no snapshot, so any missed during the outage are lost. This gap will be reported to callers.

## 10. Toolchain status (2026-10-23)

| Need | Status |
|---|---|
| g++ 15.2 (C++20), OpenSSL 3.5.5 dev headers | installed |
| CMake 4.2.3, GoogleTest 1.17.0 | installed (apt) |
| QuickFIX/C++ v1.16.0 | built from the release tarball via FetchContent, with SSL and a TLS hostname-check patch (`cmake/patch_quickfix.cmake`) |
| git | not used: no version control, by decision (2026-10-23) |

Build with `cmake --build build -j2`. An unbounded parallel build of QuickFIX exhausted this machine's 7 GB of RAM.

## 12. Simulator: md_sim (Stage 2)

`md_sim` is a local stand-in for the gateway. It is a QuickFIX acceptor on plain TCP that plays `CPMMD` for one client API key. Code: `market_data/sim/`, with a thin `main` in `market_data/apps/md_sim/`.

```
terminal 1:  source config/sim.env && ./build/market_data/md_sim [--roll-secs 30]
terminal 2:  source config/sim.env && ./build/market_data/md_client --capture SIMGAME-26NOV01HOMAWY-HOM
```
`config/sim.env` holds **fake** credentials and points md_client at `127.0.0.1:6121`.

**Fidelity.** Every message copies a layout captured on the sandbox (`docs/open-questions.txt` O3-O7):

| Message | Layout |
|---|---|
| W book | `262\|55\|48\|268[269\|270\|271\|1023]\|60`. Empty book = `268=0`. |
| X book | `262\|268[279\|269\|55\|48\|1023\|270\|271]\|60`. Delete has no 1023 and `271=0`. Change carries the absolute size. |
| X trade | `279=0\|269=2\|55\|48\|270\|271\|278\|2446`, sent just before the book update it caused, with the same 60 |
| f | `55\|48\|326\|60`, never 324 or 58 |
| Y | `262\|281\|58`, with the sandbox's texts `unknown symbol=<SYM>` and `no subscription for mdReqId=<ID>` |
| Prices / sizes | no trailing zeros: `0.44`, `0.4`, `0.435`; `2`, `2.5` |
| Header / logout | `50=TEST`. The logout reply carries `58=CLIENT_LOGOUT`. |

Each subscription gets its own copy of every update, as the sandbox does for overlapping subscriptions (O7).

**Market model** (`Market`, plain C++, seeded RNG): three long-lived `SIMGAME-*` instruments by default, plus, with `--roll-secs`, a rolling `SIMROLL-SIM<YYMONDDHHMMSS>-0` series. Events are add, partial or full cancel, and trade at `--rate` per second. Books stay best-first, at most 10 levels per side, never crossed or locked, on TAPERED_DECI_CENT ticks. A roll sends Closed, then book deletes, then Determined, then Listed and Open for the next window, and Settled `settle_delay` later. After that the old symbol is unknown, as on the sandbox (O6).

**Where the sandbox could not show behaviour, the simulator follows the design's proposals.** Two of these were confirmed on 2026-10-26 (**O9**) and are no longer assumptions:
- **Multi-level L2 books** (Q7): 1023 is the price's position after the change. **Confirmed.**
- **L1** (Q8): a size change is a Change at level 1. A new best price is a Delete of the old best plus a New. **Confirmed.**
- **Volume/OI**: the simulator sends the spec's W then X. The live gateway sends **X only, no W** (**O8**), so the simulator is stricter than the real thing here.
- **Unobserved reject texts** (281=1/4/5/8/9) use the simulator's own wording. Mixed channel groups get 281=8 (Q4).
- **Logon**: SendingTime must be within ±30 s.

**Faults.** Each fires once per run:

| Flag | Effect |
|---|---|
| `--drop-seq N` | The first market data message with MsgSeqNum ≥ N is not sent. ResendRequests get a gap fill, since nothing is persisted. |
| `--disconnect-after N` | After the Nth application message of a session, the TCP connection drops with no Logout. |
| `--stale-after N` | N seconds after logon, every active subscription goes silent; heartbeats continue. New subscriptions get data. |

**Limits.**
- QuickFIX's acceptor has no bind-address setting, so md_sim listens on all interfaces. It only accepts the configured API key with a valid signature.
- `369` (LastMsgSeqNumProcessed) is not sent.
- The 30 s TestRequest race seen on the sandbox (O3) is not reproduced.

---

## 13. Decoding: Stage 3

The client turns every inbound market data message into the plain structs of
`market_data/include/cpm/md/events.hpp`, which have no QuickFIX in them:
`BookSnapshot`, `BookUpdate` (a list of `BookLevelChange`), `Trade`, `Volume`,
`OpenInterest`, `InstrumentLifecycle`, `MdReject` and `DecodeError`. Callers
register them in one call, `MdClient::on_market_data(MdCallbacks)`, before
`start()`; each callback runs on a QuickFIX thread, one message at a time.

**Fixed point** (`shared/common/include/cpm/shared/common/fixed.hpp`). `parse_scaled` reads a
decimal string straight into an `int64` at a fixed scale: digits with at most
one `.`, no sign, exponent, blank or separator, and never more decimal places
than the scale allows. `format_scaled` writes it back without trailing zeros,
which is the gateway's own formatting (O3/O4), so format-then-parse is the
identity over every price and size. `md_sim` now formats through the same code
the client parses with.

**Channel routing comes from the message, not from the subscription.**
`MDEntryType(269)` says which channel an entry belongs to (0/1 book, 2 trade,
B volume, C open interest), and `f` is lifecycle by message type, so decoding
needs no subscription state and survives a resubscribe with new MDReqIDs. Two
consequences:
- A `W` with no entries (`268=0`) is an **empty book snapshot**, because a
  volume/OI snapshot always carries `MDEntrySize` (spec §5.1). Observed on the
  sandbox as the answer to a book subscribe on an empty market (O3).
- A `W` carrying only B/C entries produces `Volume`/`OpenInterest` with
  `snapshot = true` and no book snapshot at all.

**Field rules.**

| Field | Rule |
|---|---|
| `55`/`48` | Per entry on `X`, else taken from message level (**Q6**); a `W` must carry them at message level. |
| `60` | Optional in every market data message (MD:311/364/782), though the sandbox always sends it: absent leaves the event time at the epoch, malformed drops the message. `yyyyMMdd-HH:mm:ss` with optional fraction, milliseconds and microseconds both accepted (**Q16**), anything finer truncated. |
| `279` | 0 and 1 both decode to `BookAction::Set`, 2 to `Delete`. A delete's `271` is 0, and an absent `271` on a delete is read as 0. |
| `271` | Absolute size at the level, never a delta (O4). On B/C an absent or zero value is kept as 0, not an error (**Q15**). |
| `1023` | Kept on the change for reference but never used for book state (**Q7**); a malformed one is reported and the change is still delivered. |
| `2446` | 1 → `YesTaker`, 2 → `NoTaker`, 0 or absent → `None` (**Q10**, O4). |
| `278` | Carried through as the dedup key; absent is allowed. |
| `326` | Mapped to `LifecycleStatus`; a code the spec does not define decodes as `Unknown`, keeps its number in `status_code`, and is reported as a decode error. |
| `324` | The request-id echo on `f` is `SecurityStatusReqID(324)`, not `262`. The sandbox never sends it (O3/O6). |

**Errors are reported, never thrown**, because decoding runs on a QuickFIX
callback thread. A `DecodeError` carries the reason, `35`, `262` and the raw
message with SOH as `|` and the API key redacted. What is dropped depends on
what the message would have done to the client's state:
- A bad level in a `W` drops the **whole snapshot**: a snapshot replaces the
  book, so a partial one would be wrong.
- A bad entry in an `X` drops **only that entry**; the rest of the message is
  delivered.
- A malformed `60`, or a `W` without `55`, drops the message.
- `Y` and `j` are always delivered, since a reject is itself the error path.

`md_client` prints one `ev ` line per event beside the raw log (`--no-events`
turns it off). Rejects on an in-flight `list_instruments` still end that wait,
and discovery's own messages are consumed rather than published: it takes
symbols from the volume/OI `X` updates as well as `W`, because the live
gateway sends no `W` for that channel (**O8**), and its wait settles when no
new instrument has appeared for `quiet` rather than when traffic stops.

**Verified against the live sandbox on 2026-10-26**, zero decode errors across
both captures:

| Capture | Channels | Messages | Events |
|---|---|---|---|
| `capture-sandbox-20261026T0502-instruments.log` | volume/OI, all instruments | 64 X | 128 |
| `capture-sandbox-20261026T0533.log` | book L2 + L1, trades, lifecycle | 2 W, 1480 X | 1482 |

Replaying the second capture through the book rules of §8 also confirmed **Q7**
and **Q8** and found no violation of any invariant (**O9**).

**Not handled here.** No book is kept, so nothing checks that an `X` delete
names a price that exists, that depth stays within 10 levels, or that the book
is uncrossed: that is Stage 4. Gap, stale and disconnect recovery is Stage 5.

---

## 14. Order book and instrument state: Stage 4

`market_data/include/cpm/md/book.hpp` and `market_state.hpp`, both plain C++
with no QuickFIX: `OrderBook` is one instrument's book for one subscription,
and `MarketState` holds every instrument's book, last trade, volume, open
interest and lifecycle. `MarketState::callbacks()` plugs straight into
`MdClient::on_market_data`.

**Applying the feed** (design §8, now confirmed by **O9**):
- A `W` snapshot replaces both sides. Levels are rebuilt through one code path
  that keeps each side best-first, so a feed that ever sent them out of order
  would still produce a sorted book.
- An `X` change sets the **absolute** size at a price. `279=0` and `279=1`
  both set; `279=2`, or a set with size 0, removes the level.
- The book is keyed by price. `1023` is checked against the price's position
  after the change rather than used as state.

**Both sides are bids**, so the accessors are `best(BookSide)`,
`implied_yes_ask()` = `1.000 − best NO bid`, `implied_no_ask()` =
`1.000 − best YES bid`, and `crossed_or_locked()` = the two bids summing to
`1.000` or more (**Q9**).

**One subscription owns a symbol's book.** The gateway sends every update once
per `MDReqID` (**O7**), so a client holding an L2 and an L1 subscription on one
instrument receives each change twice — and the L1 copy's "delete the old
best" would remove a level the L2 book still holds. The first snapshot binds
the symbol to its subscription; later snapshots and updates from any other are
ignored and counted (`ignored_foreign_snapshots()`, `ignored_foreign_updates()`).
`invalidate_books()` unbinds, so a Stage 5 resubscribe with a fresh MDReqID
takes ownership.

**Trades are deduplicated on `MDEntryID(278)`**, over a 1024-entry window per
instrument, because the same execution arrives under both a per-instrument and
an all-instrument subscription. A trade with no `278` is accepted and counted
separately, since it cannot be deduplicated.

**Lifecycle** follows Q13 and Q14: `Unknown` until the first `f`; `Amended(26)`
counts as an event and leaves the trading state alone; a `326` the spec does
not define keeps the last known state; every transition is recorded in a
bounded history, and none is rejected. `f` carries no `MDEntryID`, and
overlapping subscriptions deliver each transition once per subscription
(**O10**), so the same `326` at the same `TransactTime` is treated as the same
event and counted in `duplicate_status_events` rather than recorded twice.

**An update with no book yet is not an anomaly.** `MarketState` counts it
(`updates_awaiting_snapshot()`) and drops it. This is the normal case while a
subscription is being established, and on every Stage 5 recovery: the
gateway's in-flight updates arrive after `invalidate_books()` and before the
fresh snapshot, so flagging them would alarm on every rebuild. `OrderBook`
still reports it if an update is applied to an invalid book directly.

**Other anomalies are reported, not thrown.** `BookAnomaly` covers a new at an
existing price, a change at a missing price, a delete of a missing price, a
`1023` mismatch, depth over 10, a price outside 0.001–0.999 (dropped), and a
crossed or locked book. `MarketState`
dispatches them after releasing its lock, so a handler can read the state back.
`md_client` logs them as warnings and prints a per-instrument summary on exit.

**Tests.** 16 book unit tests, 15 state unit tests, 5 against `md_sim`, and —
replacing the throwaway script that answered Q7 — `capture_replay_test.cpp`
replays all six sandbox captures through the decoder and the book:

| Capture | Asserted |
|---|---|
| all six | zero decode errors, zero anomalies |
| `20261026T0533` | 2 snapshots, 1050 book updates, 430 trades; book bound to the L2 subscription; >600 `1023` checks with no mismatch; never crossed; >100 duplicate trades dropped |
| `20261026T0502` | 64 volume and 64 open-interest events, no snapshot at all (**O8**), status `Unknown` |
| `20261023T1117` | 225 lifecycle events, 8 rejects, >20 instruments through the full `21 → 17 → 18 → 27 → 28` sequence |
| `20261023T1018` | the two **O4** trades, ending `Settled` |
| `20261026T0607` | the **O10** roll: the book ends exactly empty after 1325 updates and 1159 `1023` checks, `18 → 27 → 28` each deduplicated from two subscriptions, successor `Open` with no duplicates |

**Not handled here.** Nothing acts on an anomaly, a gap, a disconnect or a
stale feed; `invalidate_books()` exists but nothing calls it yet. That is
Stage 5.

---

## 15. Recovery: Stage 5 is fail-stop

**Not recovery.** The original Stage 5 said "mark books invalid, resubscribe,
rebuild from a fresh snapshot". The decision of 2026-10-26 (project
instructions amendment 4, **Q24/Q25/Q27/Q28**) is that the strategy stops on a
gap, a stale feed, any reject or a disconnect, and stopping the strategy stops
the feed. So the client detects these, reports one terminal halt and stops. It
never resubscribes and never reconnects by itself.

`market_data/include/cpm/md/feed_monitor.hpp`: `FeedMonitor` watches one
client's feed and reports the first `FeedHalt`.

| Trigger | Detected by |
|---|---|
| `Gap` | an inbound `SequenceReset` with `GapFillFlag=Y`, plus inbound `MsgSeqNum` tracking as a backstop. A Logon adopts the expected number, since `141=Y` restarts the stream and the Logon reaches the application before `onLogon`. |
| `StaleFeed` | a book subscription silent for longer than `stale_after` (30 s by default, `--stale-secs`) |
| `Reject` | any `Y` or `j`, whatever the code (**Q27**), including on the first subscribe (**Q29**) |
| `Disconnect` | `onLogout` from a drop or a server Logout (**Q28**) |

**Staleness is per subscription, and only for books.** O10 showed a session
staying healthy for 20 minutes while one subscription was silent for 12 of
them, so a session-level check would never fire. Trades can be legitimately
quiet for minutes on a thin market (O3) and the lifecycle channel only speaks
at window boundaries (O6), so neither is timed. A book is also exempt once
every instrument it covers is Closed, Determined, Settled or Delisted: those
are silent by design (O10), and without the exemption the strategy would halt
at every 15-minute roll. An instrument whose status is unknown counts as live,
because halting is the safe direction.

**Who stops what.** The monitor reports; the owner acts. `md_client`'s handler
logs the halt, invalidates every book and sets a flag; the main loop then
stops the client and exits 4. Two threading rules came out of getting this
wrong:
- `MdClient::stop()` is idempotent and safe from any thread, because a halt
  handler and the owner's shutdown path can both call it. Without that, one
  thread destroyed the initiator while the other was inside its `stop()`,
  pulling the QuickFIX session out from under its own socket thread.
- A halt is delivered on the monitor's thread while it runs, inline when there
  is no thread (unit tests), and **not at all** once `stop()` has been called.
  Otherwise the logout caused by the owner's own shutdown comes back as a
  `Disconnect` on the session's thread and re-enters the handler there, which
  deadlocks against the shutdown in progress.

**Tests.** 14 unit tests drive the monitor on a fake clock, so nothing sleeps:
each trigger, the exemptions, one halt only, and the clock reset on re-logon.
Six integration tests run against `md_sim`: `--drop-seq` halts with `Gap`,
`--disconnect-after` with `Disconnect`, `--stale-after` with `StaleFeed`, an
unknown symbol with `Reject`, while a window roll and a healthy feed keep
running. All of them are in the repeat runs, since both threading bugs above
only appeared under repetition.

**Instruments are evicted.** `MarketState::evict_finished` drops Settled and
Delisted instruments that have been quiet for a grace period, and `md_client`
calls it once a minute. Without it a long run accumulated every instrument it
ever saw — about a hundred rolling windows a day (O2/O6), each holding a book,
a lifecycle history and up to 1024 trade ids. Closed and Determined are kept,
because they still settle later.

**Not handled.** The successor of a rolled window is not followed (**Q26**):
recovery repairs what you asked for, and deciding what to follow is strategy
policy.

---

## 11. Open questions

> **Answered 2026-10-23.** The current status and spec references for each question are in `docs/open-questions.txt`, which supersedes this list. In summary: Q18 is approved, Q19 means no git or GitHub at all for now, with unneeded files moved to `Temp/`, spec-vs-instruction conflicts follow the spec, and every other proposal below is adopted.

### Blocking Stage 1
- **Q18 (dependencies):** how should the dependencies be installed? Proposal: `sudo apt install cmake git libgtest-dev`, and build QuickFIX from source through CMake FetchContent at a pinned release tag, with `HAVE_SSL=ON`. Sandbox port 6120 is TLS, and the SSL support of the apt `libquickfix-dev` 1.15.1 build is unknown. The alternative is apt QuickFIX plus stunnel.
- **Q19 (git):** there is no git repository, and git is not installed. After git is installed, the plan is to run `git init`, ignore `creds.env`, binaries and `build/`, and commit Stage 0. Also: the prototype `fix_md_login.cpp` and its binary at the repo root are proposed to be removed once Stage 1 replaces them, and to stay out of the Stage 0 commit until then. Is that acceptable?

### Spec vs. your instructions
- **Q1:** your instructions say the dictionaries should include "all custom tags and message types". In the spec, all 20000+ tags, `UMS` and F1–F5 are **Order Entry only** (§5 above). Following the spec, they are left out of the MD dictionaries. Should they be left out (recommended), or added anyway?
- **Q11:** your instructions say "sizes as int64". Sizes on the wire have 2 dp (e.g. `30.72`), so they are stored as int64 **hundredths**. Prices are int64 **thousandths**. Is that OK?

### Affecting Stages 3–4 (some can be settled by capturing raw sandbox traffic in Stage 1)
- **Q3:** §4 lists `264=0` (full book) as valid, but §4.3 rejects any 264 other than 1 or 10. Proposal: send only 1 or 10.
- **Q4:** can one V combine several channel groups, e.g. 0, 1, 2, B and C? The examples never mix them, and 264 is "forbidden on non-book requests", which suggests one channel group per request. Proposal: one request per channel group.
- **Q5:** what must an unsubscribe (263=2) contain? MDReqID alone, or the same 267 and 146 groups as the subscribe? Proposal: repeat the original groups.
- **Q6:** where do 55 and 48 sit in X? Per entry, as the table says, or at message level, as the trade examples show? The volume/OI X example also has 279 *before* 268, and says `268=1` while carrying two entries. Proposal: accept both placements, and confirm against captured sandbox X messages.
- **Q7:** the L2 delta example updates price 0.580 with `1023=2`, but the snapshot had 0.580 at level 1. Proposal: key the book by price and treat 1023 as informational. Is that OK?
- **Q8:** with an L1 subscription (264=1), when the best price *moves*, what is sent: a Delete of the old level plus a New one, or a single Change carrying the new price? Proposal: the L1 book holds one level per side and any update replaces it.
- **Q9:** crossed-book rule. Proposal: flag crossed when `yes_bid + no_bid > 1.000`, and locked when the sum `= 1.000`. Both mark the book suspect. Is that the definition you want?
- **Q10:** AggressorSide to taker outcome. Proposal: 1 (Buy) → YES taker, 2 (Sell) → NO taker, 0 → none. This matches the WebSocket `taker_side` field.
- **Q13:** lifecycle has no snapshot. The WebSocket docs say to fetch current state over REST first. Is REST in scope? Proposal: no. State stays `Unknown` until the first `f` arrives.
- **Q14:** is `Amended (26)` a *state*, or an *event*, such as a close-date change, that leaves the trading state unchanged? Are there transitions we should flag as unexpected? Proposal: record every transition and treat Amended as an event without changing state.

### Informational (no answer needed unless you disagree)
- Q2: the signing recipe is borrowed from Order Entry. It was verified on the sandbox.
- Q12: on a sequence gap we don't wait for ResendRequest(2). We reconnect with 141=Y and resubscribe.
- Q15: OI and volume may be unpopulated in the sandbox **[WS]**.
- Q20: the spec gives no limits on symbols per request, subscriptions per session, or message rates.
- Q21: subscribing by SecurityID(48) alone would break QuickFIX's group delimiter, so the client always sends Symbol(55).
- Q22: the spec doesn't say whether subscriptions survive a reconnect. The client assumes they don't, and always resubscribes.
