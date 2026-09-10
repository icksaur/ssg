#!/usr/bin/env python3
"""Print how far each public concept actually reaches.

The census says what a consumer *can* name. This says who *does*. A type
declared in include/ but referenced by exactly one translation unit is not
public surface -- it is a private implementation detail that happens to be
visible, and it can usually move into that .cpp and stop being a concept.

One line per concept, sorted by reach then name:

    <count>\t<header>\t<kind>\t<name>\t<users>

`users` is the comma-separated translation-unit basenames that reference it,
so a one-user line names the file the declaration should move into. The
defining .cpp of a header counts as a user like any other; that is the whole
point, since "only its own .cpp uses it" is exactly the interesting case.

Reach is measured by clang cursor references across the real build, so a name
mentioned only in a comment or a dead #if does not count as a use. Reads
compile_commands.json for the exact flags each TU is built with -- configure
first (cmake --preset dev) or pass --builddir.

Usage:
    tools/reach.py                     # every concept, least-used first
    tools/reach.py --write reach.txt   # the committed report
    tools/reach.py --check reach.txt   # exit 1 if reach.txt is stale
    tools/reach.py --max-users 1       # only the single-user concepts
    tools/reach.py --kinds struct,class
    tools/reach.py --tests             # count tests/ as users too
"""

from __future__ import annotations

import argparse
import collections
import ctypes
import functools
import json
import multiprocessing
import os
import pathlib
import shutil
import subprocess
import sys

import clang.cindex as ci

# The python bindings do not wrap the overload-set accessors, and ctypes
# defaults to an int return, so declare them before first use.
ci.conf.lib.clang_getNumOverloadedDecls.restype = ctypes.c_uint
ci.conf.lib.clang_getOverloadedDecl.restype = ci.Cursor

INCLUDE_ROOT = "include"
SKIP_NAMES = ("detail::", "std::", "__")

# Kinds worth reporting. Enumerators and fields are reached through their
# parent, so counting them separately would report thousands of lines that a
# reader cannot act on independently.
KINDS = {
    ci.CursorKind.CLASS_DECL: "class",
    ci.CursorKind.STRUCT_DECL: "struct",
    ci.CursorKind.CLASS_TEMPLATE: "class",
    ci.CursorKind.ENUM_DECL: "enum",
    ci.CursorKind.TYPEDEF_DECL: "alias",
    ci.CursorKind.TYPE_ALIAS_DECL: "alias",
    ci.CursorKind.FUNCTION_DECL: "func",
    ci.CursorKind.FUNCTION_TEMPLATE: "func",
}


def qualified_name(cursor: ci.Cursor) -> str:
    parts = []
    node = cursor
    while node is not None and node.kind != ci.CursorKind.TRANSLATION_UNIT:
        if node.spelling:
            parts.append(node.spelling)
        node = node.semantic_parent
    return "::".join(reversed(parts))


def is_public_header(path: str | None, root: pathlib.Path) -> bool:
    if not path:
        return False
    try:
        resolved = pathlib.Path(path).resolve()
    except OSError:
        return False
    return root in resolved.parents


@functools.cache
def system_include_args() -> list[str]:
    """The host compiler's own include search path.

    libclang does not inherit a compiler driver's built-in paths, so without
    this every header fails on <cstddef>.
    """
    driver = shutil.which("clang++") or shutil.which("g++") or shutil.which("c++")
    if driver is None:
        return []
    proc = subprocess.run(
        [driver, "-E", "-x", "c++", "-v", os.devnull],
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


def declarations(repo: pathlib.Path) -> dict[str, tuple[str, str]]:
    """Every public concept, keyed by USR so overloads and redeclarations merge."""
    root = (repo / INCLUDE_ROOT).resolve()
    index = ci.Index.create()
    found: dict[str, tuple[str, str]] = {}
    headers = sorted(p for p in root.rglob("*.h") if "detail" not in p.parts)
    args = [
        "-std=c++20",
        # A .h is C by default; the census's headers are C++.
        "-xc++-header",
        f"-I{root}",
        f"-I{root.parent / INCLUDE_ROOT}",
        *(f"-I{d}" for d in root.iterdir() if d.is_dir()),
        *system_include_args(),
    ]
    for header in headers:
        unit = index.parse(str(header), args=args)
        for cursor in unit.cursor.walk_preorder():
            kind = KINDS.get(cursor.kind)
            if kind is None:
                continue
            # A free function declared in a header has no definition there, so
            # requiring one would report only the few defined inline.
            if kind != "func" and not cursor.is_definition():
                continue
            if not is_public_header(
                cursor.location.file.name if cursor.location.file else None, root
            ):
                continue
            name = qualified_name(cursor)
            if not name or name.startswith(SKIP_NAMES) or "::" not in name:
                continue
            usr = cursor.get_usr()
            if usr:
                declared = pathlib.Path(cursor.location.file.name).name
                found.setdefault(usr, (declared, kind, name))
    return found


def references(entry: dict, root: pathlib.Path) -> tuple[str, set[str]]:
    """The USRs of public declarations this translation unit references."""
    args = [
        a
        for a in entry["command"].split()
        if not a.startswith("-o") and a not in ("-c", "-MD", "-MT", "-MF")
    ]
    # Drop the compiler driver and the source path; libclang wants flags only.
    args = [a for a in args[1:] if not a.endswith(".cpp") and not a.endswith(".o")]
    # libclang does not ship the driver's own resource headers on its default
    # search path, so without these every translation unit fails on <stddef.h>
    # and clang discards the function bodies that hold most references.
    args += system_include_args()
    index = ci.Index.create()
    try:
        unit = index.parse(entry["file"], args=args)
    except ci.TranslationUnitLoadError:
        return pathlib.Path(entry["file"]).name, set()
    source = pathlib.Path(entry["file"]).resolve()
    seen: set[str] = set()

    def record(target: ci.Cursor) -> None:
        target = target.canonical
        if target.location.file is None:
            return
        if not is_public_header(target.location.file.name, root):
            return
        usr = target.get_usr()
        if usr:
            seen.add(usr)
        # A reference to a method or field is also a use of its parent type.
        parent = target.semantic_parent
        if parent is not None and parent.kind in KINDS:
            parent_usr = parent.get_usr()
            if parent_usr:
                seen.add(parent_usr)

    for cursor in unit.cursor.walk_preorder():
        # Only an explicit reference counts. Using cursor.type.get_declaration()
        # as a fallback would count every type merely *declared* in an included
        # header, which makes each TU look like it uses the whole project.
        target = cursor.referenced
        if target is None or not target.spelling:
            continue
        if cursor.location.file is None:
            continue
        # A declaration is only "used" here if the reference itself is in this
        # TU's own source, not in another header it happens to include.
        if pathlib.Path(cursor.location.file.name).resolve() != source:
            continue
        # A call to an overloaded name resolves to the overload set, which
        # points at the call site rather than any declaration; its candidates
        # are the declarations actually named. The python bindings do not wrap
        # these two, so call libclang directly.
        if target.kind == ci.CursorKind.OVERLOADED_DECL_REF:
            for index_ in range(ci.conf.lib.clang_getNumOverloadedDecls(target)):
                overload = ci.conf.lib.clang_getOverloadedDecl(target, index_)
                # A cursor built by raw ctypes lacks the translation unit the
                # bindings attach, which its location and USR accessors need.
                overload._tu = target._tu
                record(overload)
            continue
        if target.location.file is None:
            continue
        record(target)
    return pathlib.Path(entry["file"]).name, seen


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=pathlib.Path, default=pathlib.Path.cwd())
    parser.add_argument("--builddir", type=pathlib.Path, default=None)
    parser.add_argument("--max-users", type=int, default=None)
    parser.add_argument("--kinds", default=None)
    parser.add_argument("--tests", action="store_true")
    parser.add_argument("--jobs", type=int, default=multiprocessing.cpu_count())
    parser.add_argument("--write", type=pathlib.Path, default=None)
    parser.add_argument("--check", type=pathlib.Path, default=None)
    options = parser.parse_args()

    repo = options.repo.resolve()
    root = (repo / INCLUDE_ROOT).resolve()
    builddir = options.builddir or repo / "build"
    database = builddir / "compile_commands.json"
    if not database.is_file():
        raise SystemExit(
            f"reach: no compile database at {database}. Configure the build first "
            f"(cmake --preset dev) or pass --builddir."
        )

    entries = [
        e
        for e in json.loads(database.read_text())
        if options.tests or "/tests/" not in e["file"]
    ]
    declared = declarations(repo)

    users: dict[str, set[str]] = collections.defaultdict(set)
    with multiprocessing.Pool(options.jobs) as pool:
        for name, seen in pool.starmap(
            references, [(e, root) for e in entries]
        ):
            for usr in seen:
                users[usr].add(name)

    wanted = set(options.kinds.split(",")) if options.kinds else None
    rows = []
    for usr, (header, kind, name) in declared.items():
        if wanted and kind not in wanted:
            continue
        who = sorted(users.get(usr, ()))
        if options.max_users is not None and len(who) > options.max_users:
            continue
        rows.append((len(who), header, kind, name, ",".join(who)))

    report = "".join(
        "\t".join(str(field) for field in row) + "\n"
        for row in sorted(rows, key=lambda r: (r[0], r[3]))
    )

    if options.check is not None:
        existing = options.check.read_text() if options.check.is_file() else ""
        if existing != report:
            print(
                f"reach: {options.check} is stale; regenerate with "
                f"tools/reach.py --write {options.check}",
                file=sys.stderr,
            )
            return 1
        return 0
    if options.write is not None:
        options.write.write_text(report)
        return 0
    sys.stdout.write(report)
    return 0


if __name__ == "__main__":
    sys.exit(main())
