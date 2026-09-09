#!/usr/bin/env python3
"""Report public concepts that look like a second authority for one fact.

Duplicate authority is worse than duplicate code: the copies drift *semantically*,
so the compiler and the tests both stay quiet while two places disagree about
what is true. An agent produces it by re-deriving a solution locally instead of
finding the concept that already exists.

Two reports, both read off the census so there is one parse and no second
opinion about what "public" means:

  enum overlap    Two enums whose value names substantially coincide. That is
                  the same closed set declared twice -- a panel enum beside a
                  surface enum, a wire kind beside a model kind.

  name clusters   One normalized name (case, separators, and a small set of
                  interchangeable words folded away) declared as a type in more
                  than one layer. `ActivePanelId` in core and `active_panel` in
                  protocol is one fact with two owners.

This is a report, not a verdict: some overlap is a legitimate translation at a
real external boundary. Findings you have decided are correct go in an allow
file, one finding key per line, so the gate stays silent until something *new*
appears.

Usage:
    tools/duplicates.py --census census.txt
    tools/duplicates.py --census census.txt --allow duplicates.allow
"""

from __future__ import annotations

import argparse
import collections
import itertools
import pathlib
import re
import sys

# Two names mean the same thing when they differ only by these. Kept short and
# literal on purpose: a clever normalizer produces findings nobody can explain.
SYNONYMS = {
    "identifier": "id",
    "ident": "id",
    "index": "idx",
    "current": "active",
    "selected": "active",
    "kind": "type",
    "info": "",
    "data": "",
    "the": "",
}

TYPE_KINDS = {"class", "struct", "alias", "enum"}

# Enums sharing at least this share of their smaller value set are reported.
ENUM_OVERLAP = 0.7
# Below this many values an overlap is noise (Ok/Error pairs coincide everywhere).
ENUM_MIN_VALUES = 3


def words(name: str) -> list[str]:
    """Split a C++ identifier into lowercase words, camel or snake."""
    leaf = name.rsplit("::", 1)[-1]
    parts = re.findall(r"[A-Z]+(?![a-z])|[A-Z][a-z]*|[a-z]+|\d+", leaf)
    return [p.lower() for p in parts]


def normalize(name: str) -> str:
    folded = [SYNONYMS.get(w, w) for w in words(name)]
    return " ".join(sorted(w for w in folded if w))


def parse(census_text: str) -> list[tuple[str, str, str]]:
    rows = []
    for line in census_text.splitlines():
        if line.strip():
            header, kind, name = line.split("\t")[:3]
            rows.append((header, kind, name))
    return rows


def enum_findings(rows: list[tuple[str, str, str]]) -> list[tuple[str, str]]:
    values: dict[str, set[str]] = collections.defaultdict(set)
    where: dict[str, str] = {}
    for header, kind, name in rows:
        if kind == "enumerator":
            owner = name.rsplit("::", 1)[0]
            values[owner].add(normalize(name))
            where[owner] = header
    big = {e: v for e, v in values.items() if len(v) >= ENUM_MIN_VALUES}

    findings = []
    for a, b in itertools.combinations(sorted(big), 2):
        shared = big[a] & big[b]
        ratio = len(shared) / min(len(big[a]), len(big[b]))
        if ratio < ENUM_OVERLAP:
            continue
        key = f"enum {a} {b}"

        # Containment is sharper evidence than overlap. An enum holding every
        # value of another plus a case or two is usually a copy that grew,
        # rather than two vocabularies that happen to coincide -- and it names
        # the extra cases, which is the whole question when consolidating.
        if big[a] == big[b]:
            findings.append(
                (key, f"enum {a} ({where[a]}) and {b} ({where[b]}) declare the "
                      f"same values: {', '.join(sorted(big[a]))}")
            )
        elif big[a] < big[b] or big[b] < big[a]:
            small, large = (a, b) if big[a] < big[b] else (b, a)
            extra = sorted(big[large] - big[small])
            findings.append(
                (key, f"enum {large} ({where[large]}) is {small} "
                      f"({where[small]}) plus {', '.join(extra)}")
            )
        else:
            findings.append(
                (key,
                 f"enum overlap {ratio:.0%}: {a} ({where[a]}) and {b} ({where[b]}) "
                 f"share {len(shared)} of {min(len(big[a]), len(big[b]))} values")
            )
    return findings


def name_findings(rows: list[tuple[str, str, str]]) -> list[tuple[str, str]]:
    clusters: dict[str, dict[str, str]] = collections.defaultdict(dict)
    for header, kind, name in rows:
        if kind in TYPE_KINDS and "::" not in name.removeprefix("ssg::"):
            clusters[normalize(name)][name] = header

    findings = []
    for key, members in sorted(clusters.items()):
        # Two spellings in one header are usually a deliberate pair; split
        # across files they are two owners of one fact. In a layered layout the
        # split that matters is across layers, which this also catches.
        if len(members) > 1 and len(set(members.values())) > 1:
            listing = ", ".join(f"{n} ({h})" for n, h in sorted(members.items()))
            findings.append((f"name {key}", f"name cluster: {listing}"))
    return findings


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--census", required=True)
    parser.add_argument("--allow", help="file of accepted finding keys")
    parser.add_argument(
        "--baseline",
        action="store_true",
        help="write every current finding to --allow as accepted debt",
    )
    args = parser.parse_args()

    rows = parse(pathlib.Path(args.census).read_text(encoding="utf-8"))
    findings = enum_findings(rows) + name_findings(rows)

    if args.baseline:
        path = pathlib.Path(args.allow)
        path.write_text(
            "# Accepted duplicate-authority findings, checked by tools/duplicates.py.\n"
            "# Seeded from the surface as it stood, so the gate catches what is NEW.\n"
            "# Every line here is unreviewed debt until someone writes a reason after\n"
            "# it or deletes the line by consolidating the concepts.\n"
            + "".join(f"{key}\n" for key, _ in sorted(findings)),
            encoding="utf-8",
        )
        print(f"duplicates: wrote {len(findings)} accepted findings to {path}")
        return 0

    allowed = set()
    if args.allow and pathlib.Path(args.allow).exists():
        for line in pathlib.Path(args.allow).read_text(encoding="utf-8").splitlines():
            line = line.split("#", 1)[0].strip()
            if line:
                allowed.add(line)

    new = [(key, text) for key, text in sorted(findings) if key not in allowed]
    for _, text in new:
        print(text, file=sys.stderr)

    if new:
        print(
            f"\nduplicates: {len(new)} new finding(s). Consolidate onto one authority, "
            f"or -- if this is a real translation at an external boundary -- record the "
            f"key with the reason:\n"
            + "".join(f"    {key}\n" for key, _ in new),
            file=sys.stderr,
        )
        return 1
    print(f"duplicates: no new findings ({len(allowed)} accepted)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
