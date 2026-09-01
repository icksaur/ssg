# Developing SSG

Building, testing, benchmarking, and the contributor workflow. For what SSG is
and how to embed it, see [`README.md`](README.md).

## Requirements

- CMake 3.14 or newer
- A C++20 compiler
- Lua 5.4 headers and library

Linux and Windows are the required platforms.

## Build

Configure once with the `dev` preset (Ninja + ccache, Debug), then iterate with
a single build command:

```sh
cmake --preset dev     # one-time configuration into build/
cmake --build build    # steady-state build
```

Fast inner loop for iteration:

```sh
cmake --build build --target test_document   # build only the target you touched
ctest --preset dev                           # fast unit tests (~0.3s)
```

`ctest --preset dev` excludes the performance, recovery, and theme suites so the
unit loop stays sub-second. Run everything with:

```sh
ctest --preset all
```

Release and sanitizer builds use their own presets and out-of-source build
directories (`build-release/`, `build-sanitize/`):

```sh
cmake --preset release && cmake --build build-release
cmake --preset sanitize && cmake --build build-sanitize && ctest --preset sanitize
```

## What the library delivers

The user-facing capability set, stated as engineering deliverables:

- Headless editing library with UTF-8 validation, Unicode 15 grapheme/cell
  layout, multiple cursors (add-next-occurrence, add-cursor-up/down, split-
  selection-into-lines), undo/redo, clipboard registers, find/replace, command
  palette, configurable keymaps, and 160 stable commands.
- CWD-focused workspaces with tabs and split panes, atomic file operations,
  encoding and mixed-EOL preservation, scratch recovery, external-change
  handling, filesystem/Git/symbol trees, live diffs, and follow-edits.
- Shared monospace presentation model with wrapping, mouse hit targets (click to
  place the cursor, double-click to select a word, drag to select), wheel and
  scrollbar navigation, middle-click to close a tab, a collapsible left panel,
  status header/footer, and fully themeable per-role and per-syntax-scope colors.
- Tree-sitter syntax state, LSP synchronization/diagnostics/language features
  and atomic workspace edits, plus a capability-limited Lua 5.4 command host.
- A reference TUI adapter (`examples/tui/`) and focused presentation fixtures.
- A deterministic 10,000-operation performance benchmark.

## Performance benchmark

The benchmark verifies the pinned corpus and deterministic operation script:

```sh
cmake --build build --target editor_benchmark
./build/editor_benchmark --verify-only
./build/editor_benchmark --enforce
```

The designated-host limits are:

- Edit latency below 1 ms p50 and 4 ms p99
- Command-to-delta latency below 2 ms p50 and 8 ms p99
- First viewport for a 10 MiB document below 250 ms
- No unchanged-viewport cell payload and no polling CPU while idle

## Data and configuration

- `data/required-commands.json` — exact required command catalog
- `data/unicode/` — pinned Unicode 15 source data and provenance
- `data/ui/status_fields.json` — header/footer collapse priorities

Runtime settings support default, user, workspace, language, and document
scopes. Workspace file authority and persisted relative paths are rooted at the
canonical CWD.

## Publishing checklist

Before pushing to a public remote, scan the source tree for personal
information and secrets:

```sh
node ../scan-pii.js .          # scan the whole tree
node ../scan-pii.js . --staged # scan only staged files (git hooks)
```

Exit code 0 is clean; 1 means findings to review.

## Project documentation

- `AGENTS.md` — ambient contract: where a promise lives, when to spec, gates
- `doc/config.md` — user guide: writing `init.lua`
- `doc/commands.md` — generated command reference
- `doc/learnings.md` — durable implementation and integration constraints
- `cpp-values.md` — public C++ API design values
- `backlog.md` (repository root) — deferred work, and the process for it
