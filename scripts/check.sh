#!/usr/bin/env bash
# Quiet build + test for SSG.  Prints ONLY what an agent needs: compiler
# warnings/errors, failing tests, and a final PASS/FAIL line.  Suppresses Ninja's
# per-target progress and CTest's per-test chatter.
#
# Usage:
#   scripts/check.sh            configure (if needed) + build + test
#   scripts/check.sh build      build only
#   scripts/check.sh test       test only (assumes built)
#   scripts/check.sh perf       build + run ONLY the timing benchmarks
#   scripts/check.sh configure  (re)configure only
#
# Timing benchmarks (ctest label `performance`) are opt-in via `perf`, not part
# of the default gate: they are ~140s of the ~160s a full run used to cost, and
# a correctness change does not need them.  Run `perf` when touching anything
# on a hot path, or when investigating a throughput regression.
#
# Env:
#   BUILD_DIR   build directory (default: build)
#   BUILD_TYPE  CMake build type (default: Debug)
#
# Ninja is parallel by default (uses all cores); ccache (wired in CMakeLists.txt)
# makes re-builds of an already-seen tree near-instant.

set -u
BUILD_DIR="${BUILD_DIR:-build}"
BUILD_TYPE="${BUILD_TYPE:-Debug}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

log="$(mktemp)"
trap 'rm -f "$log"' EXIT

configure() {
    if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
        if ! cmake -S . -B "$BUILD_DIR" -G Ninja \
                -DCMAKE_BUILD_TYPE="$BUILD_TYPE" >"$log" 2>&1; then
            echo "CONFIGURE FAILED:"; cat "$log"; return 1
        fi
    fi
}

build() {
    configure || return 1
    # Ninja auto-parallelizes across all cores; capture output, surface only
    # warnings/errors and the FAILED marker.
    if cmake --build "$BUILD_DIR" >"$log" 2>&1; then
        local warns
        warns="$(grep -cE 'warning:' "$log" || true)"
        echo "BUILD OK (warnings: ${warns:-0})"
        [ "${warns:-0}" -gt 0 ] && grep -E 'warning:' "$log" | head -40
        return 0
    else
        echo "BUILD FAILED:"
        grep -E 'error:|FAILED|undefined reference|ninja: build stopped' "$log" | head -60
        return 1
    fi
}

run_tests() {
    # Timing benchmarks are opt-in (scripts/check.sh perf): they cost ~140s and
    # answer a question -- "did throughput regress?" -- that a correctness gate
    # is not asking.  The exclusion is anchored so it drops only the
    # `performance` label; `performance-correctness` is editor_benchmark
    # --verify-only, which verifies edit results over the large corpus without
    # timing anything, and stays here as a real oracle.
    if ctest --test-dir "$BUILD_DIR" -LE '^performance$' \
            --output-on-failure >"$log" 2>&1; then
        grep -E '% tests passed' "$log" | tail -1
        return 0
    else
        echo "TESTS FAILED:"
        grep -E 'Failed|\*\*\*|% tests passed' "$log" | head -40
        return 1
    fi
}

run_perf() {
    if ctest --test-dir "$BUILD_DIR" -L '^performance$' \
            --output-on-failure >"$log" 2>&1; then
        grep -E '% tests passed' "$log" | tail -1
        return 0
    else
        echo "PERF TESTS FAILED:"
        grep -E 'Failed|\*\*\*|% tests passed' "$log" | head -40
        return 1
    fi
}

case "${1:-all}" in
    configure) configure && echo "CONFIGURE OK" ;;
    build)     build ;;
    test)      run_tests ;;
    perf)      build && run_perf ;;
    all)       build && run_tests ;;
    *) echo "usage: scripts/check.sh [configure|build|test|perf|all]"; exit 2 ;;
esac
