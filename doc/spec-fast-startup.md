# spec-fast-startup — Fast startup (Milestone 10)

Status: PLANNED. Milestone 10 (`milestones.md` §10). Measurement-first: the
wall-clock budget is chosen from a measured baseline (M10-1), not guessed. The
durable contract is a set of **structural invariants** that keep the fast path
structural, provable without a stopwatch.

## Goals

`ssg <file>` reaches its first drawn frame quickly and stays that way as the
codebase grows:

- A **process-cold, cache-warm** start (`execve` → first frame bytes written to
  the tty, with the file and workspace already in the OS page cache — the
  repeated-launch case a user actually feels) completes within a budget fixed on
  the project benchmark host.
- No optional subsystem (Lua, LSP, Tree-sitter grammar load, filesystem watcher,
  HTTP) is constructed or initialized before the first frame.
- The first-frame path performs no **deferrable** whole-document or
  whole-workspace pass beyond the one unavoidable document scan: syntax
  enrichment and the workspace tree scan happen *after* the first frame, not on
  it.
- The guarantees are enforced by tests that mostly do **not** depend on
  wall-clock time, so they hold across hosts and resist regression.

Non-goals: very-large-file mode (>10 MiB, deferred per spec.md); making the
first frame sub-linear in document size (see "Bounding reality" — this needs
viewport-projected document reads/cell-runs, a separate future milestone);
optimizing warm re-render (the existing edit/command budgets cover it);
binary-size / link-time as a headline (a secondary guardrail only).

## Design

### Bounding reality (what CAN and CANNOT be made viewport-bounded)

An earlier draft asserted first-frame cost is bounded by the viewport. That is
**false today and out of scope to fix in M10**, and the spec must not claim it.
The audit (below) shows the first frame necessarily performs at least one
O(document) pass:
- `file.open` reads the whole file into the piece tree (you cannot show a file
  without loading it; lazy/mmapped loading is the deferred very-large-file
  feature).
- Every `snapshot` calls `Impl::active_text()` (a full-document string copy) and
  `Impl::active_cell_runs()` (`compute_cell_run` over **every line** —
  `src/editor_runtime.cpp:611`), so producing the first viewport is O(document).

Therefore M10 does **not** promise sub-linear first frames. It promises two
weaker, achievable, and still valuable things:
1. **No optional subsystem** runs before the first frame (structural, provable
   without timing).
2. **No *redundant or deferrable* whole-document / whole-workspace pass** on the
   first-frame path *beyond* that one unavoidable document scan. The offenders
   the audit finds — `refresh_syntax` re-scanning the full text on open, and
   `refresh_tree` scanning the workspace — are moved off the first-frame path.

The residual O(document) cost (file read + one cell-run pass) is governed by the
**existing** `open + first viewport ≤ 250 ms for 10 MiB` budget (spec.md
§Budgets), which M10 preserves. A future milestone may make cell-runs and the
document read viewport-projected; that is explicitly not M10.

### The measurement boundary (define precisely or the metric lies)

- **Start:** process `execve`, observed by a launcher wrapper; a secondary
  interior mark at entry to `main`.
- **Stop:** the `write()` of the **first content frame** to the terminal — the
  first `write_all(frame)` that emits an `encode_ansi_frame` payload, NOT the
  earlier terminal-setup bytes (`terminal_setup_sequence`) and NOT "first
  snapshot ready". Frame identification: the app emits, only under a
  build-time/env instrumentation flag, a single sentinel marker (an interior
  timestamp line to a log fd) immediately before that first content
  `write_all`; the wrapper correlates it with the `execve` timestamp. The
  sentinel and interior marks are compiled out (or gated off) in the Release
  binary whose wall-clock the budget measures, so instrumentation never inflates
  the measured cost.

Phases, each separately timestamped and sub-budgeted from the baseline:
1. **exec+link** — `execve` → `main` entry.
2. **static init** — before `main` body (MUST be ~zero; spec.md forbids
   self-registering globals / static side effects).
3. **runtime construct+attach** — `EditorRuntime::create` + `attach`.
4. **file open** — `file.open` → active document present.
5. **first render+encode+write** — snapshot → first content bytes on the tty.

### Critical path today (audited, `src/editor_runtime.cpp`)

`EditorRuntime::create` → `Impl::Impl` ctor synchronously performs:
- `create_directories` (scratch + recovery); `weakly_canonical` on paths.
- `RecoveryManager::create`, `ScratchStore::create`, `Workspace::create` — the
  last restores the newest unlocked recoverable session.
- **`refresh_tree()`** — `filesystem_tree_snapshot(root)`, a workspace directory
  scan; cost grows with workspace file count. **Deferrable.**
- **`refresh_syntax()`** — `syntax.run` over the whole active-document text
  (empty at construction). `LanguageId::plain_text()`; no Tree-sitter grammar
  loaded here.
- Default keymap load + `validate_keymap`; command binding (`builder.build()`).

`file.open` → `open_document` → **`refresh_syntax()`** re-runs synchronously over
the **entire** opened document — a second O(document) pass on top of the
unavoidable cell-run scan. **Deferrable** (the redundant one to remove/defer).

Off the critical path today (upholding I12): `EditorRuntime::Impl` has **no**
`FilesystemWatcher`, `Lsp*`, or `LuaCommandHost` member; they are separately
linkable and not constructed by the basic editor. M10 keeps and proves this.

### Mechanism: the post-first-frame ownership seam

Deferring library work until "after the first frame" cannot be the app silently
skipping calls — behavior and scheduling stay library-owned. Define a typed
library lifecycle seam (the mechanism; the exact name is not pinned):
- The runtime exposes a **`prime_deferred()`** (working name) command/method the
  app calls once, right after it has written the first content frame. It runs the
  work deferred out of construction/open (workspace tree scan, syntax
  enrichment), which then publishes normally through the snapshot/delta channel.
- Until `prime_deferred` runs, the affected views are in an already-legal empty/
  pending state: the tree panel is collapsible and its provider publishes deltas
  (empty-then-populated is legal), and syntax highlighting has a plain-text
  fallback and "must never block command processing" (spec.md §language
  services). So a first frame with no highlight / empty tree, filled on the next
  snapshot, is within existing contracts.
- The library still owns *what* deferred work is and *when within its own model*
  it may run; the app only signals "I have drawn," exactly as it already signals
  input and size. In-process and WebSocket clients use the same seam, so the
  deferral is not a TUI-only behavior (upholds client parity).

Alternative rejected: a library-internal timer/thread to self-trigger deferral —
rejected because it adds concurrency and a scheduling policy the app is better
placed to drive (it knows when its first frame flushed).

### Measurement harness

`benchmarks/startup_benchmark.cpp` plus a launcher wrapper. It reuses the
existing `performance-ci` discipline but pins a **startup-specific** protocol
(process launches, not operations): a fixed corpus (a small file, a 10 MiB file
from the existing pinned corpus, a deep-tree fixture, a restored-session
fixture, each SHA-256 pinned), a fixed repetition count with a fixed number of
discarded initial launches, a defined page-cache treatment (warm: pre-read the
inputs before the measured launches; a separate cache-cold pass is informational
only), a pty setup for a real terminal, a fixed environment (TERM, COLORTERM),
p50/p99 aggregation, and a recorded baseline report at a named path. Absolute
limits run only on the designated benchmark host; portable CI runs the
structural (timing-free) oracles everywhere.

## Invariants

- **INV-no-optional-init** (refines, does not equal, spec.md **I12**): producing
  the first frame constructs **no** Lua state, LSP process/adapter, Tree-sitter
  grammar, filesystem watcher, or HTTP subsystem. (I12 is broader — it also
  covers the renderer and "unused integrations impose no runtime init" in
  general; this invariant adds the *before-first-frame* temporal boundary and the
  watcher to that list.)
- **INV-no-static-init:** no work runs before `main` (no self-registering
  globals). Upholds the feature-module seam (`editor_session_assembly.cpp`).
- **INV-no-deferrable-fullscan:** the first-frame path performs no deferrable
  whole-document or whole-workspace pass beyond the single unavoidable
  document/cell-run scan. Concretely: `refresh_syntax` does not run a full-text
  pass on the first-frame path, and the workspace tree is not scanned before the
  first frame. (This is the honest replacement for the impossible
  "viewport-bounded" claim.)
- **INV-deferred-work-still-arrives:** every piece of work moved off the
  first-frame path (syntax, tree) is released by the `prime_deferred` seam and
  observably published on a subsequent snapshot/delta — deferral never drops it.
- **INV-deterministic-measurement:** the startup metric uses the pinned
  startup-benchmark protocol (Release, fixed reps, warm-up discard, defined cache
  treatment, p50/p99, provenance) so runs are comparable and regressions
  detectable.
- **INV-idle-after-start** (existing): once the first frame is drawn, an idle
  session consumes no polling CPU (the loop blocks in `select`).

## Considerations

- **Existing 250 ms budget — reconcile, don't duplicate:** spec.md already
  budgets *library-side* `open + first viewport ≤ 250 ms for 10 MiB`. M10's
  `exec → first content frame on tty` is a **distinct end-to-end** metric (adds
  exec+link, app construction, render, and terminal write) with its own inputs
  (a small cache-warm file) and its own threshold. M10-5 **preserves** the 250 ms
  library gate unchanged and **adds** the exec→first-frame gate beside it; the
  two measure different boundaries and neither replaces the other.
- **Process-cold / cache-warm naming:** the hard budget is process-cold (fresh
  `execve`) but cache-warm (inputs in the page cache), matching a user relaunch.
  A cache-cold pass (first-ever open, page-cache misses) is measured and reported
  as provenance, never a hard gate (it is dominated by disk, not our code).
- **The unavoidable O(document) scan is not an offender:** `active_cell_runs` +
  file read are the legitimate cost the 250 ms budget covers. M10-3's oracle must
  target *deferrable* passes (syntax/tree), not total bytes, or it asserts
  something impossible.
- **Frame identification is subtle:** the pty receives terminal-setup bytes
  before content. The sentinel mark must fire at the first *content* frame, not
  the setup write, or the metric is wrong.
- **Session restore on the path:** `Workspace::create` restores the newest
  recoverable session; a large restored session (many tabs, big journals) is a
  worst case — measure it, and defer non-active-tab restoration work past the
  first frame if it breaches the budget.
- **Instrumentation must not perturb the measured cost:** the interior phase
  marks and the content-frame sentinel are gated off in the Release binary whose
  wall-clock the budget measures; only the structural-test / instrumented build
  enables them.

## Risks and Mitigations

- *Claiming an impossible bound* → the viewport-bounded claim is dropped;
  INV-no-deferrable-fullscan is the achievable replacement, and the residual
  O(document) cost is explicitly governed by the existing 250 ms budget.
- *Deferral changes behavior or drops work* → INV-deferred-work-still-arrives is
  asserted (the tree fills and syntax appears on a later snapshot); the empty/
  plain-text intermediate state is already legal.
- *The post-write seam leaks scheduling into the app* → the app only signals "I
  drew"; the library owns what/when deferred work runs and publishes it through
  the normal channel; same seam for all clients.
- *A wall-clock-only spec is flaky/host-bound* → structural invariants
  (INV-no-optional-init, INV-no-static-init, INV-no-deferrable-fullscan) are the
  primary, portable gates; the number is a bench-host gate from the baseline.
- *Instrumentation perturbs measurement* → gated out of the measured Release
  path.
- *Over-scoping* → the plan pairs each oracle with its fix and splits the two
  concrete offenders (syntax, tree) into their own steps; no open-ended "fix
  whatever" step.

## Acceptance (Definition of Done)

- Observable (needs signoff): `time ssg <small file>` in a real terminal feels
  instant and is within the fixed exec→first-frame budget; the tree panel and
  syntax highlighting fill in *after* the first frame with no perceived stall;
  `ssg <10 MiB file>` still meets the existing 250 ms library first-viewport gate.
- Budgets: `exec → first content frame` within the limit fixed from the M10-1
  baseline (process-cold, cache-warm, small file) on the designated benchmark
  host; the existing `open + first viewport ≤ 250 ms / 10 MiB` gate preserved;
  idle CPU zero. Portable CI enforces the structural oracles without wall-clock
  limits.
- Gates: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`, `cmake --build build`,
  `ctest --test-dir build --output-on-failure` green (incl. new structural
  tests); the startup benchmark runs under the benchmark-host workflow.
- Oracles: one per step, below.

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| M10-1 | Startup measurement harness + baseline: launcher wrapper (execve timestamp) + interior phase marks + content-frame sentinel (all instrumentation-gated); pinned startup protocol (fixtures+SHA-256, fixed reps/discards, warm-cache treatment, pty, env); emit a baseline report (small file, 10 MiB, deep tree, restored session) at a named path | `benchmarks/startup_benchmark.cpp`, a launcher wrapper, `apps/ssg_main.cpp` (gated interior marks + sentinel before first content `write_all`), new `cmake/components/startup-benchmark.cmake` (+ auto-register) | reproducibility: the harness reports stable per-phase p50/p99 across the fixed reps with provenance recorded; instrumentation is absent from the measured Release path (assert the sentinel symbol is compiled out when the flag is off) | INV-deterministic-measurement |
| M10-2 | Executable optional-init audit: a construction/link-dependency probe that fails if ANY optional subsystem (Lua/LSP/Tree-sitter grammar/watcher/HTTP) is constructed before the first frame, PLUS a separate pre-`main` static-init probe; coverage is exhaustive over the known optional factories (enumerated in the test) so an un-instrumented new path cannot pass by omission | `tests/test_startup_path.cpp`, small hooks in `src/editor_runtime.cpp` / factories, `cmake` registration | property: (a) before the first snapshot, the count of constructed optional subsystems is 0, asserted against an explicit enumerated list of every optional factory (a missing entry fails a completeness check tied to the linker/catalog); (b) a static-init sentinel proves no user code ran before `main` | INV-no-optional-init, INV-no-static-init |
| M10-3 | Deferral seam + defer syntax-on-open: add the typed `prime_deferred` library lifecycle seam; move `refresh_syntax`'s full-text pass off construction and `file.open` to run under `prime_deferred`; the app calls it right after the first content frame | `include/ssg/editor_runtime.h`, `src/editor_runtime.cpp`, `src/runtime/files.cpp`, `apps/ssg_main.cpp`, `tests/runtime/*`, `tests/test_startup_path.cpp` | ref/property: (a) no full-text `refresh_syntax` runs on the first-frame path (a syntax-run counter is 0 before `prime_deferred`); (b) after `prime_deferred`, highlighting is present on a later snapshot (INV-deferred-work-still-arrives); (c) existing syntax/editing tests stay green | INV-no-deferrable-fullscan, INV-deferred-work-still-arrives |
| M10-4 | Defer the workspace tree scan: move `refresh_tree`'s workspace scan off construction to run under `prime_deferred`; the panel shows empty-then-populated | `src/editor_runtime.cpp`, `src/runtime/*`, `tests/runtime/*`, `tests/test_startup_path.cpp` | property: no `filesystem_tree_snapshot` scan runs before the first frame (a scan counter is 0 pre-`prime_deferred`); the tree provider publishes a populated snapshot after `prime_deferred`; existing tree tests green | INV-no-deferrable-fullscan, INV-deferred-work-still-arrives |
| M10-5 | Pin the wall-clock budget: set exec→first-frame p50/p99 limits from the M10-1 baseline (with margin) on the designated host; ADD it beside the preserved 250 ms/10 MiB library gate in spec.md; portable CI keeps the structural oracles | `benchmarks/startup_benchmark.cpp`, `.github/workflows/benchmark.yml`, `doc/spec.md` §Budgets | the benchmark-host workflow fails if exec→first-frame exceeds the fixed limit; the existing 250 ms gate is unchanged and still enforced; portable CI runs M10-2/3/4 without wall-clock gates | INV-deterministic-measurement |

## Rationale (skippable)

"It goes fast" is not a spec because speed alone is neither falsifiable nor
regression-resistant: a bare number is host-dependent and flaky, and it explains
nothing about *why* the program is fast, so the next careless change silently
breaks it. The durable content of a performance spec is what the number cannot
express — a **precise measurement boundary** (so the metric measures the
user-perceived event, `execve` → content on the tty, not a convenient interior
one) and **structural invariants** that keep the fast path a property of the
architecture (nothing optional runs before the first frame; no deferrable full
scan on it). Crucially, the first draft over-promised: it claimed first-frame
cost is viewport-bounded, but the audit shows the file read and the per-line
cell-run scan are inherently O(document) today, and making them sub-linear is a
separate (very-large-file-adjacent) milestone. The honest, achievable contract
is therefore "no *deferrable* full scan beyond the one unavoidable document
scan," with the residual document-linear cost governed by SSG's *existing* 250 ms
first-viewport budget — which M10 preserves and complements with a distinct
end-to-end exec→first-frame budget, measured first (M10-1) and enforced last
(M10-5) on one controlled host. This extends the same measurement discipline
SSG already applies to edit/command latency to the one boundary it does not yet
cover: cold process start.
