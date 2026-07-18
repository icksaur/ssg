# spec-terminal-robustness — Terminal robustness (Milestone 9)

Status: PLANNED. Milestone 9 (`milestones.md` §9).

Step dependencies (not "any order" — see review fold):
- **M9-W** (signal-event wakeup) is a prerequisite for **M9-R** and **M9-X**.
- **M9-C1** (library color mapping) is a prerequisite for **M9-C2** (encoder).
- **M9-T** (layout totality) and **M9-U** (Unicode lock) are independent.

## Goals

`ssg` behaves correctly under real terminal conditions, and every capability it
gains is expressed as a library abstraction so a future browser / `--http`
client presents identically:

- Window resize (`SIGWINCH`) re-lays out at the new size with no artifacts and
  no lag until the next keypress.
- The theme renders faithfully on truecolor, 256-color, and 16-color terminals;
  color reduction is a library mapping, not app-local guesswork.
- Wide and combining Unicode occupies the correct cells end-to-end (already
  library-owned; M9 locks it through the client encoder).
- The terminal is restored on the **supported** termination paths: normal quit,
  an exception unwinding through a top-level boundary in `main`, and a
  terminating signal (`SIGTERM`/`SIGHUP`). `_exit`/`abort` deliberately skip
  cleanup and are explicitly out of scope.

## Design

Ownership principle (drives every step): the library owns everything that is the
same for all clients; the app owns only terminal I/O. The TUI's needs define the
library surface; other clients consume the same surface.

### M9-W — signal-event wakeup (shared prerequisite for M9-R and M9-X)

Both resize and termination need the blocking event loop to react to a signal.
The current loop calls a bare, blocking `::read(STDIN_FILENO, …)`
(`apps/ssg_main.cpp` ~line 432); a signal does not reliably interrupt it and
`select()` is used only for the escape/edge-scroll timeouts. Writing to a
self-pipe from a handler is useless unless the loop is *waiting on that pipe*.

Mechanism: a single **self-pipe signal-event** abstraction.
- One non-blocking pipe. Each installed handler (`SIGWINCH`, `SIGTERM`,
  `SIGHUP`) does nothing but `write()` one distinct tag byte to the write end —
  `write()` is async-signal-safe; ignore `EAGAIN` (a full pipe already means
  "wake pending"). No other work runs in signal context.
- The event loop's wait becomes a `select()`/`poll()` over **both**
  `STDIN_FILENO` and the pipe read end (replacing the bare `read()`), so a
  signal-driven pipe write wakes it deterministically regardless of
  `SA_RESTART`. On wake, drain the pipe, classify the tag bytes, and dispatch
  the corresponding normal-context action (resize re-snapshot, or termination
  restore). The escape-timeout and edge-scroll `select()` calls also add the
  pipe fd so a resize during a drag is not missed.
- Pure seam: a `classify_signal_tags(std::string_view drained) -> SignalEvents`
  helper (which tags are pending: resize / terminate) is pure and unit-testable;
  the fd wiring is app I/O, exercised by a PTY integration test.

### M9-R — resize consumer (depends on M9-W)

On a drained resize tag: re-query `terminal_size()` and re-snapshot at the new
`ViewportDimensions`. Layout is already dimension-parametric
(`runtime.snapshot(client, dims, …)`; `compute_viewport` re-clamps the scroll
offset — spec-scroll R6), so no new library layout code is needed. Coalescing is
free: N `SIGWINCH` collapse to at most one pending tag, so a drag-resize storm
yields one redraw per drain.

### M9-T — layout totality (independent, pure library)

`render`/layout must be **total**: for any dimensions it never throws and never
emits an out-of-range caret. It does **not** invent a 1×1 "correctness floor" —
that contradicts I15 and the spec.md §Small-viewports contract. Instead:
- At or above the **minimum supported grid of 20×4** (spec.md §170), it produces
  a well-formed normal-mode snapshot: every region rect is in-bounds and
  non-overlapping and every hit target is valid.
- **Below 20×4**, `layout_shell` already returns the typed
  `viewport_too_small` state (`src/ui_layout.cpp:342,478`); the client renders a
  degraded placeholder, not malformed geometry. M9-T verifies this holds across
  the whole sweep and, if any degenerate size throws or yields an invalid rect,
  hardens the library — the fix belongs in the library, never a client clamp.

The client passes the raw terminal size and trusts the library's typed outcome
(valid snapshot or `viewport_too_small`).

### M9-C1 — color-depth mapping (independent, the new library abstraction)

Reducing an authoritative theme `SrgbColor` to a terminal-representable color is
identical for every client, so it belongs in the library. New surface (new
`include/ssg/color.h` + `src/color.cpp`, its own `cmake/components/*.cmake`):

```
enum class ColorDepth : std::uint8_t { ansi16, indexed256, truecolor };

struct ResolvedColor {
    enum class Encoding : std::uint8_t { ansi16, indexed256, truecolor } encoding;
    std::uint8_t index;   // ansi16: 0..15; indexed256: 16..255; unused for truecolor
    SrgbColor    rgb;     // truecolor: exact channels; else the assumed swatch's channels
};

[[nodiscard]] ResolvedColor resolve_color(SrgbColor color, ColorDepth depth);
```

Target swatch sets are **authoritative, terminal-defined values baked into the
library** (NOT the theme's own 16 — mapping a theme color to the nearest of its
own palette is a trivial identity and does not address a real terminal):
- `truecolor` → identity: `encoding=truecolor`, `rgb=color`.
- `indexed256` → nearest of the xterm **6×6×6 color cube (indices 16..231)** and
  the **24-step grayscale ramp (232..255)**. The system colors 0..15 are
  deliberately **excluded**: they are terminal-configurable/mutable, so mapping
  onto them would be non-deterministic. 240 fixed swatches; nearest by squared
  Euclidean distance in sRGB.
- `ansi16` → nearest of the **16 standard ANSI/xterm base colors** (the
  canonical xterm default RGB values, pinned in a library table and documented).
  These are assumed values; a terminal may theme them, which is an accepted
  limitation of a 16-color terminal.

Determinism: ties break to the **lowest index** so the mapping is a pure
function with a single golden answer. `rgb` on a reduced result carries the
assumed swatch's canonical channels (for tests and index-less clients).

Relationship to I22 (color authority) — **explicit narrow exception, folded per
review**: I22 makes `Theme` the sole source of color, and the `CellGrid` palette
(the truth) continues to carry only the 16 theme `SrgbColor`s.
`resolve_color` introduces **no color into the theme, snapshot, delta, or API
surface**; it is a client-side *hardware-capability adaptation* applied at encode
time, the same category as a physical terminal approximating a requested RGB
value. It is analogous to spec.md §Themes' promise of "deterministic … ANSI/TUI
presentation." Any client on a reduced-depth terminal uses this same library
mapping, so parity holds. This exception is scoped strictly to reduced-depth
terminal output and grants no path to invent theme colors.

### M9-C2 — depth detection + encoder (depends on M9-C1)

- **App**: detect `ColorDepth` once at startup from the environment —
  `COLORTERM` in {`truecolor`,`24bit`} → `truecolor`; else `TERM` containing
  `256color` → `indexed256`; else `ansi16`. (A heuristic — a **fact**, not an
  invariant; a later setting may override it.)
- `encode_ansi_frame` gains a `ColorDepth` parameter. Per frame it resolves each
  of the 16 palette entries via `resolve_color` and formats the SGR escape for
  the result's encoding: truecolor `38;2;r;g;b` / `48;2;…`; indexed256
  `38;5;n` / `48;5;n`; ansi16 index 0..7 → `30..37` fg / `40..47` bg, 8..15 →
  `90..97` / `100..107`. The cell-grid palette stays `SrgbColor`; resolution
  happens at encode time so one snapshot serves any depth. Escape *formatting*
  and env *detection* are app I/O; the *mapping* is library.

### M9-U — Unicode end-to-end lock (independent, verification)

Width is already library-owned: `compute_cell_run` (UAX #29 GCB + UAX #11 EAW +
emoji, Unicode 15.0.0, `layout.cpp`); `render` places a wide glyph in its first
cell and marks the trailing cell `continuation`; `encode_ansi_frame` emits
nothing for a continuation cell; combining marks fold into their base grapheme
(width 0). No new behavior — M9-U adds an end-to-end golden driven
**text → snapshot → render → encode** (never a hand-built `CellGrid`, which
would not lock the library path) that pins the client path so a future encoder
change cannot silently break wide/combining rendering, and confirms the client
contributes no width logic.

### M9-X — termination restore (depends on M9-W)

`TerminalMode`'s destructor already restores mode, cursor, mouse reporting, and
the primary screen on normal return and stack unwind. Gaps and the fold:
- **Async-signal-safety (review MUST)**: `tcsetattr` is **not** guaranteed
  async-signal-safe, so restoration must **not** run in the handler. Instead,
  `SIGTERM`/`SIGHUP` handlers only `write()` their tag to the M9-W self-pipe;
  the loop drains it, performs the full restore in **normal context**
  (`TerminalMode`'s restore path), then re-raises the signal with the default
  disposition so the wait/exit status is correct (`WTERMSIG` reports the
  original signal).
- **Exception path (review MUST — scope match)**: wrap the loop body in a
  top-level `try/catch` in `main` so an escaping exception restores the terminal
  before rethrow/exit (RAII unwinding is not portably guaranteed once past
  `main`). `_exit`/`abort` remain explicitly unsupported.
- **Testable seam (review SHOULD)**: the restore byte string is a **pure helper
  in `apps/ssg_terminal.{h,cpp}`** (the app *test* target `tests/test_ssg_app.cpp`
  can reach it there; it cannot reach a helper buried in `ssg_main.cpp`). The
  destructor, the exception boundary, and the signal-drain path all use it.

## Invariants

- **INV-app-io-only**: the app contains no editor/layout/color-decision logic.
  Color *reduction* is library (`resolve_color`); only escape *formatting*,
  env *detection*, signal-fd wiring, and termios I/O are app.
- **INV-client-parity**: any presentation capability the TUI has is reachable
  through a library abstraction so all clients present identically —
  `resolve_color` (color), the dimension-parametric snapshot (resize), the
  width layer (Unicode).
- **INV-render-total**: `render(snapshot(dims))` never throws; at/above 20×4 it
  yields a well-formed in-bounds snapshot, and below 20×4 the typed
  `viewport_too_small` state — never malformed geometry (upholds I15, spec.md
  §170).
- **INV-encoder-no-width**: the client encoder derives wide-glyph advance solely
  from the grid's `continuation` flag; it computes no display width itself.
- **I22 (color authority)**: upheld — the theme remains the sole source of color
  in the snapshot/API; `resolve_color` is the scoped reduced-depth adaptation
  documented in M9-C1, adding no color to the theme surface.

## Considerations

- **Self-pipe correctness**: write end non-blocking; handler ignores `EAGAIN`;
  drain the read end before acting; only `write()`/`sigaction` in handler
  context. One pipe, distinct tag bytes per signal.
- **Resize mid-drag**: the edge auto-scroll drag loop (M8-S2) already runs a
  timed `select()`; it must include the self-pipe fd so a `SIGWINCH` re-snapshots
  before the next hit-test, or a pointer routes against stale geometry.
- **Color detection is a heuristic** (fact, not invariant): env detection can be
  wrong; the three-tier rule matches common tools; a future override setting is
  out of scope. Do not over-engineer.
- **Reduced-swatch tables are authoritative and pinned**: the xterm 6×6×6 cube,
  the 24 grays, and the 16 ANSI base RGBs are fixed library data with documented
  provenance; ties break to the lowest index (single golden answer).
- **Totality across UI state, not just size** (review SHOULD): totality must hold
  with the panel shown/hidden, a prompt open (find/replace/palette), split panes,
  and distraction-free mode, over both an empty and an open document — not only
  default state at six sizes.
- **Grid palette stays truth**: never bake a reduced color into the `CellGrid`;
  resolve at encode time so one snapshot serves any depth.

## Risks and Mitigations

- *Signal handler re-entrancy / unsafe restore* → handler does only
  `write()` to the self-pipe; all restoration and `tcsetattr` run in normal
  context, then re-raise default. PTY subprocess test raises `SIGTERM` and
  asserts the emitted restore bytes, a restored (non-raw) slave termios, and
  `WTERMSIG == SIGTERM`.
- *Resize wake races `SA_RESTART`* → the loop waits on the pipe fd via
  `select()`, so the wake is edge-independent of restart semantics; the bare
  `read()` is replaced.
- *Resize storms* → tag coalescing collapses N signals into one redraw.
- *Color regression on truecolor terminals* → `resolve_color(_, truecolor)` is
  identity; the common path is unchanged and golden-guarded.
- *I22 misread as violated* → the exception is documented and scoped; a
  color-origin test asserts the snapshot/API still carries only theme colors.

## Acceptance (Definition of Done)

- Observable (needs signoff — visual):
  - Resize while editing (including a slow drag-resize): layout reflows
    immediately, no artifacts, caret stays on its glyph; shrinking below 20×4
    shows the degraded placeholder, and growing back restores the shell.
  - Same file on truecolor, `256color`, and 16-color `TERM`: theme is
    recognizably the same, no missing/black cells.
  - Open a file with CJK + combining marks + a ZWJ emoji: columns line up; caret
    navigation lands on grapheme boundaries.
  - `kill -TERM` the process: the shell prompt returns with a sane terminal (no
    raw mode, cursor visible, primary screen), exit status reflects the signal.
- Budgets: n/a (correctness milestone; startup budget is M10).
- Gates: `cmake --build build -j20` clean; `ctest --test-dir build -j20 -E
  performance_measurement` all green.
- Oracles: one per step, below.

## Plan

| # | Step | Deps | Files | Oracle | Invariants |
|---|------|------|-------|--------|------------|
| M9-W | Self-pipe signal-event abstraction: non-blocking pipe, async-signal-safe tag-writing handlers, `select()` over stdin + pipe replacing the bare `read()`; pure `classify_signal_tags` | — | `apps/ssg_main.cpp`; `tests/test_ssg_app.cpp` | ref/hand: `classify_signal_tags` maps drained byte multisets → the correct pending {resize, terminate} set incl. coalescing dup tags; PTY test: a signal delivered while idle wakes the loop within one interval | INV-app-io-only |
| M9-T | Verify + harden layout totality across a size × UI-state sweep; below 20×4 must be `viewport_too_small`, never a throw or invalid rect | — | `src/ui_layout.cpp`/`src/render.cpp` (only if a case fails); new `tests/test_resize_totality.cpp` + its `cmake/components/*.cmake` registration | property: for `dims` in {1×1,2×1,1×2,19×4,20×3,20×4,8×3,200×60,80×24} crossed with {panel on/off, prompt open, split, distraction-free} × {empty, open doc}: `snapshot`+`render` never throws; ≥20×4 ⇒ every region rect in-bounds/non-overlapping and every hit target valid; <20×4 ⇒ typed `viewport_too_small` | INV-render-total, I15 |
| M9-R | Resize consumer: on a drained resize tag re-query `terminal_size()` and re-snapshot; include the pipe fd in the escape/edge-scroll waits | M9-W | `apps/ssg_main.cpp` | PTY integration: resize the pty window and raise `SIGWINCH` with **no** keyboard input; observe a frame cursor-addressed to the new dimensions (parse `\x1b\[(\d+);1H` per project PTY technique) | INV-client-parity |
| M9-C1 | Library `resolve_color` + pinned xterm-256 (cube+grays, excl. 0..15) and 16-ANSI base-color tables | — | new `include/ssg/color.h`, `src/color.cpp`, `cmake/components/color-depth.cmake` (+ register); `tests/test_color.cpp` | ref-impl: an independent nearest-swatch search over pinned reference RGB tables (authored in the test, not shared with production) matches `resolve_color` over: the 16 theme colors, hand-computed vectors incl. cube/gray-ramp boundaries and exact ties (lowest-index), and a deterministic broad sRGB sample; truecolor asserted identity | INV-client-parity |
| M9-C2 | App depth detection (`COLORTERM`/`TERM`) + `ColorDepth`-parameterized `encode_ansi_frame` formatting per encoding | M9-C1 | `apps/ssg_terminal.{h,cpp}`, `apps/ssg_main.cpp`; `tests/test_ssg_app.cpp` | golden: encoded frame for the theme at each of the 3 depths (exact SGR bytes: `38;2`/`38;5`/`30..37`+`90..97`); detection unit table over env combinations | INV-app-io-only |
| M9-U | End-to-end Unicode golden driven text→snapshot→render→encode | — | `tests/test_ssg_app.cpp` (+ fixture) | golden: a fixed line (named narrow ASCII + a specific CJK U+4E00, a base+combining U+0301 cluster, a ZWJ emoji sequence) at a fixed viewport → exact `CellGrid` cells + `continuation` flags + caret column, and exact cursor-addressed encoded bytes (one glyph per wide cluster, continuations skipped, caret past a wide glyph advances by 2) | INV-encoder-no-width |
| M9-X | Signal-drain restore in normal context + re-raise; top-level `try/catch` in `main`; pure restore-bytes helper in `ssg_terminal` | M9-W | `apps/ssg_terminal.{h,cpp}` (helper), `apps/ssg_main.cpp` (exception boundary, drain-restore, re-raise); `tests/test_ssg_app.cpp` | hand case: the pure helper returns the exact restore string (equals the destructor's current bytes); PTY subprocess raises `SIGTERM` → asserts emitted restore bytes, restored non-raw slave termios, and `WTERMSIG == SIGTERM` | INV-app-io-only |

## Rationale (skippable)

The milestone's real design question is where color reduction lives. In the app,
a browser client would re-implement the same nearest-swatch math (or diverge),
breaking client parity; as a pure library function
(`SrgbColor × ColorDepth → ResolvedColor`) every client declares only its depth
tier and gets identical swatches — the same reason `render` owns cells and
`compute_cell_run` owns width. Resize and Unicode are, on inspection, already
library-total; M9 proves that (a totality property crossed with UI state, an
end-to-end golden) and fixes the app-side I/O gaps — idle-resize redraw and
signal restore — that are inherently the client's job. The review made the
signal handling honest: one self-pipe wakeup (M9-W) underpins both resize and
termination, restoration runs in normal context because `tcsetattr` is not
async-signal-safe, and the 16/256 targets are real terminal palettes, not the
theme's own colors.
