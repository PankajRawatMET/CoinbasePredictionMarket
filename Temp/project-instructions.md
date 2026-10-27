# CPM Market Data - Project Instructions (reference copy)

Saved 2026-10-23. The original instructions are below unchanged, followed by the
standing amendments the user has given since. **Where the two disagree, the
amendments win.**

## Standing amendments (2026-10-23)

1. **No git and no GitHub at all.** No `git init`, no commits, no remotes, no PRs.
   This replaces "commit after each stage" in the original instructions.
   Files that aren't needed are moved to `Temp/` instead.
2. **The spec wins over the instructions.** If the instructions and
   `docs/spec/` disagree, follow the spec and tell the user what was decided.
   Decisions made so far are recorded in `docs/open-questions.txt`.
3. **Build with `-j2`.** This machine has 7 GB of RAM, and an unbounded parallel
   build of QuickFIX hung it.
6. **Order management credentials and its one file (2026-10-26).** Order
   entry credentials live in `creds-oe.env` (chmod 600, never committed) with
   `CPM_OE_*` variable names, so both credential files can be sourced in one
   shell and the order client can never inherit the market data endpoint. The
   order manager may write ONE durable file: the last processed ExecID plus the
   ClOrdID counter, which is what makes `F3` replay possible after a restart
   (`docs/open-questions-oms.txt` OE-Q7). Everything else stays unpersisted,
   including the FIX message store.
5. **Two containers (2026-10-26).** The original "Layout" section below is
   superseded. Shared session plumbing lives in `shared/` (`common/`,
   `fix_common/`), the market data service in `market_data/`, and the order
   entry service in `order_management/`. Include paths are unchanged
   (`cpm/common/...`, `cpm/fix_common/...`, `cpm/md/...`); order management
   will use `cpm/oms/...`. `config/` and `docs/spec/` stay shared, because the
   dictionaries sit side by side and one PDF covers both services.
   `market_data/` must never depend on `order_management/`.
4. **Stage 5 is fail-stop, not recovery (2026-10-26).** The original Stage 5
   said "mark books invalid, resubscribe, rebuild from a fresh snapshot". The
   user will instead stop the strategy on a gap, a stale feed or any reject,
   and stopping the strategy stops the feed. So the client detects these
   conditions, marks the affected books invalid, reports a terminal halt and
   stops the session. It never resubscribes by itself. See
   `docs/open-questions.txt` Q24, Q25, Q27.

## Stage status

| Stage | Status |
|---|---|
| 0. Spec ingest | Done (`docs/design.md`, `docs/open-questions.txt`, `config/`) |
| 1. Build and session | Done. Verified on the sandbox 2026-10-23: TLS, signed logon, heartbeats, clean logout. Then 4 sandbox feed captures (docs/open-questions.txt O1-O7) |
| 2. Simulator (md_sim) | Done 2026-10-23. See docs/design.md §12 |
| 3. Subscribe and decode | Done 2026-10-26. See docs/design.md §13 |
| 4. Order book and instrument state | Done 2026-10-26. See docs/design.md §14 |
| 5. Recovery (fail-stop) | Done 2026-10-26. See docs/design.md §15 |

Order management (`order_management/`, plan in `docs/design-oms.md`):

| Stage | Status |
|---|---|
| 0. Spec ingest and dictionaries | Done 2026-10-26. `config/*-CPMOE.xml`, 14 tests |
| 1. Session | Done 2026-10-26. oe_client logs on; places no orders. 14 tests |
| 3. Submit and decode | Done 2026-10-26. Order path + decoder, 36 tests |
| 2. Simulator (oe_sim) | Done 2026-10-26. Matching engine + acceptor, 29 tests |
| 4. Order state machine | Done 2026-10-26. OrderState + positions, 13 tests |
| 5. Reconciliation | Done 2026-10-26. Durable bookmark + reconciler, 8 tests |
| 6. Settlement | Done 2026-10-26. UMS into positions, payout and fill checks |

Strategies (`strategies/`, one directory per strategy):

| Strategy | Status |
|---|---|
| CryptoMM | Done 2026-10-26. Quotes x ticks either side of mid, y per side. See docs/design-crypto-mm.md |

---

## Original instructions (verbatim)

You are helping me build the MARKET DATA part of a trading system for Coinbase Prediction Markets (CPM) over FIX, in C++20. Scope is market data ONLY: no order entry, no execution reports, no OMS, no risk. I build one stage at a time: complete a stage, run its tests, summarize, list what is NOT handled, then STOP until I say "next".

### Sources of truth
- The CPM FIX market data spec is in docs/spec/. Read it fully before coding. If anything I say conflicts with the spec, follow the spec and tell me.
- If something is missing or ambiguous (endpoints, logon fields, signing, snapshot vs incremental behavior, meaning of custom messages), ASK me. Do not guess.

### Known facts (from the docs)
- Session layer FIXT.1.1, application layer FIX 5.0 SP2, DefaultApplVerID = 9.
- CPM extensions use tags 20000+, U-prefixed message types and F1-F5.
- Channels: trade, orderbook, open interest and trading volume, instrument_lifecycle.

### Constraints
- CMake 3.20+, Linux, -Wall -Wextra -Wpedantic.
- Use QuickFIX/C++ for the session and parsing; GoogleTest for tests. No other dependencies.
- Generate custom transport and application data dictionaries in config/ from the spec, including all custom tags and message types.
- TLS if the spec requires it (QuickFIX built with OpenSSL, or document an stunnel setup).
- Market data does not need message persistence or resend: use a null/memory store and recover by resubscribing to a fresh snapshot.
- Prices as scaled int64, sizes as int64, never doubles.
- Credentials from environment variables only. Never log or commit them. Use the sandbox/test environment ONLY, never production.
- Keep QuickFIX types at the edge (fix_common/ and marketdata/src/fix/). The book and handler use my own plain structs.
- The market data library must expose a small interface (subscribe symbols, register callbacks for Trade, BookUpdate, OpenInterest, Volume, InstrumentLifecycle) with no dependency on any future OMS code.
- RAII, no globals, small reviewable diffs, commit after each stage.

### Layout
common/ (logging, config, signing, plain types)
fix_common/ (QuickFIX helpers, dictionary and TLS setup)
marketdata/ (lib, apps/md_client, apps/md_sim, tests)
docs/spec/, docs/design.md

### Stages
0. Spec ingest: summarize the spec into docs/design.md (endpoints, logon and signing, message list, custom tags, snapshot/incremental rules, session rules, open questions). Generate the dictionaries. Stop and show me the open questions.
1. Build and session: CMake with QuickFIX building cleanly; logon with spec-defined auth, heartbeats, clean logout; raw message logging with SOH shown as '|'. Connect to the sandbox if I've given credentials, otherwise just compile and unit test.
2. Simulator (md_sim, a QuickFIX acceptor): accepts logon, validates subscriptions, sends snapshot then incrementals for a few instruments. Fault flags: --drop-seq N, --disconnect-after N, --stale-after N seconds. md_client logs on to it.
3. Subscribe and decode: build the subscription request; convert all channels into my plain structs (Trade, BookUpdate, OpenInterest, Volume, InstrumentLifecycle); handle rejects. Print decoded events.
4. Order book and instrument state: per-instrument book applying snapshot and incrementals as the spec defines, top of book and depth accessors, last trade, lifecycle state per the spec. Unit tests including crossed-book detection.
5. Recovery: on sequence gap, disconnect or stale feed (no data for N seconds): mark books invalid, resubscribe, rebuild from a fresh snapshot. Integration tests with each simulator fault flag.

Start with Stage 0 only.
