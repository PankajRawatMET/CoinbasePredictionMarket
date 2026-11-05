# cpm_trader

A trading daemon for Coinbase prediction markets: one market data FIX session,
one order entry FIX session, and a host running strategies over them, with an
operator page served on loopback.

- `docs/design.md` and the other `docs/design-*.md` files are the design record.
- `docs/open-questions*.txt` records every decision and why it was made.

## What you need

A Linux box with **OpenSSL 3.0** and a **C++20** compiler:

| Supported | Not supported |
|---|---|
| Ubuntu 22.04 / 24.04, Debian 12 | Amazon Linux 2 — OpenSSL 1.0.2, GCC 7 |
| Amazon Linux 2023, RHEL 9, Fedora | anything with CMake < 3.20 that cannot reach GitHub |

QuickFIX 1.16 is fetched and compiled from source during the build; nothing
else is vendored.

## Deploying from GitHub

The repository is private. A fresh box has no GitHub key, so **make the
repository public for the length of the deployment and private again
afterwards** — both the download and the clone below are anonymous HTTPS.

```bash
curl -fsSL https://raw.githubusercontent.com/PankajRawatMET/CoinbasePredictionMarket/main/deploy.sh | bash
```

That one command detects the distribution, installs the toolchain, checks
CMake and OpenSSL, clones into `~/cpm`, builds, writes credential templates
into `~/.cpm` with mode 600, and installs a systemd unit — without enabling
it, because a trading daemon should not come up on its own before you have
looked at the box. It never prompts, so it is safe under `curl | bash`.

Overridable: `CPM_INSTALL_DIR`, `CPM_CREDS_DIR`, `CPM_BRANCH`, `CPM_REPO_URL`,
`CPM_SKIP_DEPS=1`, `CPM_SKIP_SERVICE=1`.

### Updating

Make the repository public, then re-run the same script — it pulls, rebuilds
and reinstalls the unit, and leaves your credential files alone:

```bash
cd ~/cpm && ./deploy.sh && sudo systemctl restart cpm-trader
```

## Credentials

Three files in `~/.cpm`, one per service, each mode 600 and outside the clone
so a pull can never touch them. Fill in the templates the deployment wrote:

| File | Variables | For |
|---|---|---|
| `creds.env` | `CPM_*` | market data session |
| `creds-oe.env` | `CPM_OE_*` | order entry session, and `CPM_OE_SUBACCOUNT` |
| `creds-rest.env` | `CPM_REST_*` | instrument catalog (optional) |
| `trader.env` | `CPM_TRADER_ARGS` | flags for the service |

Use a **subaccount nothing else trades**. The order entry session receives
execution reports for everything on the subaccount, and a report for an order
this daemon did not send is an anomaly that halts the run — see
`docs/design-oms.md` §3.

Without `creds-rest.env` a strategy can only name a full ticker: FIX has no
SecurityList, so a series like `BTCD15M` cannot be resolved to the window that
is open now.

## Running it

Two build trees:

| Command | Tree | Holds |
|---|---|---|
| `./build.sh` | `build/` | the daemon alone, no tests — what `deploy.sh` builds on a server |
| `./build.sh sim` | `build-sim/` | everything: daemon, `md_sim`, `oe_sim`, `binance_sim`, the clients, the test binaries |
| `./build.sh tests` | `build-sim/` | the same, then `ctest` |
| `./build.sh debug` | `build-sim/` | `Debug` instead of `RelWithDebInfo` |

`CPM_BUILD_DIR` overrides the directory for any mode. `debug` and `sim` share
`build-sim/` and differ in build type, so switching between them reconfigures
and recompiles that tree; give `debug` its own `CPM_BUILD_DIR` if you alternate.

```bash
./build.sh              # build/      the daemon
./build.sh sim          # build-sim/  + simulators and tests
./build.sh tests        # build-sim/  + ctest

./start.sh              # foreground, observer: nothing can be armed
./start.sh --allow-orders
./status.sh             # running? which run? what link?
./url.sh                # the operator page's link for the run going on now
./stop.sh               # SIGTERM: withdraw, log out, save the bookmark
```

As a service:

```bash
echo 'CPM_TRADER_ARGS=--allow-orders' > ~/.cpm/trader.env   # when it should trade
sudo systemctl enable --now cpm-trader
journalctl -u cpm-trader -f
```

The unit restarts only a genuine crash. A daemon that halted (exit 4), found
another holding its bookmark (69), was given bad configuration (78) or bad
flags (64), could not bind the GUI port (73) or failed to start (2) stays
down — every one of those needs a human to read the reason.

`./build.sh` bounds compiler jobs by memory, one per 2 GB. QuickFIX's larger
translation units peak near a gigabyte each, and an unbounded `-j` on a 7 GB
box drove it into swap until it stopped responding.

## Reaching the operator page

The page binds `127.0.0.1` and nothing else, and every request needs that
run's token. An SSH local forward is how you reach it:

```bash
ssh -L 8080:localhost:8080 <user>@<host>
```

The durable form, set once in `%USERPROFILE%\.ssh\config` on Windows:

```
Host cpm
  HostName <ec2-host>
  User ubuntu
  LocalForward 8080 localhost:8080
```

Then, on the box, `./url.sh` prints the link with its token. The daemon also
prints it at startup — highlighted, if it has a terminal — but under systemd
there is no terminal, so `url.sh` reads it out of the run's log instead.

## Logs

One directory per run, under `logs/<UTC day>/<start time>/`, with
`logs/latest` symlinked to the run going on now:

| File | What is in it |
|---|---|
| `cpm-trader.log` | sessions, rolls, halts, anything that went wrong |
| `strategy.log` | a strategy on its own quoting |
| `wire.log` | raw FIX both ways |
| `fills.csv`, `orders.csv`, `pnl.csv` | the run's records |

Each file is a single timeline, so `sort -m` puts any two of them back
together.
