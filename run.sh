#!/usr/bin/env bash
#==============================================================================
# Run cpm_trader the way this project is usually driven by hand:
#
#   ./run.sh                     == source config/creds.env && \
#                                   ./build/trader/cpm_trader --allow-orders \
#                                   --binance-on --max-orders-per-sec 1000000
#   ./run.sh --port 8090         the same, plus anything else cpm_trader takes
#   ./run.sh --max-orders-per-sec 20     later flags win, so this one does
#   CPM_RUN_ARGS= ./run.sh       no defaults at all: observer, no reference feed
#   CPM_RUN_ARGS="--allow-orders" ./run.sh      your own set instead
#
# Note what the defaults mean: orders are PERMITTED (each strategy still has
# to be armed), the Binance reference feed is held open, and the session's
# order rate limit is lifted from 8/s to a million, which is to say off. That
# is deliberate for hand-driven sandbox runs and is not what a deployed box
# should do - the service started by deploy.sh defaults to observer instead.
#==============================================================================
set -euo pipefail
cd "$(dirname "$0")"

# `-` and not `:-`, so CPM_RUN_ARGS= (set but empty) really means no flags.
DEFAULT_ARGS="--allow-orders --binance-on --max-orders-per-sec 1000000"
ARGS="${CPM_RUN_ARGS-$DEFAULT_ARGS}"

CREDS="${CPM_CREDS:-config/creds.env}"
if [ -z "${CPM_OE_API_KEY:-}" ]; then
    [ -f "$CREDS" ] || { echo "no credentials: $CREDS is missing (set CPM_CREDS to point elsewhere)" >&2; exit 78; }
    # set -a covers a file written without `export`; config/creds.env has it.
    set -a
    # shellcheck disable=SC1090
    . "$CREDS"
    set +a
fi

# build/ is the daemon-only tree, build-sim/ the one with the simulators and
# the tests in it. Prefer build/, fall back to build-sim/, and let
# CPM_BUILD_DIR name either outright.
BIN_DIR="${CPM_BUILD_DIR:-}"
if [ -z "$BIN_DIR" ]; then
    for d in build build-sim; do
        [ -x "$d/trader/cpm_trader" ] && { BIN_DIR="$d"; break; }
    done
fi
[ -n "${BIN_DIR:-}" ] && [ -x "$BIN_DIR/trader/cpm_trader" ] || {
    echo "not built: run ./build.sh (or ./build.sh sim), or set CPM_BUILD_DIR" >&2; exit 1; }

echo "run: $BIN_DIR/trader/cpm_trader $ARGS $*"
# shellcheck disable=SC2086
exec "./$BIN_DIR/trader/cpm_trader" $ARGS "$@"
