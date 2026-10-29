# CPM GUI: a browser control panel over one trading daemon

`gui/` (the HTTP surface and the page) and `strategies/host/` (the strategy
host the GUI drives). Written 2026-10-28, Stage G0.

Scope: start, stop, arm and watch strategies from a browser. It adds no
trading logic of its own — every decision still belongs to the strategy, every
order still goes out through `order_management/`, and the GUI can do nothing a
careful operator could not already do with CLI flags.

## 0. The shape

The architecture the operator drew, and what each box is here:

```
                    +--------------------------------------+
                    |            Control GUI               |
                    |   start, stop, parameters, status    |
                    +--------------------------------------+
                        commands |          ^ status
                    POST /api/...|          | GET /api/snapshot, /api/events
                                 v          |
   +--------------------------------------------------------------------+
   |  Strategy engine                 StrategyHost, one 250 ms tick      |
   |   +----------------+  +----------------+  +----------------------+  |
   |   |  Strategy A    |  |  Strategy B    |  |  Strategy C          |  |
   |   |  market making |  |  arbitrage     |  |  execution algo      |  |
   |   |  (CryptoMM)    |  |  (a kind +     |  |  (a kind + schema)   |  |
   |   +----------------+  |   schema)      |  +----------------------+  |
   +--------------------------------------------------------------------+
         ^ market data            orders |        ^ fills, acks
         |                               v        |
   +----------------------+        +-----------------------------+
   |  FIX market data     |        |  FIX order management       |
   |  quotes, trades, book|        |  NewOrder, ExecReport       |
   +----------------------+        +-----------------------------+
         ^ FIX session                   ^ FIX session
         |                               |
   +----------------------+        +-----------------------------+
   |  Market data feed    |        |  Broker or exchange         |
   +----------------------+        +-----------------------------+
```

| Box | Here |
|---|---|
| Control GUI | `gui/` - the embedded page, the HTTP/SSE server, the token |
| commands | `POST /api/strategies`, `/{id}/{start,stop,arm,disarm,destroy}`, `/api/stop-all` -> `ControlApi` |
| status | `GET /api/snapshot` and the SSE stream `GET /api/events` -> `ControlApi::Snapshot` |
| Strategy engine | `strategies/host/` - `IStrategy`, `StrategyHost`, the parameter schemas |
| Strategy A, B, C | a registered kind and its schema. `CryptoMM` is the only one built; a second needs no change to the page, because the form comes from the schema |
| FIX market data | `market_data/` - `MdClient` -> `MarketState` -> `FeedMonitor` |
| FIX order management | `order_management/` - `OeClient` -> `OrderState` -> `Reconciler` |
| Market data feed, broker | the CPM gateway; `md_sim` and `oe_sim` locally |

Two places where the picture and the build differ, both deliberate. **The GUI
and the engine are one process**: the two boxes are a logical split, and the
command and status arrows are function calls behind a loopback HTTP shim, for
the reason section 2 gives. And **the GUI reads market data directly as well**
- the snapshot carries top of book, session state, recovery state and the
limits in force, not only what the engine says about itself, because an
operator diagnosing a strategy that is quoting nothing needs to see the book it
is quoting nothing against.

P&L is in the drawn GUI box and is deliberately not built yet: `PositionView`
carries `net`, `cash`, `fees` and `settlement_payout`, from which realized P&L
is derivable, but nothing derives it and unrealized needs a mark. See
`docs/open-questions-gui.txt` GUI-Q15.

## 1. Why a browser and not a GUI toolkit

The app runs on an AWS Linux instance reached from Windows over PowerShell or
cmd. That rules out most of the field before taste enters into it.

| | |
|---|---|
| **Browser + localhost HTTP** | **Chosen.** No new C++ dependency: a small HTTP/SSE server on POSIX sockets, a token from OpenSSL (already linked for signing), hand-written JSON. Works over an SSH forward, which is the only way the operator can see anything at all. |
| Qt 6 Widgets | Hundreds of MB of new dependency against a standing "QuickFIX and GoogleTest, no other dependencies" rule, needs X11 forwarding to be visible from Windows, and GUI code resists unit testing. |
| Dear ImGui + GLFW | Still new dependencies plus OpenGL on a headless instance. Good for a book heatmap, poor for parameter forms. |
| Terminal UI | Smallest of all, but the operator asked for a GUI, and parameter entry in ncurses is worse than an HTML form by every measure. |

The cost of the choice is roughly 400–600 lines of HTTP/SSE plumbing we own.
The benefit is that `cpm_trader` stays a single static-ish binary with the same
dependency list it has today.

## 2. The hard constraint: one FIX session per API key

`cpm::oms::SessionEvent::Kind::Replaced` already records what the gateway does
when a second Logon arrives for the same API key: it **displaces** the first
session with `Logout(5)` "replaced by newer connection", and because the API
key is still live, **cancel-on-disconnect does not fire** — the displaced
session's resting orders stay in the market with nobody managing them.

So "start a strategy" must not mean "start a process". Two `crypto_mm`
processes under one API key is not merely untidy, it is the specific failure
the OMS design warns about: the second one's logon orphans the first one's
quotes. The same reasoning applies to `market_data/`, and separately to state
that cannot be split — one `OrderState`, one position per symbol, and the one
durable bookmark file of OE-Q7, which two processes would race.

**Therefore: one daemon, many strategies.** This is a constraint, not a
preference, and it is the main reason this stage touches `strategies/` at all.

## 3. `cpm_trader`

One process owning exactly one of everything the venue counts:

```
cpm_trader
├── MdClient ──── MarketState ──── FeedMonitor      (one MD session)
├── OeClient ──── OrderState  ──── Reconciler       (one OE session, one bookmark)
├── StrategyHost        N strategy instances, one 250 ms loop over all of them
└── gui::Server         127.0.0.1 only, JSON + SSE + one embedded page
```

Credentials are read from the environment by the daemon at startup, exactly as
`crypto_mm` reads them now. **The GUI never sees, carries or sets a
credential**, and there is no API that would let it — the browser cannot reach
the venue even in principle.

Two things the daemon fixes that `crypto_mm` left undone: it runs a
`FeedMonitor` (which `crypto_mm/main.cpp` never wired up, so a stale or gapped
feed does not currently halt it), and it calls `MarketState::evict_finished()`
on a timer, which a long-lived process needs and a 60-second run did not.

## 4. `IStrategy`, and why the GUI does not know what CryptoMM is

A strategy is whatever satisfies:

```cpp
class IStrategy {
public:
    virtual ~IStrategy() = default;
    virtual void on_market(const md::InstrumentView&, const oms::Position&,
                           oms::Clock::time_point) = 0;
    virtual void on_report(const oms::ExecutionReport&) = 0;
    virtual void withdraw(const std::string& why) = 0;
    virtual bool quiescent() const = 0;           // nothing live at the venue
    virtual std::vector<std::string> symbols() const = 0;
    virtual StrategyStatus status() const = 0;
};
```

`CryptoMM` becomes a thin implementation over the existing `QuoteManager`,
which already has `on_market`, `on_report`, `withdraw`, `live_bid`/`live_ask`
and the counters. `desired_quotes` is untouched: it is a pure function and
stays one.

Parameters cross the wire as `map<string,string>`, and each registered kind
publishes a **schema** — name, type, default, bounds, one line of help — which
the daemon serves and the page renders into a form. Adding a second strategy
therefore means registering a kind and its schema; it does not mean editing
JavaScript. The same schema does the validating, so the GUI and the CLI cannot
disagree about what `--width-ticks 0` means.

Every row is the same row, under one header of fixed columns (GUI-Q25,
GUI-Q26, GUI-Q29):

```
Buttons | Kind | Symbol | Spread | Qty | Max Inventory | Requote Threshold |
Withdraw Before | Ticks
```

The three buttons are one cell of **Buttons**, not three columns. The section
is a single CSS grid and the row wrappers are `display: contents`, so every
cell is an item of that one grid - which is what makes a column a column
across rows rather than a width each row approximates. The columns themselves
come from the schema: the union of every registered kind's parameters, each
name once, so a second kind shares the columns it has in common and leaves
the others empty. What each field means is its tooltip rather than a line under it,
which is what makes one line possible, and a field's label is title-cased from
its name unless the schema gives one: `width-ticks` shows as **Spread** and is
still `--width-ticks` everywhere else. The buttons are not new verbs:
Start Feed is `create`, which subscribes the book and leaves the strategy
stopped and disarmed, Start is `start` (and creates first if there is nothing
to start yet) **and then arms it**, and Stop is `stop`, which leaves the feed
up because only `destroy` unsubscribes and nothing on the page calls it.

**Start is what arms** (GUI-Q26). It keeps the confirmation - the dialog
names the strategy, the symbol, the parameters, the per-order limits and the
recovery state, and nothing is sent until it is confirmed - but arming is no
longer a separate key the operator turns, which is half of GUI-Q8 reversed on
the operator's instruction. `--allow-orders` is untouched: without it Start
is a dry run and does not ask, because nothing can reach the venue.

**There is no table above it, and no row appears by itself.** Every live
strategy is a row of exactly this shape - its three buttons, its parameters
shown and disabled, and one line under it carrying state, armed, id, series
and window, live orders, placed/cancelled/fills and why it is not quoting.
`Add row` under them adds a blank one; a blank row carries a `×` that throws
*it* away, and it is the only thing that `×` can throw away. When the symbol
typed into a blank row already has a strategy its buttons refuse and point at
the row above: one strategy, one set of controls.

A live row's parameters are disabled because they are fixed for the life of
an instance - `StrategyHost` replaces the instance rather than re-pointing it
- and its symbol is read off the live subscription rather than the spec, so a
strategy following a series shows the window it is on now.

A live row's Symbol box shows the **series** - `BTCD15M`, the name the
operator typed - and the window it is on now is on the line underneath, where
it can change every quarter hour without the row moving. The snapshot carries
`series` for both cases: the target when it follows one, `parse_ticker`'s
series otherwise.

The blank row's fields are built once per kind and left alone. A snapshot
arrives twice a second, and a form rebuilt on each one moves the caret out
from under whoever is typing - only the buttons and the live rows are redrawn
on a snapshot.

## 5. The control API, and the two keys on the trigger

`ControlApi` is plain C++ with no sockets in it — `kinds()`, `create()`,
`start()`, `stop()`, `arm()`, `disarm()`, `destroy()`, `stop_all()`,
`snapshot()`. The tests drive it directly; the HTTP layer only translates.
Every refusal is a returned value rather than an exception, because each one
has to cross an HTTP boundary intact.

`create()` also opens the symbol's market data subscription, and unwinds both
if that fails: a strategy on a symbol whose book never arrives is
indistinguishable from the wrong symbol, which is the diagnosis `crypto_mm`
had to print on its way out.

Arming takes **two independent keys**:

1. `cpm_trader --allow-orders` at launch. Without it no strategy can be armed
   at all, and `OrderLimits::allow_orders` stays false for the whole run.
2. Per strategy, `arm` from the GUI, default **disarmed**. A disarmed strategy
   computes and logs its quotes and sends nothing — the dry run `crypto_mm`
   already has, now per strategy and switchable while running.

`OeClient`'s limits carry the daemon-wide answer, so disarming is enforced
below the strategy rather than by it. Cancels are never gated, because the
existing client deliberately exempts them: stopping must always work.

The reconciler's gate is a third, *dynamic* condition and is deliberately not
one of the keys: arming is an intent, and reconciliation is a reason to hold
an order back, not a reason to refuse the intent (GUI-Q11). So `arm` consults
only the static `--allow-orders` key, while `can_submit` folds in all of
permitted, armed, reconciled and not-halted. Stopping a strategy disarms it,
so a restart needs arming again (GUI-Q13).

## 6. Halts stay terminal

A `FeedHalt` from `FeedMonitor` or a `Halt` from `Reconciler` is daemon-wide,
never per strategy. A gapped book or a lost execution report invalidates the
state every strategy is reasoning from, so:

1. every strategy is withdrawn,
2. the submission gate closes and `create`/`start`/`arm` are refused,
3. the reconciler flattens and reconciles as OE-Q6 already prescribes,
4. both sessions stop.

Nothing resumes. There is no "clear halt" button, because fail-stop
(instructions amendment 4) means stop.

**One deliberate difference from `crypto_mm`:** the process does not exit. It
stays up in a terminal `Halted` state, serving the page read-only so the
operator can see the reason, the last book, the positions and which strategy
was holding what. A run that halts at 02:00 and leaves nothing but an exit code
on a closed PowerShell window is the one outcome worth engineering against —
this is the same instinct as "it says what it sees" in the CryptoMM design,
applied to the process boundary. See `docs/open-questions-gui.txt` GUI-Q4.

## 7. The HTTP surface

Bound to `127.0.0.1` only, never `0.0.0.0`. Every request must carry the
run's token, as `Authorization: Bearer <t>` or `?t=<t>` on the page load.

| | |
|---|---|
| `GET /` | the page: one HTML file, embedded in the binary at build time |
| `GET /api/snapshot` | everything: daemon state, halt, sessions, strategies, books, positions |
| `GET /api/events` | SSE; a `snapshot` event first, then another whole snapshot whenever the state changes |
| `GET /api/schema` | the registered kinds and their parameter schemas |
| `GET /api/ladder?symbol=` | one instrument's whole price axis; static, so asked for once per symbol |
| `POST /api/strategies` | create from `{kind, params}` |
| `POST /api/strategies/{id}/{start,stop,arm,disarm}` | |
| `POST /api/strategies/{id}/params` | retune one that is not armed, from `{params}` (GUI-Q30) |
| `POST /api/stop-all` | the kill switch: withdraw and disarm everything |

A refusal from `ControlApi` is **409** with its reason as `{"ok":false,"error":...}`,
because the request was well formed and the daemon's state is what said no;
**400** is reserved for a body that is not what it claims. A missing or wrong
token is 401, a `Host` that is not loopback is 403, and an unknown path or
action is 404. `EventSource` cannot set a header, so the stream and the first
page load carry the token as `?t=`; everything else may use
`Authorization: Bearer`. The page strips the token from the address bar on
load.

A snapshot first, because a browser arriving mid-run is the normal case here,
not the exception — the operator will close the laptop and reconnect. The page
reconnects the stream on its own and re-snapshots when it does.

What follows is **another whole snapshot**, not a delta, sent whenever the
daemon's state differs from what that connection was last told, and at most
one per `refresh` (500 ms). A snapshot of a daemon running five strategies is
a few kilobytes over loopback, so a delta protocol would save nothing
measurable and would cost the property that matters at 02:00: with no merging
in the page, there is no way for what the browser shows and what the daemon
holds to drift apart (GUI-Q14).

The comparison is on the rendered state, not on the revision. The revision
counts control-API calls, and a stream woken by it alone freezes on the last
button the operator pressed — while the book moves, quotes are re-placed and
fills arrive, none of which passes through `ControlApi` at all. That was a
real bug, shipped in G3 and only found when a ladder sat still in front of a
market that was not (GUI-Q20). A `heartbeat` event every 15 s covers having
nothing to say, so a dead tunnel never looks like a quiet market.

JSON is written by hand (`gui/src/json.cpp`) and tested on its own: scaled
integers render through the existing `format_price`/`format_qty`/`format_amount`,
so prices never become doubles on the way to the browser either.

## 8. Getting to it from Windows

A `localhost` URL is only answered by the machine the browser runs on, so the
link the daemon prints works from Windows **only through an SSH forward**. The
daemon therefore prints the link and the command that makes it work:

```
cpm_trader listening on 127.0.0.1:8080
  open:  http://localhost:8080/?t=f3c9a1b2e8d47506
  no tunnel? reconnect with:  ssh -L 8080:localhost:8080 ubuntu@<this-host>
orders: DISARMED (start with --allow-orders to permit arming)
```

The durable form, set once on the Windows side in
`%USERPROFILE%\.ssh\config`, so nobody has to remember the flag:

```
Host cpm
  HostName <ec2-host>
  User ubuntu
  LocalForward 8080 localhost:8080
```

Windows 10 and 11 ship OpenSSH, so this needs nothing installed. Where the
instance takes no inbound SSH, `aws ssm start-session --target <id>
--document-name AWS-StartPortForwardingSession --parameters
portNumber=8080,localPortNumber=8080` gives the same tunnel. For a forward
forgotten on an already-open session, OpenSSH's `~C` escape then
`-L 8080:localhost:8080` adds one without reconnecting.

There is no auto-open: `xdg-open` means nothing on a headless instance. An
`--open` flag covers running it on a desktop box.

## 9. Surviving the terminal

Closing PowerShell sends `SIGHUP`, which is default-terminate — `crypto_mm`
blocks `SIGINT` and `SIGTERM` only, so today an SSH drop would kill it without
withdrawing. `CancelOnDisconnect::ApiKey` is the real backstop and would clear
the orders, but the daemon still treats `SIGHUP` as `SIGTERM`: withdraw,
reconcile, log out, exit.

Run it detached (`systemd-run --user`, `screen`, `tmux`) so a dropped
connection does not stop a live strategy, and reconnect the browser to the
daemon that is still running. That is the case section 7's snapshot-first
stream exists for.

## 10. Stages

| Stage | Deliverable |
|---|---|
| **G0** | This document and `docs/open-questions-gui.txt`. No code. |
| **G1** | Done. `IStrategy`, `StrategyStatus`, the parameter schema and `StrategyHost` in `strategies/host/`. `CryptoMM` reimplemented on it; `crypto_mm` keeps its flags and behaviour, its 25 tests unchanged as the regression. |
| **G2** | Done. `cpm_trader` in `trader/`: one session pair, `FeedMonitor` wired, registry, `ControlApi`, halt propagation, `SIGHUP`, `evict_finished` on a timer. Driven by GoogleTest against `md_sim`/`oe_sim`, and by an operator console on stdin. No sockets. |
| **G3** | Done. `gui/`: the HTTP/SSE server, the token, the loopback bind, the `Host` guard, hand-written JSON both ways, the startup banner. 42 tests, plus an end-to-end `curl` run against both simulators. |
| **G4** | Done. The page: strategy table with live orders, schema-driven start form, top of book, positions, halt banner, Stop All. One file, no framework, no build step. 7 more tests, and rendered in headless Chrome against both simulators, live and halted. |
| **G5** | Done. Confirm-on-arm, a stream that admits when it has given up, a kill switch nothing can disable, idle-connection reaping, and 10 more tests - 5 of them the whole stack over HTTP against both simulators. Driven in a real browser over the DevTools protocol. |

## 11. The page

One document, `gui/web/index.html`, embedded at build time. It renders a
snapshot and posts commands; it computes nothing. Every price and quantity
arrives already formatted from scaled integers, and every refusal shown is the
daemon's own words, so there is no second opinion on this page about what a
number means.

What it shows: the header pills (daemon state, both sessions, recovery,
whether orders are permitted at all, and the stream's own health), the halt
banner, one row per strategy carrying its controls and its parameters,
`Add row` to make another from `/api/schema`, top of book, a ladder per
symbol, positions, the session limits in force, and - on the Strategies page
only, pinned to the bottom of the window - the tail of the daemon's log
(GUI-Q27, GUI-Q28). Every line carries a `LogSource`: `Daemon` for the events
a person needs, `Strategy` for a market maker's commentary on its own quotes,
`Wire` for the raw FIX log and the one-line summary of each order and cancel.
The panel takes `Daemon`, plus anything at Warn or Error whatever its source,
so a strategy that *cannot* quote still reaches it. The log file gets all of
it either way.

### Changing a strategy's parameters

A parameter is read once, when the instance is constructed, so the only way
one can change is to build the instance again - which is what `roll()` already
does at every window boundary. `POST /api/strategies/{id}/params` does the
same thing for a different reason: same id, same symbols, same arming, the
counters carried over, a new instance built from the new values.

**Armed is the line (GUI-Q30).** A strategy is armed because the operator was
shown what it would quote and said yes; a width that changed underneath would
put a different order in the market than the one they confirmed. So the host
refuses an edit while a strategy is armed, and takes it in every state short
of that: created and quoting nothing, or Running and disarmed, which is the
state a strategy is watched in before it is trusted. Disarm, retune, arm again
is the loop, and it costs nothing at the venue because a disarmed strategy is
not quoting.

Two things it is not. It is **not a move**: the symbol may be sent, because
the form shows it, but it may not differ from the one the strategy already has
- changing an instrument means a subscription, a REST resolution and the
one-strategy-per-symbol claim, which is create's job and roll's. And it is
**not a free pass over a resting order**: a disarm leaves what was already
resting to be taken off deliberately, so if the old instance still has
anything at the venue, the edit cancels it before the instance that placed it
goes away. That cancel is a real cancel, not an abandon - the market is open
here, and nothing else is going to take those orders off (OE-Q6 is the
opposite case).

On the page, a live row's boxes are the strategy's own values, editable while
it is not armed and visibly locked once it is, with `Apply` and `Revert` next
to `Stop`. `Apply` sends only what changed. What is typed survives the twice-a-
second redraw, and a row that locks while it is being typed into is redrawn
anyway, with the uncommitted text dropped: a box that can no longer be applied
must not sit there looking as though it could.

Nothing in the Strategies section moves by itself. A row carries state, armed,
id, series, window and why it is not quoting - things that change when the
operator acts, when it rolls or when it stops - and not the live orders or the
placed/cancelled/fills counters, which belong on Ladders, Books and Positions.
The rows are rebuilt only when that text differs rather than on every
snapshot, so twice a second the page does nothing to them at all. `Stop All` is in the header,
outside the scroll, because the kill switch must be reachable without finding
anything first.

### Ladders

One ladder per subscribed symbol: **the instrument's whole price axis, 0 to
100**, with whatever rests on each rung and our own orders coloured in. A
price here is a percentage chance, which is how a prediction market is read:
59.9, not 0.599. No arithmetic is involved - a price is already thousandths of
one, so it is the same integer with one implied decimal instead of three, and
the snapshot says `"price_unit":"percent"` so nothing has to guess. The
daemon's own logs and the stdin console still speak in the wire's units, which
is what the FIX messages carry. Asks above, bids below, the spread showing as the band of empty rungs it
actually is. The book itself is as deep as the subscription asked for
(`--book-depth`, 1 or 10).

The axis is the instrument's tick grid, which lives in `cpm/md/ticks.hpp`
because it is a property of the instrument and not of any strategy - it
decides what an order may be priced at and equally what rungs a ladder has.
It came from CryptoMM, where only one caller could see it.

The default, `tapered-cent`, is what the operator reports the venue doing: **a
percentage point at a time, with a tenth of a point inside the first and last
point** - 0, 0.1, 0.2 … 0.9, 1, 2 … 98, 99, 99.1 … 99.9, 100. That is 119
rungs. It is the same shape as the spec's `TAPERED_DECI_CENT` but tapering a
tenth as far in from each end, so it is not that structure and not any of the
three the `PriceLevelStructure` table lists (GUI-Q23). The three that are
remain available: `tapered` (281 rungs), `cent` (101) and `deci` (1001).
`--tick-structure` picks one for the daemon, and a strategy takes the same
spelling as a parameter, because it is not on the FIX feed (O5).

0 and 100 are rungs but not prices: "order prices exclude 0 and 1; settlement
values may equal either" (O5). The ladder shows them greyed and says so.

Membership and stepping are not the same question, and they differ at the
boundaries: `tick_at(0.900)` is the coarse tick, because that is where a
strategy stepping up from 0.900 lands - but 0.901 is still a price an order
can rest at, so the ladder has a rung for it.

That distinction turned out to matter for trading, not only for drawing.
CryptoMM stepped one tick off the touch to avoid crossing it, and a tick
taken off a price lands between rungs whenever the touch is not on our grid -
or when the step crosses the taper. One below 99% is 98%, not 98.9%, and an
order at 98.9% would be rejected, which halts the daemon (OE-Q6). It now asks
the grid for the next price below instead (`price_below`/`price_above`), and
rounds onto the grid with `snap_down`/`snap_up` rather than by remainder.

The axis is served by `GET /api/ladder?symbol=X` and asked for once per
symbol, not carried in every snapshot: 281 rungs twice a second is a lot of
bytes to spend saying the same thing. The page builds each ladder once and
updates the cells in place, because rebuilding hundreds of rows twice a second
would throw away wherever the operator had scrolled to. It centres itself on
the market the first time it can - a hidden page has no height to centre in -
and then leaves the scroll alone, since an operator who has scrolled somewhere
is looking at it. There is a `centre` button for going back.

Four more decisions make it readable:

- **One price axis, in YES terms.** This venue holds two stacks of bids and no
  asks at all (Q9), so the daemon turns each NO bid into the YES ask it
  implies before it sends it. A ladder with two price scales is not a ladder.
  Each ask rung still carries `venue_price`, the NO bid the venue is really
  holding, so a screen here can be compared with a screen there.
- **Every price travels twice**: as the text the page prints, and as the exact
  tick count it sorts and matches on. The page never parses a decimal, so
  no price on it has been through a double - the same rule the rest of the
  system keeps from the wire inwards (design §8).
- **Anything resting at a price the grid does not contain still gets a rung**,
  marked in amber. Not seeing your own order is the one thing a ladder must
  never do, and a book that disagrees with the configured grid is worth seeing
  rather than hiding - against `md_sim`, whose book is deci-cent, running with
  `--tick-structure tapered` lights up exactly that mismatch. Those rungs are
  kept for as long as the ladder lives, because one that came and went would
  rebuild the table under whoever was reading it.
- **Two orders on one rung are listed, not added** ("2+1"). Adding two
  formatted quantities in JavaScript is arithmetic on a price-like number,
  which this page does not do.

Four rules it keeps:

1. **Nothing the daemon says is written as markup.** Symbols, halt reasons and
   refusals come off a FIX session; they go on the page with `textContent`.
   The server's `Content-Security-Policy` is `default-src 'none'`, so the
   document also cannot load anything from anywhere, and a test asserts the
   file has no external reference to load.
2. **The form is built from the schema, never from a list of parameters in
   JavaScript.** A kind registered in C++ after this file was written renders
   its own form, with its own bounds and help text, and the daemon validates
   against the same schema whatever the form sends.
3. **It never polls.** The schema is read once; everything else arrives on the
   stream, so there is no second path by which what the browser shows could
   disagree with what the daemon holds. A missed event cannot leave a stale
   leg on the screen, because nothing is merged.
4. **It does not imply a way out of a halt.** The banner says every strategy
   was withdrawn, both sessions were stopped and nothing resumes in this
   process. There is no clear-halt control, and a test asserts the words are
   not on the page. With the session down the books are tagged "feed down:
   last known" rather than being drawn as if they were the market.

Arming is the one control that can put an order in the market, so a disarmed
strategy's `Arm` button says why it cannot be pressed when it cannot - an
observer run names `--allow-orders`, a stopped strategy says to start it
first. The confirmation step on arming is G5.

## 12. Hardening

Five things G5 changed, each because of a way the page could have lied or
locked up.

**Arming asks first, and shows what it is asking.** The dialog names the
strategy, its symbol, every parameter it was created with, the per-order
limits it will trade under and whether recovery has finished - then says, in
so many words, that it will send live orders to the venue. The browser's own
confirm box could show none of that. Cancel holds the focus, so a stray Return
cancels rather than arms, and Escape cancels. Stop All asks the same way, and
lists what it is about to stop.

**The heartbeat is a named event, not an SSE comment.** A comment keeps the
connection warm but is invisible to the page, which then cannot tell a quiet
market from a dead tunnel - the one distinction an operator needs at 02:00. It
carries the revision, so a page that somehow missed an update notices and
reconnects for a fresh snapshot rather than merging anything.

**The page says when it has stopped hearing.** After five failures with
nothing received in between - which is also what a token the daemon will never
accept looks like - it stops claiming to be reconnecting, dims every number on
the screen, marks the daemon state "last known", and offers Reconnect. A
banner over numbers that still read green is not an honest screen.

**The kill switch cannot be taken away.** It is never disabled, it is never
queued behind another request, and it does not wait on a snapshot, a schema or
a working stream. Every request carries a ten-second deadline, so a daemon
that accepts a connection and then stops answering cannot leave the page with
its buttons stuck disabled.

**The server lets go of connections nobody is using.** A keep-alive connection
idle past `--idle-timeout` (60 s, advertised in `Keep-Alive` so the browser
retires its end first) is closed, which is what stops something that opens a
socket and says nothing from spending one of the sixteen slots. Streams are
exempt: sitting there is what a stream is for.

## 13. What this deliberately leaves out

No multi-user access and no login — SSH is the authentication, and the token
only keeps another user on the same instance out. No TLS on the HTTP surface,
because the tunnel already provides it. No charts in G4; a table of numbers is
what an operator reads at 02:00. No strategy editing while running: change a
parameter by stopping and starting, so there is no question of what a quote in
flight was priced against. No persistence of the strategy list across restarts
— the one durable file stays the bookmark of OE-Q7.
