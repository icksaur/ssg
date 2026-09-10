#!/usr/bin/env python3
"""Print the small types that may not be earning their name.

Dead code is gone; what remains is reachable but not necessarily worth a
concept. Two shapes recur here.

A *bag* is a record whose fields are all public and whose only methods are
trivial one-line predicates -- it carries values without owning behavior, so
it is a tuple that was given a name. That is not automatically wrong: a name
earns its keep when callers say it. The `named` column counts places that
write the type's name outside its own declaration, so a bag returned by one
function and received by `auto` everywhere shows up as a bag nothing names.

A *forwarder* is a function whose body is a single return of one call --
a layer that adds a stack frame and nothing else. Forwarders are reported
separately since they are what makes a bag flow through code that does not
read it.

Neither is a verdict. Both are questions worth asking of a type, ranked so
the cheapest ones to answer come first.

Bag columns:

    <named>\t<header>\t<name>\t<fields>\t<field types>

Usage:
    tools/shapes.py                    # bags, least-named first
    tools/shapes.py --forwarders       # single-call pass-through functions
    tools/shapes.py --max-fields 3
    tools/shapes.py --write shapes.txt
"""

import argparse
import collections
import functools
import json
import multiprocessing
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import reach  # noqa: E402  -- shares the parse setup this depends on

import clang.cindex as ci  # noqa: E402

RECORDS = {ci.CursorKind.STRUCT_DECL, ci.CursorKind.CLASS_DECL}


def trivial_body(cursor: ci.Cursor) -> bool:
    """True when this method's body is a single return statement."""
    body = next(
        (c for c in cursor.get_children() if c.kind == ci.CursorKind.COMPOUND_STMT),
        None,
    )
    if body is None:
        return False
    statements = list(body.get_children())
    return len(statements) == 1 and statements[0].kind == ci.CursorKind.RETURN_STMT


def bag(cursor: ci.Cursor) -> tuple[list[str], list[str]] | None:
    """The field names and types of a record that only carries values."""
    fields: list[str] = []
    types: list[str] = []
    for child in cursor.get_children():
        if child.kind == ci.CursorKind.FIELD_DECL:
            if child.access_specifier != ci.AccessSpecifier.PUBLIC:
                return None
            fields.append(child.spelling)
            types.append(child.type.spelling)
        elif child.kind == ci.CursorKind.CXX_METHOD:
            # A method with a real body is behavior; the type owns something.
            if not trivial_body(child):
                return None
        elif child.kind == ci.CursorKind.CXX_BASE_SPECIFIER:
            return None
    return (fields, types) if fields else None


def header_bags(header: pathlib.Path, root: pathlib.Path, args: list[str]) -> list:
    unit = ci.Index.create().parse(str(header), args=args)
    found = []
    pending = list(unit.cursor.get_children())
    while pending:
        cursor = pending.pop()
        located = cursor.location.file
        if located is None or pathlib.Path(located.name) != header:
            continue
        pending.extend(cursor.get_children())
        if cursor.kind not in RECORDS or not cursor.is_definition():
            continue
        if not reach.is_public_header(located.name, root):
            continue
        name = reach.qualified_name(cursor)
        if not name or name.startswith(reach.SKIP_NAMES):
            continue
        shape = bag(cursor)
        if shape is not None:
            found.append((name, pathlib.Path(located.name).name, shape[0], shape[1]))
    return found


def source_forwarders(entry: dict) -> list[tuple[str, str, int]]:
    """Functions in this translation unit whose body is one return of a call."""
    args = reach.parse_args(entry)
    try:
        unit = ci.Index.create().parse(entry["file"], args=args)
    except ci.TranslationUnitLoadError:
        return []
    source = pathlib.Path(entry["file"]).resolve()
    found = []
    pending = list(unit.cursor.get_children())
    while pending:
        cursor = pending.pop()
        located = cursor.location.file
        if located is None or pathlib.Path(located.name).resolve() != source:
            continue
        pending.extend(cursor.get_children())
        if cursor.kind not in (
            ci.CursorKind.FUNCTION_DECL,
            ci.CursorKind.CXX_METHOD,
        ):
            continue
        if not cursor.is_definition() or not trivial_body(cursor):
            continue
        body = next(
            c for c in cursor.get_children() if c.kind == ci.CursorKind.COMPOUND_STMT
        )
        returned = list(list(body.get_children())[0].get_children())
        if not returned:
            continue
        calls = [
            c
            for c in returned[0].walk_preorder()
            if c.kind == ci.CursorKind.CALL_EXPR and c.spelling
        ]
        if len(calls) != 1:
            continue
        # A relay passes its own parameters straight through. An accessor
        # reaching into a member, or a call that supplies a constant the
        # caller never saw, is doing something the caller cannot.
        parameters = [
            c.spelling
            for c in cursor.get_children()
            if c.kind == ci.CursorKind.PARM_DECL
        ]
        forwarded = [
            c.spelling
            for c in calls[0].walk_preorder()
            if c.kind == ci.CursorKind.DECL_REF_EXPR
        ]
        if all(p in forwarded for p in parameters):
            found.append((reach.qualified_name(cursor), source.name, len(parameters)))
    return found


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=pathlib.Path, default=pathlib.Path.cwd())
    parser.add_argument("--builddir", type=pathlib.Path, default=None)
    parser.add_argument("--max-fields", type=int, default=4)
    parser.add_argument("--forwarders", action="store_true")
    parser.add_argument("--jobs", type=int, default=multiprocessing.cpu_count())
    parser.add_argument("--write", type=pathlib.Path, default=None)
    options = parser.parse_args()

    repo = options.repo.resolve()
    root = (repo / reach.INCLUDE_ROOT).resolve()
    builddir = options.builddir or repo / "build"
    database = builddir / "compile_commands.json"
    if not database.is_file():
        raise SystemExit(f"shapes: no compile database at {database}")
    entries = [
        e for e in json.loads(database.read_text()) if "/tests/" not in e["file"]
    ]

    if options.forwarders:
        rows = []
        with multiprocessing.Pool(options.jobs) as pool:
            for found in pool.imap_unordered(source_forwarders, entries, chunksize=1):
                rows.extend(found)
        report = "".join(
            f"{count}\t{n}\t{s}\n" for n, s, count in sorted(set(rows), key=lambda r: (-r[2], r[0]))
        )
    else:
        headers = sorted(p for p in root.rglob("*.h") if "detail" not in p.parts)
        args = reach.header_args(root)
        bags = []
        with multiprocessing.Pool(options.jobs) as pool:
            for found in pool.imap_unordered(
                functools.partial(header_bags, root=root, args=args),
                headers,
                chunksize=1,
            ):
                bags.extend(found)

        named: dict[str, int] = collections.Counter()
        shorts = {name: name.rsplit("::", 1)[-1] for name, _, _, _ in bags}
        homes = {name: header for name, header, _, _ in bags}
        for path in list(repo.glob("src/*.cpp")) + list(root.rglob("*.h")):
            words = collections.Counter(
                re.findall(r"[A-Za-z_]\w*", path.read_text(errors="ignore"))
            )
            for name, short in shorts.items():
                # The declaration itself is not a use, and neither is a mention
                # inside the header that declares it: a name earns its keep by
                # being said somewhere that had the choice not to.
                if path.name == homes[name]:
                    continue
                named[name] += words[short]

        rows = [
            (named.get(name, 0), header, name, len(fields), ", ".join(types))
            for name, header, fields, types in sorted(set(map(tuple_key, bags)))
            if len(fields) <= options.max_fields
        ]
        report = "".join(
            "\t".join(str(f) for f in row) + "\n" for row in sorted(rows)
        )

    if options.write is not None:
        options.write.write_text(report)
        return 0
    sys.stdout.write(report)
    return 0


def tuple_key(item):
    name, header, fields, types = item
    return (name, header, tuple(fields), tuple(types))


if __name__ == "__main__":
    raise SystemExit(main())
