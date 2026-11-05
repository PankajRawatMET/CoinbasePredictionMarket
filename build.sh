#!/usr/bin/env bash
#==============================================================================
# cpm_trader - build
#
# Two trees, on purpose:
#
#   ./build.sh            -> build/      the daemon alone, no tests.
#                                        What deploy.sh builds on a server.
#   ./build.sh sim        -> build-sim/  everything: the daemon, md_sim,
#                                        oe_sim, binance_sim, the clients and
#                                        the test binaries.
#   ./build.sh tests      -> build-sim/  the same, then ctest.
#   ./build.sh debug      -> build-sim/  Debug instead of RelWithDebInfo.
#
# CPM_BUILD_DIR overrides the directory for any mode.
#
# `debug` and `sim` share build-sim/ and differ in build type, so switching
# between them reconfigures that tree and recompiles it. Give debug its own
# CPM_BUILD_DIR if you go back and forth.
#
# Jobs are bounded by MEMORY, not by core count. QuickFIX is fetched and
# compiled from source by cmake/Dependencies.cmake, and one translation unit
# of it peaks near 1 GB: an unbounded -j on a 7 GB box drove this machine
# into swap until it stopped responding. So: one job per 2 GB of RAM, never
# more than the core count, never fewer than one.
#==============================================================================
set -euo pipefail

cd "$(dirname "$0")"

mode="${1:-release}"
case "$mode" in
    release|"") dir="build";     build_type="RelWithDebInfo"; tests="OFF"; targets="cpm_trader"; run_tests="no" ;;
    sim)        dir="build-sim"; build_type="RelWithDebInfo"; tests="ON";  targets="";           run_tests="no" ;;
    tests)      dir="build-sim"; build_type="RelWithDebInfo"; tests="ON";  targets="";           run_tests="yes" ;;
    debug)      dir="build-sim"; build_type="Debug";          tests="ON";  targets="";           run_tests="no" ;;
    *) echo "usage: $0 [release|sim|tests|debug]" >&2; exit 64 ;;
esac
build_dir="${CPM_BUILD_DIR:-$dir}"

cores=$(nproc 2>/dev/null || echo 1)
mem_kb=$(awk '/^MemTotal:/ {print $2}' /proc/meminfo 2>/dev/null || echo 2097152)
by_memory=$(( mem_kb / 2097152 ))          # one job per 2 GB
[ "$by_memory" -lt 1 ] && by_memory=1
jobs=$(( by_memory < cores ? by_memory : cores ))

echo "build: $build_dir, $build_type, tests $tests, -j$jobs (${cores} cores, $(( mem_kb / 1024 )) MB RAM)"

cmake -S . -B "$build_dir" \
      -DCMAKE_BUILD_TYPE="$build_type" \
      -DCPM_BUILD_TESTS="$tests"
# A bare `release` builds the one target a server needs; every other mode
# builds everything, simulators and test binaries included.
if [ -n "$targets" ]; then
    # shellcheck disable=SC2086
    cmake --build "$build_dir" --target $targets -j"$jobs"
else
    cmake --build "$build_dir" -j"$jobs"
fi

if [ ! -x "$build_dir/trader/cpm_trader" ]; then
    echo "build finished but $build_dir/trader/cpm_trader is missing" >&2
    exit 1
fi

if [ "$run_tests" = "yes" ]; then
    ctest --test-dir "$build_dir" --output-on-failure -j"$jobs"
fi

echo "built: $PWD/$build_dir/trader/cpm_trader"
