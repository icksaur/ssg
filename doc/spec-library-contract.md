# spec-library-contract — Library API is the contract (Milestone 11)

Status: PLANNED. Milestone 11 (`milestones.md` §11), an ongoing invariant: the
`ssg` app contains no editor or layout behavior; the command/snapshot/delta
surface the TUI consumes is the same contract a future browser / `--http` client
adheres to. This milestone makes that invariant **executable** and fixes the two
places the audit found the app authoring screen content or owning presentation.

## Goals

- Prove the on-screen result is a **pure function of the library
  `SessionSnapshot`**: `ssg::render(snapshot)` is the sole producer of cells, for
  **every** screen branch (normal, prompt-open, and too-small) — no app-authored
  content or layout on any branch.
- A headless test drives the **production `EditorRuntime`** (not a hand-authored
  fixture model) and reproduces the TUI screen as a `CellGrid` golden.
- Prove the app is **pure transport** with an oracle **independent of the app's
  own encoder**: an independent ANSI terminal model decodes the real `ssg`
  binary's output into an observable grid and it matches `render(snapshot)`.
- Exercise the **delta** path, not just snapshot: a delta-replayed view matches a
  fresh production snapshot after each command, and renders identically.
- Move the two audited violations into the library: the **too-small placeholder**
  (app currently authors literal text + centering) becomes a library-rendered
  grid; and the client's palette/find state is proven to stay within the
  **sanctioned "latency-sensitive derived view"** boundary (spec.md, the I17
  derived-view clause), inventing no product data.

Non-goals: building a browser/HTTP client (none exists yet — the contract is
locked against the TUI). Re-architecting the palette into fully server-owned
state: the client derived view is *explicitly permitted* (below); M11 locks its
boundary rather than removing it.

## Design

### The screen is `render(snapshot)` — including the too-small branch (M11-L)

`ssg::render(SessionSnapshot) -> CellGrid` (`src/render.cpp`) is the single place
shell geometry and content become cells; `CellGrid::canonical()`
(`src/render.cpp:529`) is the deterministic golden serialization. Today `render`
**throws** when the shell viewport is `{0,0}` (below the 20×4 minimum), and the
**app** draws the "terminal too small" text with its own centering/truncation
(`encode_too_small_frame`, `apps/ssg_main.cpp`). That literal content and geometry
are **not** in the snapshot, so the app authors screen content — a real
INV-screen-is-snapshot violation (review MUST).

Fix (M11-L): the library owns the too-small screen. `render` (or a dedicated
`render` path) produces a placeholder `CellGrid` of the requested terminal size
for the too-small state — the message text, its centering, and its palette come
from the library. The app then just `encode_ansi_frame`s that grid like any other
frame; `encode_too_small_frame` is deleted. This requires the snapshot to carry
the raw terminal size even when layout is declined (so `render` can size the
placeholder), or a small typed "too-small view" the library renders.

### Client-owned prompt input is a sanctioned derived view — with a locked boundary

The project invariant permits a client to "compute a **latency-sensitive derived
view** as a pure function of authoritative server-published state plus local
input — … fuzzy filtering/ranking of a published candidate list — provided the
authoritative catalog, command execution, and **presentation placement/color
remain server-owned** and the client invents no product data" (spec.md, the I17
derived-view clause). The TUI's input-line query/selection + `palette_rank` /
`input_line.ghost` / `compute_list_scroll_view` are exactly this: local input +
library functions over the server-published candidate list. So this is **not** an
app violation — but it is also **not** "library-owned"; it is a bounded, sanctioned
client derived view.

M11's job is to LOCK the boundary, not to claim more than is true:
- the authoritative candidate list, command execution, and the rendered palette
  **placement and color** come from the library (`snapshot` → `render`);
- the client's `PaletteReport` carries only a derived view (ranked/windowed
  indices, query, ghost) computed as a pure function of the published candidates
  plus local query — it **invents no product data**.
This is proven by an oracle (M11-4), and it scopes the transport byte/grid proof
(M11-2) to non-prompt screens, where the snapshot is a pure function of dispatch
history; the prompt screen's *render* is still golden-tested via a `PaletteReport`.

### Four proofs + one library fix

- **M11-L** (library fix): the too-small screen becomes a library-rendered grid;
  the app stops authoring it.
- **M11-1** real-runtime render goldens: production `EditorRuntime` → snapshot →
  `render().canonical()` == checked-in goldens, for a normal screen, a prompt
  screen (via `PaletteReport`), and the too-small screen. Distinct from the
  existing `test_tui_fixture.cpp` golden (which renders a hand-authored
  `FixtureModel`).
- **M11-2** app-is-pure-transport via an **independent** terminal model: a small,
  self-contained ANSI decoder (NOT `encode_ansi_frame`) parses the real `ssg`
  binary's captured frame into an observable grid (per-cell text, resolved
  foreground/background color, and cursor position); assert it equals
  `render(snapshot)` with theme indices resolved to colors, for the same headless
  session. Split into (2a) a deterministic pty capture/frame-selection harness and
  (2b) the decode-and-compare oracle. Linux-scoped (`forkpty`); the platform-
  independent part is the decoder + comparison, reusable for a Windows harness.
- **M11-3** delta-path parity: after each scripted command, `derive_session_delta`
  the before/after production snapshots, apply the delta to a replayed view, and
  assert the delta-reconstructed state equals a fresh production snapshot and
  renders to an identical grid. This exercises the "delta" leg of the contract.
- **M11-4** derived-view boundary: the client `PaletteReport` is a pure function
  of the published candidates + local query (recompute it independently and
  compare); the rendered palette placement/color in the snapshot comes only from
  the library; the client introduces no product data (labels/ids/colors) absent
  from the published candidates.

### The independent terminal model (M11-2b), precisely

A minimal ANSI parser sufficient for the renderer's output vocabulary
(`ESC[?25l/h`, `ESC[r;cH` cursor address, `ESC[0m` reset, `ESC[38;2;r;g;b` /
`38;5;n` / `30–37`/`90–97` + bg, printable UTF-8, wide-glyph advance). It builds a
grid of `{text, fg_rgb, bg_rgb}` per cell plus a cursor cell. Comparison fields
(the ONLY observable ones — semantic roles are compile-time and not on the wire):
- cell text (grapheme string), with continuation cells empty;
- resolved foreground/background **SrgbColor** (the app's palette resolved to the
  detected depth), compared to `render(snapshot)`'s palette entry for that cell
  resolved through the SAME `resolve_color(depth)` — note this shares
  `resolve_color` (a pure library mapping, not the encoder under test), which is
  acceptable because the encoder (`encode_ansi_frame`) is what M11-2 scrutinizes,
  not the color mapping;
- cursor position vs `grid.caret`.
Byte-for-byte equality is kept only as a SUPPLEMENTAL check, never the primary
oracle (a faulty encoder would satisfy encoder-vs-encoder equality — review MUST).

## Invariants

- **INV-screen-is-snapshot:** the `CellGrid` is a pure function of the
  `SessionSnapshot` for every branch (normal, prompt, too-small); no app code
  contributes cells. (M11-L removes the last violation; M11-1 locks all branches.)
- **INV-app-transport-only:** an independent terminal model of the real `ssg`
  binary's output equals `render(snapshot)` (text + resolved color + cursor); the
  app adds no screen content. (M11-2.)
- **INV-delta-faithful:** applying `derive_session_delta` to a prior view yields
  the same authoritative view (and grid) as a fresh snapshot after every command.
  (M11-3.)
- **INV-derived-view-bounded:** the client's `PaletteReport` is a pure function of
  published candidates + local query and invents no product data; placement/color
  are server-owned. (M11-4, grounded in the spec.md derived-view clause.)
- **INV-render-deterministic:** `render(snapshot)` is deterministic and
  client-agnostic (same snapshot → same grid across clients).

## Considerations

- **Too-small into the library (M11-L) is a real, small library change**, not
  just a test: `render` must not throw on the declined-layout state; it must size
  and center a placeholder from the terminal dimensions the snapshot carries.
  Audit callers of the throwing path and the M9-T guard in `ssg_main` (which will
  now just render).
- **The existing fixture-model golden stays** (it tests `render` over curated
  sections); M11-1 adds production-runtime goldens with different provenance.
- **Independent decoder scope:** it need only cover the renderer's actual output
  vocabulary; keep it minimal and unit-tested against a couple of hand-built
  frames so the oracle itself is trustworthy.
- **First-frame vs settled frame (M10 deferral):** M11-2 compares the **settled**
  frame (after `prime_deferred` + re-snapshot); the headless reconstruction must
  prime likewise (or both use eager construction) — pick one and match it.
- **Env/winsize fixed** (`TERM`, `COLORTERM`, 80×24) on both the pty child and the
  reconstruction so `detect_color_depth` and `render` agree.
- **Concrete fixtures required (review SHOULD):** each scenario pins the exact
  workspace files, the input byte/command script, the viewport, env, the
  `PaletteReport` (for prompt), the expected selection/scroll, the frame boundary
  rule (split on `ESC[?25l`), the quiescence rule (drain until no new frame for N
  ms), and child termination (`SIGTERM`, reaped). No "representative" hand-waving.
- **Platform (review SHOULD):** the pty capture (M11-2a) is Linux-only; the
  decoder + comparison (M11-2b), the render goldens (M11-1), the delta parity
  (M11-3), and the derived-view oracle (M11-4) are platform-independent and are
  the portable contract gates. A Windows capture harness is a later add.
- **Delta coverage completeness:** replay every command in the script, not just
  the final state, so a delta that drops a section fails.

## Risks and Mitigations

- *Independent decoder is itself buggy* → unit-test it against hand-built frames
  (a known grid → encode → decode → same grid) before trusting it as an oracle.
- *pty timing flakiness* → deterministic frame selection (settled frame after a
  quiescence window); the portable oracles (M11-1/3/4 + the decoder unit) do not
  depend on the pty, so a flaky capture cannot mask a contract regression.
- *M11-L changes a visible screen* (too-small) → it is the same message, now
  library-owned; golden-tested (M11-1) and visually signed off.
- *Over-scoping* → M11-L is a small library change; the proofs are four
  independently-landable steps; the browser client stays out.

## Acceptance (Definition of Done)

- Observable (needs visual signoff for M11-L): the too-small screen looks the
  same but is now produced by the library; the production-runtime golden matches;
  the real `ssg` frame, decoded independently, matches `render(snapshot)`.
- Budgets: n/a.
- Gates: `cmake --build build` clean; `ctest --test-dir build -E
  performance_measurement` green (incl. the new library render path, goldens,
  decoder unit, parity, delta, and derived-view tests).
- Oracles: one per step, below.

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| M11-L | Move the too-small placeholder into the library: `render` produces a sized/centered placeholder grid for the declined-layout state (snapshot carries the terminal size); delete `encode_too_small_frame`; `ssg_main` renders it like any frame | `include/ssg/render.h`, `src/render.cpp`, `src/runtime/snapshot.cpp` (carry terminal size), `apps/ssg_main.cpp`, `apps/ssg_terminal.{h,cpp}`; `tests/test_render.cpp` | golden: `render(too_small_snapshot).canonical()` equals a hand-authored placeholder grid at several sub-20×4 sizes (incl. 1×1); render no longer throws on `{0,0}` | INV-screen-is-snapshot |
| M11-1 | Real-runtime render goldens: production `EditorRuntime` (create+attach+open+script+prime) → `render(snapshot).canonical()` == goldens for a normal screen, a prompt screen (via `PaletteReport`), and the too-small screen | new `tests/test_library_contract.cpp`, `tests/fixtures/tui/runtime-*.txt`, cmake registration | golden: each production-runtime screen matches its hand-verified fixture; a second render of the same snapshot is identical | INV-screen-is-snapshot, INV-render-deterministic |
| M11-2a | Deterministic pty capture harness: drive the real `ssg` through a pinned non-prompt byte script at fixed winsize/env; select the settled frame (split on `ESC[?25l`, drain to quiescence); reap the child | `tests/test_library_contract.cpp` (Linux-guarded) | the harness returns a stable settled frame across repeated runs for the fixed script | INV-app-transport-only |
| M11-2b | Independent ANSI terminal model + compare: decode the captured frame into `{text, fg_rgb, bg_rgb}` cells + cursor; assert equality with `render(headless snapshot)` resolved to colors; byte-parity kept as a supplemental check | `tests/test_library_contract.cpp`, a small `AnsiScreen` decoder (test-side) | the decoder passes a self-test (known grid→encode→decode round-trip), then the decoded app frame equals `render(snapshot)` field-for-field (text/fg/bg/cursor) | INV-app-transport-only |
| M11-3 | Delta-path parity: after each scripted command, `derive_session_delta(before, after)`; apply to a replayed view; assert it equals a fresh production snapshot and renders to an identical grid | `tests/test_library_contract.cpp` | property: for every command in the script, delta-replayed snapshot == fresh snapshot, and `render` of both is identical | INV-delta-faithful |
| M11-4 | Derived-view boundary: independently recompute the client `PaletteReport` (rank/window from the published candidates + query via the library functions) and compare to the client's; assert the snapshot's palette placement/color come only from the library and the report invents no product data (labels/ids absent from candidates) | `tests/test_library_contract.cpp` | property: the client `PaletteReport` is a pure function of published candidates + query; every rendered palette label/id/color traces to a published candidate + the theme | INV-derived-view-bounded |

## Rationale (skippable)

The milestone's claim is architectural: the TUI is a transport over the same
contract a browser would use, not a second editor. The first draft asserted this
but softened the two places it is not yet true — the app authors the too-small
screen, and it owns palette input state — and it proposed a transport oracle that
compared the app's encoder against itself. The review was right on all counts.
The honest milestone therefore (1) moves the too-small screen into the library so
`render` truly owns every cell; (2) proves transport with an *independent*
terminal model, not the encoder under test; (3) exercises the delta leg, since
the contract is command/snapshot/**delta**; and (4) grounds the client palette
view in the project's own "latency-sensitive derived view" allowance and locks
its boundary rather than pretending it is server-owned. The pty capture is the
only non-portable piece and is deliberately isolated so the portable oracles
carry the contract on both required platforms.
