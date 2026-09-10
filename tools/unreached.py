#!/usr/bin/env python3
"""Print the functions no path from main() can reach.

The reach report asks who *names* a declaration, which is why a chain of
headers referring to each other looks alive: every link is a real reference.
This asks the only question that settles it -- after the linker walks the
call graph out from main() and discards everything it cannot arrive at, what
was left behind? A function absent from the binary is dead however many
places mention it.

Tests are not users; ssg is feature complete, so a function kept alive only
by its test is dead code with a test. This links the real ssg binary, so
tests are excluded by construction rather than by filtering.

The build is -O0 on purpose. Under optimization a function can vanish
because it was inlined into its caller rather than because nothing called
it, and that false positive is indistinguishable afterwards.

Emitted per line:

    <function>\t<object file>

Special members and template instantiations are omitted: the compiler emits
those on demand, so their absence says nothing about your code. Pass --all
to keep them.

Usage:
    tools/unreached.py                  # every unreachable function
    tools/unreached.py --write unreached.txt
    tools/unreached.py --check unreached.txt
"""

import argparse
import pathlib
import re
import shutil
import subprocess
import sys

BUILD = pathlib.Path("/tmp/ssg-unreached-build")
TARGET = "ssg"

# The compiler emits these implicitly wherever they are needed, so their
# absence from the binary reflects its own bookkeeping, not a dead concept.
IMPLICIT = re.compile(r"::~|::operator=|\b(\w+)::\1\(|::operator new|::operator delete")


def run(command: list[str], **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run(command, capture_output=True, text=True, **kwargs)


def configure(repo: pathlib.Path) -> None:
    if BUILD.exists():
        shutil.rmtree(BUILD)
    result = run(
        [
            "cmake",
            "-S",
            str(repo),
            "-B",
            str(BUILD),
            "-G",
            "Ninja",
            "-DCMAKE_BUILD_TYPE=Debug",
            "-DCMAKE_CXX_FLAGS=-ffunction-sections -fdata-sections -O0",
            "-DCMAKE_C_FLAGS=-ffunction-sections -fdata-sections -O0",
            "-DCMAKE_EXE_LINKER_FLAGS=-Wl,--gc-sections",
        ]
    )
    if result.returncode != 0:
        raise SystemExit(f"unreached: configure failed\n{result.stderr}")


def build() -> None:
    result = run(["ninja", "-C", str(BUILD), TARGET])
    if result.returncode != 0:
        raise SystemExit(f"unreached: build failed\n{result.stdout}\n{result.stderr}")


def symbols(path: pathlib.Path) -> set[str]:
    """The mangled function symbols this object or binary defines."""
    result = run(["nm", "--defined-only", str(path)])
    found = set()
    for line in result.stdout.splitlines():
        fields = line.split()
        if len(fields) >= 3 and fields[1] in "TtWw" and fields[2].startswith("_Z"):
            found.add(fields[2])
    return found


def demangle(names: list[str]) -> list[str]:
    if not names:
        return []
    result = run(["c++filt"], input="\n".join(names))
    return result.stdout.splitlines()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=pathlib.Path, default=pathlib.Path.cwd())
    parser.add_argument("--all", action="store_true")
    parser.add_argument("--write", type=pathlib.Path, default=None)
    parser.add_argument("--check", type=pathlib.Path, default=None)
    options = parser.parse_args()

    for tool in ("cmake", "ninja", "nm", "c++filt"):
        if shutil.which(tool) is None:
            raise SystemExit(f"unreached: {tool} is not installed")

    repo = options.repo.resolve()
    configure(repo)
    build()

    binary = BUILD / TARGET
    kept = symbols(binary)

    rows: list[tuple[str, str]] = []
    objects = [
        p
        for p in (BUILD / "CMakeFiles").rglob("*.cpp.o")
        if "vendor" not in str(p) and "_deps" not in str(p)
    ]
    for obj in sorted(objects):
        dropped = sorted(symbols(obj) - kept)
        source = str(obj.relative_to(BUILD))
        for name in demangle(dropped):
            rows.append((name, source))

    rows = [(n, s) for n, s in rows if n.startswith("ssg::")]
    if not options.all:
        rows = [(n, s) for n, s in rows if "<" not in n and not IMPLICIT.search(n)]

    # The same inline function is emitted into every object that uses it, so a
    # name dropped from one object may live in another; report each once.
    report = "".join(
        f"{name}\t{source}\n" for name, source in sorted(set(rows))
    )

    if options.check is not None:
        current = options.check.read_text() if options.check.is_file() else ""
        if current != report:
            print(f"unreached: {options.check} is stale; regenerate with --write")
            return 1
        return 0
    if options.write is not None:
        options.write.write_text(report)
        return 0
    sys.stdout.write(report)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
