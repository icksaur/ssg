#!/usr/bin/env bash
#
# Build and install the SSG terminal editor on Arch Linux (and derivatives).
#
# Usage:
#   scripts/install.sh [--prefix DIR] [--no-deps] [--build-only]
#
#   --prefix DIR   Install prefix (default: /usr/local). The binary goes to
#                  $PREFIX/bin/ssg. A non-writable prefix is installed with sudo.
#   --no-deps      Skip the pacman dependency step (assume tools are present).
#   --build-only   Configure and build, but do not install.
#
# libgit2 and the tree-sitter grammars are vendored in-tree and built from
# source, so they need no system packages.

set -euo pipefail

PREFIX="/usr/local"
INSTALL_DEPS=1
DO_INSTALL=1

while [[ $# -gt 0 ]]; do
    case "$1" in
        --prefix)     PREFIX="${2:?--prefix needs a directory}"; shift 2 ;;
        --prefix=*)   PREFIX="${1#*=}"; shift ;;
        --no-deps)    INSTALL_DEPS=0; shift ;;
        --build-only) DO_INSTALL=0; shift ;;
        -h|--help)    awk 'NR>=3 && /^#/ {sub(/^# ?/,""); print; next} NR>=3 {exit}' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

# Resolve the SSG source root from this script's location, independent of cwd.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SSG_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$SSG_ROOT/build-release"

say() { printf '\033[1;36m==>\033[0m %s\n' "$*"; }

# --- Dependencies ---------------------------------------------------------
if [[ "$INSTALL_DEPS" -eq 1 ]]; then
    if ! command -v pacman >/dev/null 2>&1; then
        echo "pacman not found; this OS is not Arch-based. Re-run with --no-deps" >&2
        echo "after installing: base-devel cmake ninja lua git (ccache optional)." >&2
        exit 1
    fi
    # lua in the Arch repos is 5.4. ccache is optional but the presets expect it.
    PKGS=(base-devel cmake ninja lua git ccache)
    say "Installing build dependencies: ${PKGS[*]}"
    sudo pacman -S --needed "${PKGS[@]}"
fi

for tool in cmake ninja git; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "required tool '$tool' not found on PATH" >&2; exit 1; }
done

# --- Configure & build ----------------------------------------------------
# Configure directly (no preset) so ccache is optional: the release preset
# hard-codes ccache as the launcher, which fails when it is absent.
CCACHE_ARG=()
if command -v ccache >/dev/null 2>&1; then
    CCACHE_ARG=(-DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
fi

say "Configuring (Release) in $BUILD_DIR"
cmake -S "$SSG_ROOT" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release "${CCACHE_ARG[@]}"

say "Building the ssg binary"
cmake --build "$BUILD_DIR" --target ssg_app

BIN="$BUILD_DIR/ssg"
[[ -x "$BIN" ]] || { echo "build did not produce $BIN" >&2; exit 1; }
say "Built $BIN"

# --- Install --------------------------------------------------------------
if [[ "$DO_INSTALL" -eq 0 ]]; then
    say "Build-only requested; skipping install. Run it with: $BIN ."
    exit 0
fi

DEST="$PREFIX/bin"
say "Installing to $DEST/ssg"
if mkdir -p "$DEST" 2>/dev/null && [[ -w "$DEST" ]]; then
    install -Dm755 "$BIN" "$DEST/ssg"
else
    sudo install -Dm755 "$BIN" "$DEST/ssg"
fi

say "Done. Run 'ssg .' to open the current directory."
