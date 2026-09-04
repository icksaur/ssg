#!/usr/bin/env python3
"""Print the public concept census: every name a consumer of this library can name.

The census is a sorted, deterministic text file. Commit it. When a change adds,
removes, or renames a public concept, the census diff says so in one line --
separate from the implementation noise in the same commit. That diff is the
thing a human (or a fresh-context reviewer) can actually read.

One line per concept:

    <header>\t<kind>\t<name>[\t<signature>]

Kinds: class, struct, enum, enumerator, alias, func, var, macro.
Names are fully qualified. Signatures are canonical clang types, so a parameter
or return-type change shows up as a changed line rather than silence.

Usage:
    tools/census.py                    # write to stdout
    tools/census.py --check FILE       # exit 1 if FILE differs from the census
    tools/census.py --write FILE

Requires the `clang` python bindings and libclang (Linux: extra/clang,
Windows: the LLVM installer; set LIBCLANG_PATH if it is not on the default
search path).
"""

from __future__ import annotations

import argparse
import functools
import multiprocessing
import os
import pathlib
import shutil
import subprocess
import sys

import clang.cindex as ci

# The public surface is whatever lives under this directory. A project either
# includes its own headers flat (<Math.h>) or under a namespace directory
# (<ssg/UiTree.h>), so both levels become include paths -- see include_args.
INCLUDE_ROOT = "include"

# Declarations clang reports that are not part of the readable public surface.
SKIP_NAMES = ("detail::", "std::", "__")

HEADER_SUFFIXES = (".h", ".hpp", ".hh", ".hxx")


def public_root(repo: pathlib.Path) -> pathlib.Path:
    root = repo / INCLUDE_ROOT
    if not root.is_dir():
        raise SystemExit(
            f"census: no public headers at {root}. The census reads the directory "
            f"a consumer includes from; pass --repo to point at the project root."
        )
    return root


def find_headers(repo: pathlib.Path) -> list[pathlib.Path]:
    root = public_root(repo)
    return sorted(
        p
        for p in root.rglob("*")
        if p.suffix in HEADER_SUFFIXES and "detail" not in p.parts
    )


def include_args(repo: pathlib.Path) -> list[str]:
    """Every directory a public header might include a sibling relative to.

    The root itself covers a flat or namespaced layout; its immediate children
    cover a per-layer one, where each layer root is its own include path. Going
    deeper would let a header include a sibling by a path no consumer can use.
    """
    root = public_root(repo)
    roots = [root, *(d for d in sorted(root.iterdir()) if d.is_dir())]
    return [f"-I{d}" for d in roots]


@functools.cache
def system_include_args() -> list[str]:
    """The host compiler's own include search path.

    libclang does not inherit a compiler driver's built-in paths, so without
    this every header fails on <cstddef>. Asking the installed compiler keeps
    this correct on both Linux and Windows without a hardcoded path list.
    Override with CENSUS_FLAGS when the census must be built for another target.
    """
    override = os.environ.get("CENSUS_FLAGS")
    if override:
        return override.split()
    driver = shutil.which("clang++") or shutil.which("g++") or shutil.which("c++")
    if driver is None:
        return []
    null = "NUL" if os.name == "nt" else "/dev/null"
    proc = subprocess.run(
        [driver, "-E", "-x", "c++", "-v", null],
        capture_output=True,
        text=True,
    )
    paths, collecting = [], False
    for line in proc.stderr.splitlines():
        if line.startswith("#include <"):
            collecting = True
        elif line.startswith("End of search list"):
            break
        elif collecting and line.startswith(" "):
            paths.append(line.strip())
    return [arg for p in paths for arg in ("-isystem", p)]


def qualified_name(cursor: ci.Cursor) -> str:
    parts = []
    node = cursor
    while node is not None and node.kind != ci.CursorKind.TRANSLATION_UNIT:
        if node.spelling:
            parts.append(node.spelling)
        node = node.semantic_parent
    return "::".join(reversed(parts))


# Cursor kind -> census kind. Anything not listed is not a named public concept
# (or is reached through one that is).
KINDS = {
    ci.CursorKind.CLASS_DECL: "class",
    ci.CursorKind.STRUCT_DECL: "struct",
    ci.CursorKind.CLASS_TEMPLATE: "class",
    ci.CursorKind.ENUM_DECL: "enum",
    ci.CursorKind.ENUM_CONSTANT_DECL: "enumerator",
    ci.CursorKind.TYPEDEF_DECL: "alias",
    ci.CursorKind.TYPE_ALIAS_DECL: "alias",
    ci.CursorKind.FUNCTION_DECL: "func",
    ci.CursorKind.FUNCTION_TEMPLATE: "func",
    ci.CursorKind.CXX_METHOD: "func",
    ci.CursorKind.CONSTRUCTOR: "func",
    ci.CursorKind.CONVERSION_FUNCTION: "func",
    ci.CursorKind.VAR_DECL: "var",
    ci.CursorKind.FIELD_DECL: "var",
}

FUNC_KINDS = {"func"}

def is_public(cursor: ci.Cursor) -> bool:
    return cursor.access_specifier in (
        ci.AccessSpecifier.INVALID,
        ci.AccessSpecifier.PUBLIC,
    )


def walk(cursor: ci.Cursor, header: str, out: list[str]) -> None:
    for child in cursor.get_children():
        loc = child.location.file
        if loc is None or os.path.normpath(loc.name) != header:
            continue
        if not is_public(child):
            continue
        kind = KINDS.get(child.kind)
        name = qualified_name(child)
        if kind and name and not any(s in name for s in SKIP_NAMES):
            if kind in ("func", "var"):
                out.append(f"{kind}\t{name}\t{child.type.spelling}")
            else:
                out.append(f"{kind}\t{name}")
        # An inline body's locals are not public concepts. Descending into one
        # records `Cache::run::victim` as though a consumer could name it, which
        # both pollutes the census and inflates the ratchet's variable count.
        if kind != "func":
            walk(child, header, out)


def header_entries(job: tuple[str, str, list[str]]) -> tuple[str, list[str], list[str]]:
    path, rel, args = job
    unit = ci.Index.create().parse(path, args=args)
    # A missing include truncates the declaration tree, so the census would be
    # silently short. Any other diagnostic (an incomplete type in a template
    # trait, say) still leaves the declarations themselves intact.
    fatal = [
        f"{rel}: {d.spelling}"
        for d in unit.diagnostics
        if d.severity >= ci.Diagnostic.Error and "file not found" in d.spelling
    ]
    found: list[str] = []
    walk(unit.cursor, os.path.normpath(path), found)
    return rel, found, fatal


def census(repo: pathlib.Path) -> str:
    args = ["-x", "c++", "-std=c++20", *system_include_args(), *include_args(repo)]
    root = repo / INCLUDE_ROOT
    jobs = [
        (str(h), h.relative_to(root).as_posix(), args) for h in find_headers(repo)
    ]
    lines: list[str] = []
    fatal: list[str] = []
    with multiprocessing.Pool() as pool:
        for rel, found, errors in pool.imap_unordered(header_entries, jobs):
            lines.extend(f"{rel}\t{entry}" for entry in found)
            fatal.extend(errors)
    if fatal:
        raise SystemExit(
            "census: headers could not find an include, so the census would be "
            "incomplete:\n  " + "\n  ".join(sorted(fatal)[:20])
        )
    return "".join(f"{line}\n" for line in sorted(set(lines)))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default=".", help="repository root")
    parser.add_argument("--write", metavar="FILE", help="write the census to FILE")
    parser.add_argument("--check", metavar="FILE", help="fail if FILE is stale")
    args = parser.parse_args()

    repo = pathlib.Path(args.repo).resolve()
    text = census(repo)

    if args.check:
        path = pathlib.Path(args.check)
        old = path.read_text(encoding="utf-8") if path.exists() else ""
        if old == text:
            return 0
        import difflib

        diff = difflib.unified_diff(
            old.splitlines(True), text.splitlines(True), "committed", "actual"
        )
        sys.stdout.writelines(diff)
        print(
            f"\ncensus: {path} is stale. Review the diff above; if the public "
            f"surface should change, run: tools/census.py --write {path}",
            file=sys.stderr,
        )
        return 1

    if args.write:
        pathlib.Path(args.write).write_text(text, encoding="utf-8")
        return 0

    sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
