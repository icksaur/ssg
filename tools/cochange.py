#!/usr/bin/env python3
"""Report files that keep changing together, from git history alone.

Two files that always change together implement one concept between them. That
is sometimes correct (a platform pair) and sometimes a concept with no home.
This tool cannot tell those apart, so it reports rather than gates -- run it
when choosing what to consolidate, not on every commit.

    tools/cochange.py                 the last 300 commits
    tools/cochange.py --commits 1000  a longer window
    tools/cochange.py --all           every commit

The window matters. Git history is append-only, so a pair stays near the top
long after the problem is fixed; a bounded window answers "what is smearing
now" instead of "what did this project once struggle with".

Language-agnostic by construction: it reads paths from `git log`, never source.
"""

from __future__ import annotations

import argparse
import collections
import itertools
import pathlib
import subprocess
import sys

SOURCE_SUFFIXES = (".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx",
                   ".py", ".js", ".ts", ".rs", ".go", ".java", ".cs", ".lua")

# A commit touching more than this is a rename, a reformat, or a sweep. It says
# nothing about which files share a concept, and its pairs would swamp the rest.
BULK_COMMIT_FILES = 15


def commits(repo: pathlib.Path, limit: int | None) -> list[set[str]]:
    """Commits as sets of source paths that still exist.

    A deleted or renamed path is dropped as history is read, not when pairs are
    ranked. The goal is to improve the code that is here: a pair naming a file
    nobody can open is not actionable, and counting deleted files would also
    distort the surviving files' commit totals, push real commits past the bulk
    threshold, and leave one-file commits contributing pairs.
    """
    argv = ["git", "log", "--name-only", "--pretty=format:%H", "--no-merges"]
    if limit:
        argv.append(f"-n{limit}")
    result = subprocess.run(argv, capture_output=True, text=True, cwd=repo)
    if result.returncode != 0:
        raise SystemExit(f"cochange: git log failed in {repo}\n{result.stderr}")

    found: list[set[str]] = []
    current: set[str] | None = None
    for line in result.stdout.splitlines():
        if not line.strip():
            continue
        if len(line) == 40 and all(c in "0123456789abcdef" for c in line):
            current = set()
            found.append(current)
        elif (
            current is not None
            and line.endswith(SOURCE_SUFFIXES)
            and (repo / line).is_file()
        ):
            current.add(line)
    return [c for c in found if 1 < len(c) <= BULK_COMMIT_FILES]


def same_unit(a: str, b: str) -> bool:
    """Whether two paths are one thing: a header and its implementation, or a
    file and its own test. Their co-change is structure, not evidence."""
    def stem(path: str) -> str:
        name = pathlib.Path(path).stem.lower().replace("_", "")
        for prefix in ("test", "tests", "bench"):
            if name.startswith(prefix):
                name = name[len(prefix):]
        return name

    return stem(a) == stem(b)


TEST_MARKERS = ("test", "tests", "spec", "bench", "benchmarks", "examples")


def is_test(path: str) -> bool:
    parts = [p.lower() for p in pathlib.Path(path).parts]
    stem = parts[-1]
    return any(p in TEST_MARKERS for p in parts[:-1]) or stem.startswith(
        ("test_", "test-", "bench_")
    )


def report(repo: pathlib.Path, limit: int | None, top: int,
           with_tests: bool) -> str:
    history = commits(repo, limit)
    changes: collections.Counter[str] = collections.Counter()
    together: collections.Counter[tuple[str, str]] = collections.Counter()
    for files in history:
        if not with_tests:
            files = {f for f in files if not is_test(f)}
        if len(files) < 2:
            continue
        changes.update(files)
        together.update(itertools.combinations(sorted(files), 2))

    pairs = [
        (count, a, b)
        for (a, b), count in together.items()
        if not same_unit(a, b)
    ]
    pairs.sort(key=lambda row: (-row[0], row[1], row[2]))
    pairs = pairs[:top]

    appearances: collections.Counter[str] = collections.Counter()
    for _, a, b in pairs:
        appearances[a] += 1
        appearances[b] += 1

    window = f"{len(history)} usable commits"
    if limit:
        window += f" of the last {limit}"
    out = [f"# files that change together   ({window})", ""]
    out.append("# times together, share of the rarer file's commits")
    for count, a, b in pairs:
        share = count / min(changes[a], changes[b])
        out.append(f"{count:4d}x {share:4.0%}  {name(a)}, {name(b)}")

    out += ["", "# files entangled with the most others"]
    for path, seen in sorted(
        appearances.items(), key=lambda kv: (-kv[1], kv[0])
    )[:top // 3]:
        out.append(f"{seen:4d} pairs  {name(path)}")
    return "\n".join(out) + "\n"


def name(path: str) -> str:
    """Bare filenames read better in a list, but keep the directory when it
    disambiguates a name the project uses more than once."""
    return path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default=".", help="repository to read")
    parser.add_argument(
        "--commits", type=int, default=300, help="how many commits to look back"
    )
    parser.add_argument(
        "--all", action="store_true", help="use the whole history"
    )
    parser.add_argument("--top", type=int, default=30, help="pairs to print")
    parser.add_argument(
        "--with-tests",
        action="store_true",
        help="include tests, which mostly restates that each has one",
    )
    args = parser.parse_args()

    sys.stdout.write(
        report(
            pathlib.Path(args.repo).resolve(),
            None if args.all else args.commits,
            args.top,
            args.with_tests,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
