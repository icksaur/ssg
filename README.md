# SSG

SSG is a library-first, client-server text editor. Its authoritative editor
state runs headlessly so TUI, browser, and future desktop clients can share one
implementation.

The C++20 static library currently includes the foundation types and
Unicode-aware logical-line cell layout. Grapheme segmentation is pinned to
Unicode 15.0.0 using the official `GraphemeBreakProperty.txt`,
`emoji-data.txt`, and `GraphemeBreakTest.txt` data under `data/unicode/`.
The complete architecture and delivery plan are in `doc/spec.md`; `spec.md`
is a root-level link to that canonical document.

## Build and test

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## Layout

- `include/ssg/` — public C++ API
- `src/` — library implementation
- `tests/` — standalone test executables
- `data/unicode/` — pinned Unicode sources, provenance, and update procedure
- `doc/` — feature specifications and backlog
- `doc/spec.md` — canonical project specification
- `doc/learnings.md` — durable project tribal knowledge
- `spec.md` — root-level link to `doc/spec.md`
- `plan.md` — current implementation plan
- `process.md` — parent/child task execution process

## Agent Guide

- Read `plan.md` for current work
- Read `process.md` before orchestrating or implementing tasks
- Read `doc/learnings.md` before implementing a task
- Read `copilot-instructions.md` for cross-cutting invariants
- Read `doc/spec.md` for project architecture
- Read `code-quality.md` before making changes
- Read [cpp-lib-values.md](cpp-lib-values.md) before designing or changing the public C++ API
- Feature designs are in `doc/features/`
- Deferred work and bugs are in `doc/backlog.md`
- Build: `cmake -S . -B build && cmake --build build`
- Test: `ctest --test-dir build --output-on-failure`
- Do not start backlog items without asking
