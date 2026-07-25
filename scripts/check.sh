#!/usr/bin/env bash
# Quiet build + test for SSG.  Prints ONLY what an agent needs: compiler
# warnings/errors, failing tests, and a final PASS/FAIL line.  Suppresses Ninja's
# per-target progress and CTest's per-test chatter.
#
# Usage:
#   scripts/check.sh            configure (if needed) + build + fast tests
#   scripts/check.sh build      build only
#   scripts/check.sh test       fast tests only (assumes built)
#   scripts/check.sh perf       build + ONLY the timing benchmarks
#   scripts/check.sh push       build + EVERY test (the pre-push gate)
#   scripts/check.sh configure  (re)configure only
#
# Two tiers, because the slow checks answer questions an edit-test loop is not
# asking.  The default gate runs the unit tests in parallel and skips three
# labels -- `performance` (timing benchmarks), `performance-correctness`
# (editor_benchmark --verify-only over a large corpus), and `embed` (rebuilds
# the library as an add_subdirectory consumer).  `push` runs all of them, so
# the skipped checks still gate work leaving the machine; wire it to a
# pre-push hook with:
#
#   ln -s ../../scripts/pre-push.hook .git/hooks/pre-push
#
# Env:
#   JOBS  test parallelism (default: all cores)
#
# Env:
#   BUILD_DIR   build directory (default: build)
#   BUILD_TYPE  CMake build type (default: Debug)
#
# Ninja is parallel by default (uses all cores); ccache (wired in CMakeLists.txt)
# makes re-builds of an already-seen tree near-instant.

set -u
BUILD_DIR="${BUILD_DIR:-build}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
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
    # The FAST gate: unit tests only, in parallel.
    #
    # Excluded by label, each for its own reason:
    #   performance             timing benchmarks (~140s); answer "did
    #                           throughput regress?", not "is this correct?"
    #   performance-correctness editor_benchmark --verify-only; a real oracle,
    #                           but ~16s over a large expanded corpus, which is
    #                           the entire critical path of a parallel run
    #   embed                   rebuilds the whole library as an add_subdirectory
    #                           consumer (~7s)
    # All three run in the `push` gate, so nothing is merely dropped.
    if ctest --test-dir "$BUILD_DIR" -j"$JOBS" \
            -LE '^(performance|performance-correctness|embed)$' \
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
    if ctest --test-dir "$BUILD_DIR" -j"$JOBS" -L '^performance$' \
            --output-on-failure >"$log" 2>&1; then
        grep -E '% tests passed' "$log" | tail -1
        return 0
    else
        echo "PERF TESTS FAILED:"
        grep -E 'Failed|\*\*\*|% tests passed' "$log" | head -40
        return 1
    fi
}

run_push() {
    # Everything, including what the fast gate skips.  Intended for a pre-push
    # hook: slow checks still run before work leaves the machine.
    if ctest --test-dir "$BUILD_DIR" -j"$JOBS" \
            --output-on-failure >"$log" 2>&1; then
        grep -E '% tests passed' "$log" | tail -1
        return 0
    else
        echo "PUSH GATE FAILED:"
        grep -E 'Failed|\*\*\*|% tests passed' "$log" | head -40
        return 1
    fi
}

case "${1:-all}" in
    configure) configure && echo "CONFIGURE OK" ;;
    build)     build ;;
    test)      run_tests ;;
    perf)      build && run_perf ;;
    push)      build && run_push ;;
    all)       build && run_tests ;;
    *) echo "usage: scripts/check.sh [configure|build|test|perf|push|all]"; exit 2 ;;
esac
