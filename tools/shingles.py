#!/usr/bin/env python3
"""Report passages of shared vocabulary between source files.

Two files that use the same run of domain words are often two implementations
of one idea -- the case where someone re-derived a solution locally instead of
calling the one that exists. This reads the words in the implementations, not
the names in the headers, so it catches duplication that no declaration shows.

    tools/shingles.py                 report cross-module shared passages
    tools/shingles.py --words 6       require longer shared runs

How it works, in four steps:

1. Each file becomes a stream of domain words: identifiers split on case and
   underscores, lowercased, with language keywords and ubiquitous terms
   dropped. Comments, punctuation, and string contents never enter the stream.
2. Every run of N consecutive words is hashed. A shared passage in two files
   yields a shared run of hashes regardless of how the code around it differs.
3. Winnowing keeps only the smallest hash in each sliding window of hashes.
   The rule is positional, not random, so identical text always keeps identical
   samples -- which is what makes a bounded sample still guarantee that any
   sufficiently long shared passage is found. (Schleimer, Wilkerson, Aiken,
   "Winnowing: Local Algorithms for Document Fingerprinting", SIGMOD 2003.)
4. Hashes present in more than one file are reported, as the words themselves.

A finding is not automatically a defect. Shared vocabulary can mean shared
subject matter, and two files that legitimately both discuss rows and columns
will match. Those findings are still worth reading: if two files use identical
words for different ideas, the words are wrong.
"""

from __future__ import annotations

import argparse
import collections
import hashlib
import pathlib
import re
import sys

SOURCE_SUFFIXES = (".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx")

# Words that carry no domain meaning: language syntax, ubiquitous type and
# variable vocabulary, and the units of ordinary control flow. Keeping them
# would match every file against every other file.
NOISE = frozenset("""
alignas alignof asm auto bool break case catch char class concept const
consteval constexpr constinit continue decltype default delete
do double dynamic else enum explicit export extern false final float for
friend goto if inline int long mutable namespace new noexcept nullptr
operator override private protected public register reinterpret requires
return short signed sizeof static struct switch template this thread throw
true try typedef typeid typename union unsigned using virtual void volatile
while
begin end size empty clear push back emplace front value type name data
string vector optional span move forward make shared unique pair tuple
result count index idx num len ptr ref tmp temp item elem args
get set has with from into out for and the not all any one two
std detail impl assert static_cast const_cast
""".split())

# A word must be at least this long to be domain vocabulary; shorter ones are
# almost always loop counters and abbreviations.
MIN_WORD = 4


def words(text: str) -> list[str]:
    """The domain vocabulary of a source file, in order.

    Comments and string literals are removed first: a copied comment or an
    error message is not evidence that two implementations share a concept.
    """
    text = re.sub(r"//[^\n]*", " ", text)
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r'"(\\.|[^"\\])*"', " ", text)
    text = re.sub(r"^\s*#\s*include[^\n]*", " ", text, flags=re.M)

    found = []
    for identifier in re.findall(r"[A-Za-z_][A-Za-z0-9_]*", text):
        for word in re.findall(r"[A-Z]+(?![a-z])|[A-Z][a-z]+|[a-z]+", identifier):
            word = word.lower()
            if len(word) >= MIN_WORD and word not in NOISE:
                found.append(word)
    return found


def fingerprints(
    stream: list[str], run: int, window: int
) -> dict[int, int]:
    """Winnowed hash -> position of the word run it stands for.

    Every run of `run` words is hashed; within each sliding window of `window`
    hashes only the smallest survives. Ties keep the rightmost, the convention
    from the winnowing paper, so overlapping windows agree on their choice.
    """
    if len(stream) < run:
        return {}
    hashes = [
        (
            int.from_bytes(
                hashlib.blake2b(
                    " ".join(stream[i:i + run]).encode(), digest_size=8
                ).digest(),
                "big",
            ),
            i,
        )
        for i in range(len(stream) - run + 1)
    ]

    kept: dict[int, int] = {}
    for start in range(max(1, len(hashes) - window + 1)):
        chunk = hashes[start:start + window]
        best = min(chunk, key=lambda pair: (pair[0], -pair[1]))
        kept.setdefault(best[0], best[1])
    return kept


def module(path: str) -> str:
    parts = pathlib.Path(path).parts
    return "/".join(parts[:-1]) or "."


def same_unit(a: str, b: str) -> bool:
    """Whether two paths are one thing declared and defined, or a file and its
    own test. Shared vocabulary between those is the point, not a finding."""
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
    return any(p in TEST_MARKERS for p in parts[:-1]) or parts[-1].startswith(
        ("test_", "test-", "bench_")
    )


def sources(repo: pathlib.Path, with_tests: bool) -> list[pathlib.Path]:
    # Prefix-matched, because build trees multiply: build/, build-release/,
    # build-sanitize/. A generated compiler-probe file matching another one is
    # the loudest possible finding and says nothing about this project.
    skip_prefixes = ("build", "cmake-build", "out", "dist")
    skip_exact = {"vendor", "third_party", "external", ".git", "generated",
                  "node_modules", "__pycache__"}

    def excluded(path: pathlib.Path) -> bool:
        parts = path.relative_to(repo).parts[:-1]
        return any(
            part in skip_exact or part.lower().startswith(skip_prefixes)
            for part in parts
        )

    return sorted(
        p
        for p in repo.rglob("*")
        if p.suffix in SOURCE_SUFFIXES
        and not excluded(p)
        and (with_tests or not is_test(str(p.relative_to(repo))))
    )


def report(repo: pathlib.Path, run: int, window: int, top: int,
           same_module: bool, with_tests: bool) -> str:
    streams: dict[str, list[str]] = {}
    owners: dict[int, list[tuple[str, int]]] = collections.defaultdict(list)
    for path in sources(repo, with_tests):
        rel = str(path.relative_to(repo))
        stream = words(path.read_text(errors="ignore"))
        streams[rel] = stream
        for value, position in fingerprints(stream, run, window).items():
            owners[value].append((rel, position))

    # A passage in three or more files is usually shared idiom rather than a
    # duplicated authority, and it would otherwise dominate the report.
    shared: dict[tuple[str, str], list[str]] = collections.defaultdict(list)
    for holders in owners.values():
        if not 2 <= len(holders) <= 3:
            continue
        for i, (file_a, at_a) in enumerate(holders):
            for file_b, _ in holders[i + 1:]:
                if file_a == file_b or same_unit(file_a, file_b):
                    continue
                if not same_module and module(file_a) == module(file_b):
                    continue
                key = tuple(sorted((file_a, file_b)))
                phrase = " ".join(streams[file_a][at_a:at_a + run])
                shared[key].append(phrase)

    rows = sorted(shared.items(), key=lambda kv: (-len(kv[1]), kv[0]))[:top]
    out = [
        f"# shared vocabulary   ({len(streams)} files, runs of {run} words)",
        "",
    ]
    if not rows:
        out.append("# no cross-module shared passages")
    for (file_a, file_b), phrases in rows:
        out.append(f"{len(phrases):3d} passages  {file_a}, {file_b}")
        for phrase in sorted(set(phrases))[:3]:
            out.append(f"             \"{phrase}\"")
    return "\n".join(out) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default=".")
    parser.add_argument(
        "--words", type=int, default=5, help="words in a shared run"
    )
    parser.add_argument(
        "--window", type=int, default=8, help="hashes per winnowing window"
    )
    parser.add_argument("--top", type=int, default=25)
    parser.add_argument(
        "--same-module",
        action="store_true",
        help="also report pairs inside one directory",
    )
    parser.add_argument(
        "--with-tests", action="store_true", help="include tests"
    )
    args = parser.parse_args()

    sys.stdout.write(
        report(
            pathlib.Path(args.repo).resolve(),
            args.words,
            args.window,
            args.top,
            args.same_module,
            args.with_tests,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
