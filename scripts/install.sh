#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SSG_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

if ! command -v makepkg >/dev/null 2>&1; then
    echo "makepkg is required; install Arch Linux's base-devel package group" >&2
    exit 1
fi

if [[ -e /usr/local/bin/ssg ]]; then
    echo "/usr/local/bin/ssg is an unmanaged legacy installation that would" >&2
    echo "shadow the package. Remove it before installing: sudo rm /usr/local/bin/ssg" >&2
    exit 1
fi

cd "$SSG_ROOT"
exec makepkg --syncdeps --install --force "$@"
