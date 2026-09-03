#!/usr/bin/env bash
# Quiet build + test for SSG.  Prints ONLY what an agent needs: compiler
# warnings/errors, failing tests, and a final PASS/FAIL line.  Suppresses Ninja's
# per-target progress and CTest's per-test chatter.
#
# Usage:
#   scripts/check.sh            configure (if needed) + build + fast tests
#   scripts/check.sh build      build only
#   scripts/check.sh test       fast tests only (assumes built)
#   scripts/check.sh push       build + tests
#   scripts/check.sh configure  (re)configure only
#
# Wire the push tier to a pre-push hook with:
#
#   ln -s ../../scripts/pre-push.hook .git/hooks/pre-push
#
# Env:
#   JOBS  test parallelism (default: all cores)
#
# Env:
#   BUILD_DIR   build directory (default: build)
#   BUILD_TYPE  CMake build type (default: MinSizeRel)
#   ASSERTIONS  Keep assertions enabled (default: ON)
#
# Ninja is parallel by default (uses all cores); ccache (wired in CMakeLists.txt)
# makes re-builds of an already-seen tree near-instant.

set -u
BUILD_DIR="${BUILD_DIR:-build}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
BUILD_TYPE="${BUILD_TYPE:-MinSizeRel}"
ASSERTIONS="${ASSERTIONS:-ON}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

log="$(mktemp)"
trap 'rm -f "$log"' EXIT

configure() {
    if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
        if ! cmake -S . -B "$BUILD_DIR" -G Ninja \
                -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
                -DSSG_ASSERTIONS="$ASSERTIONS" >"$log" 2>&1; then
            echo "CONFIGURE FAILED:"; cat "$log"; return 1
        fi
    fi
}

build() {
    configure || return 1
    # Ninja auto-parallelizes across all cores; capture output, surface only
    # warnings/errors and the FAILED marker.
    if cmake --build "$BUILD_DIR" >"$log" 2>&1; then
        local warns compiled
        warns="$(grep -cE 'warning:' "$log" || true)"
        # Warnings can only be counted for translation units this invocation
        # actually compiled.  An incremental build that recompiles nothing would
        # otherwise report "warnings: 0" and read as a clean tree, hiding
        # whatever the tree already carries -- the gate's most load-bearing
        # number, silently wrong in the common case.
        compiled="$(grep -cE '^\[[0-9]+/[0-9]+\] (Building|Compiling)' "$log" || true)"
        if [ "${compiled:-0}" -eq 0 ] && [ "${warns:-0}" -eq 0 ]; then
            echo "BUILD OK (warnings: not measured -- nothing recompiled)"
        else
            echo "BUILD OK (warnings: ${warns:-0} in ${compiled:-0} recompiled files)"
        fi
        [ "${warns:-0}" -gt 0 ] && grep -E 'warning:' "$log" | head -40
        return 0
    else
        echo "BUILD FAILED:"
        grep -E 'error:|FAILED|undefined reference|ninja: build stopped' "$log" | head -60
        return 1
    fi
}

run_tests() {
    if ctest --test-dir "$BUILD_DIR" -j"$JOBS" \
            --output-on-failure >"$log" 2>&1; then
        grep -E '% tests passed' "$log" | tail -1
        return 0
    else
        echo "TESTS FAILED:"
        grep -E 'Failed|\*\*\*|% tests passed' "$log" | head -40
        return 1
    fi
}

run_push() {
    run_tests
}

case "${1:-all}" in
    configure) configure && echo "CONFIGURE OK" ;;
    build)     build ;;
    test)      run_tests ;;
    push)      build && run_push ;;
    all)       build && run_tests ;;
    *) echo "usage: scripts/check.sh [configure|build|test|push|all]"; exit 2 ;;
esac
