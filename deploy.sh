#!/usr/bin/env bash
#==============================================================================
# cpm_trader - deployment for a fresh Linux box (AWS or otherwise)
#
#   curl -fsSL https://raw.githubusercontent.com/PankajRawatMET/CoinbasePredictionMarket/main/deploy.sh | bash
#
# or, from inside a clone:
#
#   ./deploy.sh
#
# One file. It installs the toolchain, clones, builds, and writes out the
# systemd unit, the credential templates and the helper scripts itself - so
# the only two scripts this repository carries are this one and build.sh.
#
# The repository must be PUBLIC while this runs: a fresh box has no GitHub
# key, so both the download above and the clone below are anonymous HTTPS.
# Make it private again afterwards; make it public again when you want to
# pull. Nothing here stores a credential or a token.
#
# Supported: Ubuntu 22.04 / 24.04, Amazon Linux 2023, Debian 12, RHEL 9 and
# Fedora. NOT Amazon Linux 2, whose OpenSSL is 1.0.2 and whose GCC is 7:
# this project needs OpenSSL 3.0 and C++20.
#
# Overridable: CPM_REPO_URL, CPM_BRANCH, CPM_INSTALL_DIR, CPM_CREDS_DIR,
# CPM_SKIP_DEPS=1, CPM_SKIP_SERVICE=1.
#==============================================================================
set -euo pipefail

REPO_URL="${CPM_REPO_URL:-https://github.com/PankajRawatMET/CoinbasePredictionMarket.git}"
BRANCH="${CPM_BRANCH:-main}"
INSTALL_DIR="${CPM_INSTALL_DIR:-$HOME/cpm}"
CREDS_DIR="${CPM_CREDS_DIR:-$HOME/.cpm}"
SERVICE_NAME="cpm-trader"

RED=$'\033[0;31m'; GREEN=$'\033[0;32m'; YELLOW=$'\033[1;33m'
BLUE=$'\033[0;34m'; BOLD=$'\033[1m'; NC=$'\033[0m'
if [ ! -t 1 ] || [ -n "${NO_COLOR:-}" ]; then RED=; GREEN=; YELLOW=; BLUE=; BOLD=; NC=; fi

info()  { echo "${BLUE}[info]${NC} $1"; }
ok()    { echo "${GREEN}[ok]${NC} $1"; }
warn()  { echo "${YELLOW}[warn]${NC} $1"; }
fail()  { echo "${RED}[error]${NC} $1" >&2; exit 1; }
step()  { echo; echo "${BOLD}== $1${NC}"; }

trap 'echo; echo "${RED}deployment stopped at line $LINENO.${NC} Nothing was started." >&2' ERR

#------------------------------------------------------------------------------
# Step 1: the box
#------------------------------------------------------------------------------
detect_os() {
    step "Checking the machine"
    [ -f /etc/os-release ] || fail "no /etc/os-release: unsupported system"
    # shellcheck disable=SC1091
    . /etc/os-release
    OS="$ID"; OS_VERSION="${VERSION_ID:-unknown}"
    info "$PRETTY_NAME"

    if [ "$OS" = "amzn" ] && [ "${OS_VERSION%%.*}" = "2" ]; then
        fail "Amazon Linux 2 ships OpenSSL 1.0.2 and GCC 7; this project needs OpenSSL 3.0 and C++20. Use Amazon Linux 2023 or Ubuntu 22.04+."
    fi

    case "$(uname -m)" in
        x86_64|aarch64) ;;
        *) warn "untested architecture $(uname -m)" ;;
    esac
}

#------------------------------------------------------------------------------
# Step 2: toolchain and libraries
#------------------------------------------------------------------------------
install_dependencies() {
    step "Installing build dependencies"
    case "$OS" in
        ubuntu|debian)
            sudo apt-get update -qq
            sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
                build-essential cmake git ca-certificates curl \
                libssl-dev zlib1g-dev tmux
            ;;
        amzn|rhel|centos|rocky|almalinux|fedora)
            local mgr="dnf"; command -v dnf >/dev/null 2>&1 || mgr="yum"
            sudo $mgr groupinstall -y "Development Tools" >/dev/null
            sudo $mgr install -y gcc-c++ cmake git ca-certificates curl \
                openssl-devel zlib-devel tmux
            ;;
        *) fail "unsupported distribution '$OS'. Install git, cmake 3.20+, a C++20 compiler and OpenSSL 3.0 headers, then re-run with CPM_SKIP_DEPS=1." ;;
    esac
    ok "dependencies installed"
}

verify_toolchain() {
    step "Verifying the toolchain"

    command -v cmake >/dev/null 2>&1 || fail "cmake is not on PATH after installation"
    local version major minor
    version=$(cmake --version | head -1 | awk '{print $3}')
    major=${version%%.*}; minor=$(echo "$version" | cut -d. -f2)
    if [ "$major" -lt 3 ] || { [ "$major" -eq 3 ] && [ "$minor" -lt 20 ]; }; then
        warn "cmake $version is older than the required 3.20; installing 3.28.3 into /usr/local"
        local arch installer
        arch=$(uname -m)
        installer="cmake-3.28.3-linux-${arch}.sh"
        ( cd /tmp && curl -fsSLO "https://github.com/Kitware/CMake/releases/download/v3.28.3/${installer}" \
          && sudo sh "./${installer}" --skip-license --prefix=/usr/local >/dev/null \
          && rm -f "./${installer}" )
        export PATH=/usr/local/bin:$PATH
        version=$(cmake --version | head -1 | awk '{print $3}')
    fi
    info "cmake $version"

    # OpenSSL 3.0 is a hard requirement of cmake/Dependencies.cmake; finding
    # out here beats finding out after QuickFIX has compiled.
    local ssl; ssl=$(openssl version 2>/dev/null | awk '{print $2}' || echo "none")
    case "$ssl" in
        3.*) info "openssl $ssl" ;;
        none) warn "openssl command not found; trusting the development headers" ;;
        *) fail "OpenSSL $ssl is too old - this project requires 3.0 or newer" ;;
    esac
    ok "toolchain is good"
}

#------------------------------------------------------------------------------
# Step 3: the source
#------------------------------------------------------------------------------
fetch_source() {
    step "Fetching the source"

    if [ -d "$INSTALL_DIR/.git" ]; then
        info "$INSTALL_DIR is already a clone; pulling $BRANCH"
        # No prompt to re-clone: this script is made to run under
        # `curl | bash`, where stdin is the script itself and a read would
        # swallow the rest of it.
        git -C "$INSTALL_DIR" remote set-url origin "$REPO_URL"
        if ! git -C "$INSTALL_DIR" pull --ff-only origin "$BRANCH"; then
            fail "could not pull. Is the repository public right now? Make it public, then re-run."
        fi
    elif [ -e "$INSTALL_DIR" ]; then
        fail "$INSTALL_DIR exists and is not a git clone. Move it aside, or set CPM_INSTALL_DIR to somewhere else."
    else
        info "cloning $REPO_URL ($BRANCH) into $INSTALL_DIR"
        if ! git clone --branch "$BRANCH" --single-branch "$REPO_URL" "$INSTALL_DIR"; then
            fail "clone failed. A fresh box has no GitHub key, so the repository must be PUBLIC while this runs."
        fi
    fi
    ok "source at $INSTALL_DIR ($(git -C "$INSTALL_DIR" rev-parse --short HEAD))"
}

#------------------------------------------------------------------------------
# Step 4: build
#------------------------------------------------------------------------------
build_platform() {
    step "Building"
    info "QuickFIX is compiled from source; build.sh bounds the job count by RAM, so this takes a while on a small instance"
    chmod +x "$INSTALL_DIR/build.sh" "$INSTALL_DIR/deploy.sh"
    ( cd "$INSTALL_DIR" && ./build.sh release )
    [ -x "$INSTALL_DIR/build/trader/cpm_trader" ] || fail "build finished without producing build/trader/cpm_trader"
    ok "built $INSTALL_DIR/build/trader/cpm_trader"
}

#------------------------------------------------------------------------------
# Step 5: credentials
#
# Three files, one per service, each chmod 600 and outside the clone so a
# pull can never touch them. No `export` and no quotes in any of them: they
# are read both by systemd's EnvironmentFile=, which does not run a shell,
# and by start.sh, which sources them under `set -a`.
#------------------------------------------------------------------------------
setup_credentials() {
    step "Preparing the credential files"
    mkdir -p "$CREDS_DIR"
    chmod 700 "$CREDS_DIR"

    local wrote_any=0

    if [ ! -f "$CREDS_DIR/creds.env" ]; then
        cat > "$CREDS_DIR/creds.env" << 'EOF'
# Market data credentials (CPM_*). chmod 600. Never committed.
# Fill in the keys below by UNCOMMENTING them. A key left as `KEY=` is an
# assignment to the empty string, and systemd reads these files in order - so
# a blank key in a later file would wipe a real value set in an earlier one.
# That also means you may keep every CPM_* in this one file if you prefer.
#CPM_API_KEY=
#CPM_PASSPHRASE=
#CPM_SECRET=
CPM_FIX_HOST=fix-marketdata.cpm-sandbox.fairx.net
CPM_FIX_PORT=6120
CPM_FIX_TLS=1
CPM_FIX_TARGET_COMP_ID=CPMMD
EOF
        chmod 600 "$CREDS_DIR/creds.env"; wrote_any=1
        info "created $CREDS_DIR/creds.env"
    else
        info "$CREDS_DIR/creds.env exists; left untouched"
    fi

    if [ ! -f "$CREDS_DIR/creds-oe.env" ]; then
        cat > "$CREDS_DIR/creds-oe.env" << 'EOF'
# Order entry credentials (CPM_OE_*). chmod 600. Never committed.
#
# A separate file from market data on purpose: both can be sourced into one
# shell and the order client can never inherit the market data endpoint.
#CPM_OE_API_KEY=
#CPM_OE_PASSPHRASE=
#CPM_OE_SECRET=
CPM_OE_HOST=fix-order.cpm-sandbox.fairx.net
CPM_OE_PORT=6110
CPM_OE_TLS=1
CPM_OE_TARGET_COMP_ID=CPMOE
# The subaccount orders are placed for. Required to place an order at all.
# Use one nothing else trades: the order entry session receives execution
# reports for everything on the subaccount, and a report for an order this
# daemon did not send is an anomaly that halts the run.
#CPM_OE_SUBACCOUNT=
CPM_OE_CANCEL_ON_DISCONNECT=1
CPM_OE_SETTLEMENT_REPORTS=1
CPM_OE_MANUAL_ORDERS=0
EOF
        chmod 600 "$CREDS_DIR/creds-oe.env"; wrote_any=1
        info "created $CREDS_DIR/creds-oe.env"
    else
        info "$CREDS_DIR/creds-oe.env exists; left untouched"
    fi

    if [ ! -f "$CREDS_DIR/creds-rest.env" ]; then
        cat > "$CREDS_DIR/creds-rest.env" << 'EOF'
# REST instrument catalog and fill recovery (CPM_REST_*). chmod 600. Never committed.
# Sandbox trading requires these credentials for the same firm as order-entry FIX.
#
# Without it a series target like BTCD15M cannot be resolved to the window
# that is open now - FIX has no SecurityList and the lifecycle feed is
# changes-only - so a strategy could only name a full ticker.
#CPM_REST_API_KEY=
#CPM_REST_PASSPHRASE=
#CPM_REST_SECRET=
CPM_REST_URL=https://api.cpm-sandbox.fairx.net
# CPM_REST_CA_FILE=   # optional: a CA bundle, if the default store is not enough
EOF
        chmod 600 "$CREDS_DIR/creds-rest.env"; wrote_any=1
        info "created $CREDS_DIR/creds-rest.env"
    else
        info "$CREDS_DIR/creds-rest.env exists; left untouched"
    fi

    if [ ! -f "$CREDS_DIR/trader.env" ]; then
        cat > "$CREDS_DIR/trader.env" << 'EOF'
# Flags for the service, split on whitespace by systemd.
#
# Empty means observer: the daemon watches, serves the page, and no strategy
# can be armed. Add --allow-orders only when this box is meant to trade.
#
#   CPM_TRADER_ARGS=--allow-orders --port 8080
#
CPM_TRADER_ARGS=
EOF
        chmod 600 "$CREDS_DIR/trader.env"; wrote_any=1
        info "created $CREDS_DIR/trader.env"
    fi

    [ "$wrote_any" = "1" ] && warn "the new files are empty templates - the daemon cannot log on until you fill them in"
    ok "credentials directory $CREDS_DIR"
}

#------------------------------------------------------------------------------
# Step 6: the systemd service
#
# One unit, not a template-per-date: this daemon runs across days and rolls
# its own series windows, and a second copy on the same state is already
# refused by the flock on <bookmark>.lock.
#
# The heredoc is unquoted so the paths expand now; \$CPM_TRADER_ARGS is
# escaped so systemd, not this shell, does the expanding.
#------------------------------------------------------------------------------
create_systemd_service() {
    if [ "${CPM_SKIP_SERVICE:-0}" = "1" ]; then
        step "Skipping systemd (CPM_SKIP_SERVICE=1)"
        return
    fi
    step "Installing the systemd service"
    command -v systemctl >/dev/null 2>&1 || { warn "no systemctl here; skipping the service"; return; }

    sudo tee "/etc/systemd/system/${SERVICE_NAME}.service" > /dev/null << EOF
[Unit]
Description=cpm_trader - Coinbase prediction market trading daemon
Documentation=file://$INSTALL_DIR/README.md
After=network-online.target
Wants=network-online.target

# Three restarts in five minutes and it stays down: a daemon that cannot
# start is not made to start by trying again. These are [Unit] keys, not
# [Service] ones, whatever older examples show.
StartLimitBurst=3
StartLimitIntervalSec=300

[Service]
Type=simple
User=$(id -un)
Group=$(id -gn)

# The daemon writes its bookmark, its lock and logs/<day>/<run>/ relative to
# here, and the FIX dictionaries are read from the clone's config/.
WorkingDirectory=$INSTALL_DIR

# Credentials, one file per service. A missing market data or order entry
# file fails the start loudly; the REST catalog is optional.
EnvironmentFile=$CREDS_DIR/creds.env
EnvironmentFile=$CREDS_DIR/creds-oe.env
EnvironmentFile=-$CREDS_DIR/creds-rest.env
EnvironmentFile=-$CREDS_DIR/trader.env

# Unquoted \$CPM_TRADER_ARGS is split on whitespace by systemd, which is what
# lets the flags in trader.env arrive as separate arguments. Empty by
# default: no --allow-orders means no strategy can be armed.
ExecStart=$INSTALL_DIR/build/trader/cpm_trader \$CPM_TRADER_ARGS

# SIGTERM is the clean path: strategies withdraw, both sessions log out, the
# bookmark is saved. About five seconds in testing; sixty is room to spare.
KillSignal=SIGTERM
TimeoutStopSec=60

# A crash may restart. A decision must not. cpm_trader exits 4 when it
# halted, 69 when another daemon holds the bookmark, 78 on bad configuration,
# 73 when the GUI port is taken, 64 on bad flags and 2 when startup failed -
# every one of those needs a human to read the reason, and restarting into it
# would either loop or re-enter a market the daemon just withdrew from.
Restart=on-failure
RestartPreventExitStatus=2 4 64 69 73 78
RestartSec=30

# stdout goes to the journal. The daemon keeps its own files under
# logs/<day>/<run>/ as well, and those are the ones to read.
StandardOutput=journal
StandardError=journal
SyslogIdentifier=cpm-trader

# Hardening. ReadWritePaths takes precedence over ProtectHome, which is what
# keeps the install directory writable while the rest of \$HOME is not.
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=read-only
ReadWritePaths=$INSTALL_DIR
PrivateTmp=true
ProtectKernelTunables=true
ProtectControlGroups=true
RestrictSUIDSGID=true

[Install]
WantedBy=multi-user.target
EOF

    sudo systemctl daemon-reload
    ok "installed /etc/systemd/system/${SERVICE_NAME}.service"
    info "not enabled and not started: a trading daemon should not come up on its own before you have checked the box"
}

#------------------------------------------------------------------------------
# Step 7: helper scripts
#------------------------------------------------------------------------------
create_helper_scripts() {
    step "Writing the helper scripts"

    cat > "$INSTALL_DIR/start.sh" << 'EOF'
#!/usr/bin/env bash
# Run cpm_trader in the foreground, with the credentials loaded.
#
#   ./start.sh                     observer: nothing can be armed
#   ./start.sh --allow-orders      permit arming
#   ./start.sh --port 8090 ...     anything else cpm_trader takes
#
# For a long run use the service (systemctl start cpm-trader) or tmux. This
# is the foreground path: Ctrl-C withdraws every strategy and logs out.
set -euo pipefail
cd "$(dirname "$0")"

CREDS_DIR="${CPM_CREDS_DIR:-$HOME/.cpm}"

# set -a exports what the files define, with or without `export` in them.
set -a
for f in creds.env creds-oe.env creds-rest.env trader.env; do
    [ -f "$CREDS_DIR/$f" ] && . "$CREDS_DIR/$f"
done
set +a

if [ -z "${CPM_API_KEY:-}" ] || [ -z "${CPM_OE_API_KEY:-}" ]; then
    echo "no credentials: fill in $CREDS_DIR/creds.env and $CREDS_DIR/creds-oe.env" >&2
    exit 78
fi

[ -x build/trader/cpm_trader ] || { echo "not built yet: run ./build.sh release" >&2; exit 1; }

# Flags from trader.env first, then this command line, so an argument wins.
# shellcheck disable=SC2086
exec ./build/trader/cpm_trader ${CPM_TRADER_ARGS:-} "$@"
EOF

    cat > "$INSTALL_DIR/url.sh" << 'EOF'
#!/usr/bin/env bash
# Print the operator page's link for the run going on now.
#
# The daemon prints it at startup, but it scrolls away - and under systemd
# there is no terminal to print it to at all. The link is in the run's log
# either way, and logs/latest always points at the newest run.
#
#   ./url.sh          the link
#   ./url.sh --open   the link, and open it in a local browser if there is one
set -euo pipefail
cd "$(dirname "$0")"

log="${CPM_LOG_DIR:-logs}/latest/cpm-trader.log"
url=""

if [ -f "$log" ]; then
    url=$(grep -o 'http://localhost[^ ]*' "$log" | tail -1 || true)
fi

if [ -z "$url" ] && command -v journalctl >/dev/null 2>&1; then
    url=$(journalctl -u cpm-trader --no-pager -o cat 2>/dev/null \
          | grep -o 'http://localhost[^ ]*' | tail -1 || true)
fi

if [ -z "$url" ]; then
    echo "no link found. Is the daemon running? Looked in $log and the cpm-trader journal." >&2
    exit 1
fi

echo "$url"

if [ "${1:-}" = "--open" ]; then
    for browser in xdg-open google-chrome firefox; do
        if command -v "$browser" >/dev/null 2>&1; then
            "$browser" "$url" >/dev/null 2>&1 &
            exit 0
        fi
    done
    echo "(no browser on this box - forward the port and open it from your own machine)" >&2
fi
EOF

    cat > "$INSTALL_DIR/stop.sh" << 'EOF'
#!/usr/bin/env bash
# Stop cpm_trader the clean way: SIGTERM, which withdraws every strategy,
# logs both sessions out and saves the bookmark. Never SIGKILL - that would
# leave quotes resting with nobody managing them until the venue's
# cancel-on-disconnect fires.
set -euo pipefail
cd "$(dirname "$0")"

if command -v systemctl >/dev/null 2>&1 && systemctl is-active --quiet cpm-trader 2>/dev/null; then
    echo "stopping the cpm-trader service..."
    sudo systemctl stop cpm-trader
    echo "stopped"
    exit 0
fi

pids=$(pgrep -x cpm_trader || true)
if [ -z "$pids" ]; then
    echo "cpm_trader is not running"
    exit 0
fi

for pid in $pids; do
    echo "sending SIGTERM to $pid (withdrawing, logging out)..."
    kill -TERM "$pid" 2>/dev/null || continue
done

for _ in $(seq 60); do
    pgrep -x cpm_trader >/dev/null || { echo "stopped"; exit 0; }
    sleep 1
done

echo "still running after 60s. Look at the log before forcing anything:" >&2
echo "  tail -40 logs/latest/cpm-trader.log" >&2
exit 1
EOF

    cat > "$INSTALL_DIR/status.sh" << 'EOF'
#!/usr/bin/env bash
# Is it running, where is its log, and what is the link.
set -uo pipefail
cd "$(dirname "$0")"

echo "=== cpm_trader ==="

if command -v systemctl >/dev/null 2>&1 && systemctl list-unit-files cpm-trader.service >/dev/null 2>&1; then
    systemctl status cpm-trader --no-pager --lines=0 2>/dev/null | head -5
    echo
fi

pids=$(pgrep -x cpm_trader || true)
if [ -n "$pids" ]; then
    echo "running:"
    ps -o pid,etime,%cpu,%mem,args -p $pids | sed 's/^/  /'
else
    echo "not running"
fi

run=$(readlink "${CPM_LOG_DIR:-logs}/latest" 2>/dev/null || true)
if [ -n "$run" ]; then
    echo
    echo "newest run: ${CPM_LOG_DIR:-logs}/$run"
    url=$(./url.sh 2>/dev/null || true)
    [ -n "$url" ] && echo "page:       $url"
    echo
    echo "last lines:"
    tail -10 "${CPM_LOG_DIR:-logs}/latest/cpm-trader.log" 2>/dev/null | sed 's/^/  /'
fi
EOF

    chmod +x "$INSTALL_DIR"/start.sh "$INSTALL_DIR"/stop.sh \
             "$INSTALL_DIR"/status.sh "$INSTALL_DIR"/url.sh
    ok "start.sh, stop.sh, status.sh and url.sh written to $INSTALL_DIR"
}

#------------------------------------------------------------------------------
# Step 8: what to do next
#------------------------------------------------------------------------------
print_summary() {
    local host; host=$(hostname -f 2>/dev/null || hostname)
    echo
    echo "${GREEN}${BOLD}cpm_trader is installed.${NC}"
    echo
    echo "  source      $INSTALL_DIR"
    echo "  binary      $INSTALL_DIR/build/trader/cpm_trader"
    echo "  credentials $CREDS_DIR/{creds.env,creds-oe.env,creds-rest.env}"
    echo "  flags       $CREDS_DIR/trader.env"
    echo
    echo "${BOLD}1. Fill in the credentials${NC}"
    echo "     \$EDITOR $CREDS_DIR/creds.env        # CPM_*       market data"
    echo "     \$EDITOR $CREDS_DIR/creds-oe.env     # CPM_OE_*    order entry, and the subaccount"
    echo "     \$EDITOR $CREDS_DIR/creds-rest.env   # CPM_REST_*  instrument catalog and fill recovery"
    echo
    echo "${BOLD}2. Run it in the foreground first${NC}"
    echo "     cd $INSTALL_DIR && ./start.sh"
    echo "   Observer by default: no strategy can be armed until --allow-orders is given."
    echo
    echo "${BOLD}3. Or run it as a service${NC}"
    echo "     echo 'CPM_TRADER_ARGS=--allow-orders' > $CREDS_DIR/trader.env   # when it should trade"
    echo "     sudo systemctl enable --now $SERVICE_NAME"
    echo "     ./status.sh"
    echo
    echo "${BOLD}4. Open the operator page from your own machine${NC}"
    echo "     ssh -L 8080:localhost:8080 $(id -un)@$host"
    echo "     cd $INSTALL_DIR && ./url.sh      # prints the link, token and all"
    echo "   The page binds 127.0.0.1 only; the SSH forward is how it is reached."
    echo
    echo "${BOLD}5. Make the repository private again.${NC}"
    echo "   Make it public again when you next want to pull, then: ./deploy.sh"
    echo
}

#------------------------------------------------------------------------------
# Main
#------------------------------------------------------------------------------
main() {
    echo "${BOLD}cpm_trader deployment${NC}"
    detect_os
    [ "${CPM_SKIP_DEPS:-0}" = "1" ] || install_dependencies
    verify_toolchain
    fetch_source
    build_platform
    setup_credentials
    create_systemd_service
    create_helper_scripts
    print_summary
}

# CPM_DEPLOY_NO_RUN=1 defines the functions without running them, which is
# how the generated unit and helper scripts are tested. An env guard rather
# than a BASH_SOURCE check, because under `curl | bash` there is no source
# file to compare against and main would never run.
[ "${CPM_DEPLOY_NO_RUN:-0}" = "1" ] || main "$@"
