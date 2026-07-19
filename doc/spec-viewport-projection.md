# spec-viewport-projection — Viewport-bounded first frame

Status: PLANNED. Follow-up to Milestone 10 (exceeds the reviewed
`doc/spec-fast-startup.md` scope, which explicitly deferred sub-linear first
frames). Goal: the 10 MiB first frame drops from ~1.7 s to viewport-bounded.

## Goals

- Producing a viewport (the per-frame snapshot) for a large document costs
  O(visible rows) in the expensive per-line work (`compute_cell_run`), plus one
  cheap O(document) byte scan — not O(document) grapheme segmentation.
- The 10 MiB `first_frame` phase (measured ~1.7 s in the M10-1 baseline) becomes
  a few milliseconds, restoring the app to the existing `250 ms / 10 MiB`
  first-viewport budget on the real runtime path (which today it silently
  violates).
- No regression to scrolling, scrollbar, mouse hit-testing, caret reveal, or
  rendering for normal-size documents: the projected result is byte-identical to
  the current full computation where behavior is unchanged.

Non-goals: incremental syntax; lazy/mmapped file loading (still O(document) to
read the file into the piece tree — a separate very-large-file concern); making
the *word-wrap-ON* huge-document case sub-linear (kept exact; see Considerations).

## Design

### Root cause and the wrap entanglement (audited)

The per-frame `EditorRuntime::Impl::viewport(dims)` calls `active_cell_runs()`
(`src/editor_runtime.cpp:611`), which runs `compute_cell_run` for **every**
logical line of the whole document, then `compute_viewport` (`src/viewport.cpp`).
`compute_cell_run` (UAX #29 grapheme segmentation + UAX #11 width) over ~130 000
lines is ~0.9 s of it.

**The first frame does this O(document) segmentation TWICE (review finding).**
Beyond the viewport computation, `render()` calls
`logical_lines(document.text)` (`src/render.cpp:69,357`), which *also* runs
`compute_cell_run` for every line before painting — a second whole-document pass.
And `TextModel` in `src/selection.cpp` builds runs for the whole document to
compute `visual_row`/`visual_column` for caret movement and reveal. So a complete
fix must project **all three** — viewport, render, and selection — or the
~1.7 s first frame is not removed.

`compute_viewport` needs, from **all** lines:
- `total_visual_rows` (= `wrap_rows(...).size()`) for the scrollbar
  (`maximum_first_row`, thumb) and scroll clamping;
- `line_document_start[]` (cumulative byte offsets) so each visible cell's
  `CellHitTarget.byte_offset` is document-absolute (mouse hit-testing, M8);
- the wrap mapping from `first_visual_row` → logical line (caret reveal).

**Key finding:** `compute_viewport` *always* wraps long lines at
`dimensions.columns` (`wrap_rows`, `src/viewport.cpp:20`). The runtime tracks
`word_wrap` (default **false**, `src/runtime/editor_runtime_internal.h:116`) but
**never passes it to the viewport path** — `viewport()` ignores it
(`src/editor_runtime.cpp:628`). So today long lines wrap even with word wrap off,
which both (a) contradicts the `word_wrap=false` default (Sublime-style: long
lines clip and scroll horizontally, not wrap) and (b) forces `total_visual_rows`
to depend on every line's width, defeating projection.

### Open product decision (needs the user): caret visibility under clipping

If word wrap OFF clips long lines at the pane width and there is **no horizontal
scrolling**, a caret (or a match) positioned past the pane width is not visible —
which contradicts INV-caret-reveal and the spec.md caret-reveal invariant ("every
displaying view minimally scrolls to reveal its primary caret"). This must be
resolved before implementation. Options:
- **(A) Add minimal horizontal scrolling** (recommended, correct): the viewport
  model gains a `first_visual_column` (horizontal offset), and reveal keeps the
  caret's cell column within `[first_col, first_col + width)` exactly as vertical
  reveal keeps its row visible. This makes clipping correct but adds a horizontal
  dimension to the viewport/render/hit-test/reveal model (more scope).
- **(B) Accept horizontal caret invisibility** and weaken the caret-reveal
  invariant to *vertical only* when word wrap is off (documented limitation).
  Cheaper, but a real UX regression on long lines.
This spec assumes **(A)** in its invariants and plan; if the user chooses (B),
drop the horizontal-scroll step and weaken INV-caret-reveal accordingly.

### The projection, gated on wrap mode

Make the viewport path honor `word_wrap` and project the no-wrap case:

**Word wrap OFF (the default, and the case that matters for large files):**
one logical line renders as exactly one visual row, clipped to the pane width
(horizontal scroll of clipped long lines is a separate future feature; today the
overflow is simply not drawn, matching the intent of `word_wrap=false`). Then:
- `total_visual_rows` = logical line count = `(count of '\n') + 1` — a single
  cheap O(document) `memchr`-class byte scan, no `compute_cell_run`.
- `line_document_start[first_visual_row + k]` for the visible window comes from
  the same byte scan (record the byte offset where each visible line starts).
- Only the visible logical lines (`[first_visual_row, first_visual_row + rows)`)
  get `compute_cell_run`. Hit targets, visible rows, and the caret's visual row
  (= its logical line index, found by counting '\n' before the caret offset) are
  all exact.

**Word wrap ON:** keep the current exact full computation
(`active_cell_runs` + `compute_viewport`). Correct, and O(document); a wrapped
10 MiB document staying slow is an accepted, documented limitation and a
candidate for a future incremental per-line width cache (Option C, below).

### Mechanism: a windowed viewport builder

New library function in `viewport.cpp` (the single place that owns cell→row
geometry), consumed by the runtime:

```
ViewportViewState compute_viewport_unwrapped(
    std::string_view document_text,
    ViewportDimensions dimensions,
    std::uint32_t requested_first_visual_row,
    int tab_width);
```

It: scans `document_text` once for line boundaries (total line count + the byte
start of each line in the visible window); clamps `first_visual_row` against the
line-count total exactly as `compute_viewport` clamps against its total; runs
`compute_cell_run` only for the visible lines; and assembles the SAME
`ViewportViewState` shape (`visible_rows`, `hit_targets` with document-absolute
offsets, `scrollbar` from the line-count total) that `compute_viewport` produces
for a non-wrapping document. Long lines are clipped at `dimensions.columns`
(cells beyond the width are not emitted as hit targets / visible cells), which is
exactly what `compute_viewport` already does per visible row via
`visible_width = min(span.cell_width, available)` — so for lines that fit, the
output is identical, and for long lines the *visible row* content is identical;
only the extra *wrapped* rows a long line used to spawn disappear (the intended
no-wrap behavior).

The runtime's `viewport()` chooses the path:
```
if (word_wrap) return compute_viewport(active_cell_runs(), dims, first_row);   // exact, O(document)
else           return compute_viewport_unwrapped(active_text(), dims, first_row, tab_width);
```
`active_text()` still materializes the document string (an O(document) memcpy
from the piece tree — cheap relative to segmentation; making it a `string_view`
into the piece tree is a separate optimization, not this spec).

### Alternative considered and rejected for now (Option C: lazy/cached total)

Keep always-wrap, but compute only the visible window exactly on the first
frame and defer/cache the exact `total_visual_rows` (estimating it from the line
count until a post-first-frame pass fills it in, then maintaining it
incrementally on edits). Rejected as the primary approach: it adds an estimate
state, a caching layer, and incremental-maintenance complexity, and it leaves the
`word_wrap=false` default still wrongly wrapping. Honoring `word_wrap` (this
spec) is simpler and fixes the latent inconsistency. Option C remains the future
path for making *word-wrap-ON* huge documents fast.

## Invariants

- **INV-projection-equivalence:** for word wrap OFF, `compute_viewport_unwrapped`
  produces a `ViewportViewState` **field-for-field equal** (via the type's
  `operator==`, not raw object-representation bytes) to
  `compute_viewport(active_cell_runs(document), dims, first_row)` **whenever no
  logical line exceeds `dimensions.columns` cells** (the case where the current
  code also produces one visual row per line). This is the reference oracle.
- **INV-hit-offsets-absolute:** every `CellHitTarget.byte_offset` is
  document-absolute and resolves (via `resolve_document_position`) to the same
  document position the full path yields. (Upholds M8 mouse correctness.)
- **INV-scrollbar-total:** the scrollbar's `total_rows` equals the true visual
  row count for the active wrap mode (line count when off; wrapped count when
  on), so scroll clamping and thumb geometry are correct.
- **INV-caret-reveal:** `reveal_primary_caret` / `revealed_first_row` land the
  caret's visual row identically to today for non-wrapping documents; under
  decision (A), horizontal reveal keeps the caret's cell column within the pane.
- **INV-viewport-bounded-work:** for word wrap OFF, the total number of
  `compute_cell_run` calls to build **and render** one frame is ≤
  `dimensions.rows` (+ the caret's line for reveal), independent of document
  length. This spans viewport, render, AND selection — all three must project.
- **INV-render-projection:** `render()` computes `compute_cell_run` only for the
  logical lines referenced by `viewport.visible_rows` (plus any small fixed set
  it needs), never the whole document.

## Considerations

- **Behavior change (needs visual signoff):** with word wrap OFF, long lines now
  **clip** at the pane width instead of wrapping to multiple rows. This aligns
  with the `word_wrap=false` default and Sublime's model, and is arguably a bug
  fix, but it is user-visible. Horizontal scrolling of clipped lines is a
  separate future feature; until then the overflow is not shown. Requires
  sign-off before commit.
- **Existing tests may encode always-wrap:** any test that opens a document with
  a line longer than the test viewport width and asserts multiple visual rows
  currently relies on always-wrap. Those must be re-classed as word-wrap-ON tests
  (or the fixture shortened). Audit `tests/*viewport*`, `tests/*tui*`,
  `tests/runtime/*presentation*`, and the `wrapped.txt` golden.
- **The wrapped.txt golden** (M8 hit-target fixture) is a wrap case — keep it
  under the word-wrap-ON path.
- **`scroll_fraction` / scrollbar drag** (`src/runtime/presentation.cpp:51`) also
  calls `active_cell_runs()`; it must use the same wrap-gated total so a drag maps
  to the same position the scrollbar thumb shows. Route it through the same seam.
- **Caret reveal off-screen:** `visual_row(caret)` for word wrap OFF is the
  caret's logical line index (count '\n' up to the caret byte offset) — cheap and
  exact; must not fall back to wrapping-based counting.
- **Tab width:** `compute_cell_run` takes the configured tab width; the unwrapped
  path must pass the same tab width the full path uses (`4` today via
  `active_cell_runs`; thread the real setting if it differs).
- **Empty document / final line:** match `compute_viewport`'s edge handling
  (`runs.empty()` → one empty run; a trailing newline yields a final empty line).
  The line scan must produce the same line count as the run-based path for these
  edges.

## Risks and Mitigations

- *Projected path diverges from full path on a subtle case* → the equivalence
  oracle (byte-identical `ViewportViewState` over a randomized/edge fixture
  corpus, for documents whose lines fit the width) makes divergence a test
  failure before it ships.
- *Hit-testing/caret regressions from wrong absolute offsets* →
  INV-hit-offsets-absolute cross-checked against `resolve_document_position`, and
  the existing M8 mouse tests run against the unwrapped path with fitting lines.
- *Silent behavior change (clip vs wrap)* → called out for visual sign-off;
  gated on the user confirming clip-on-no-wrap is desired.
- *Scrollbar drift between thumb and drag* → both derive `total_rows` from the
  same wrap-gated computation.
- *Large scope* → the change is localized to `viewport.cpp` (one new function)
  and the runtime's `viewport()`/`scroll_fraction` seam; wrap-ON path untouched.

## Acceptance (Definition of Done)

- Observable (needs signoff — visual): open a 10 MiB file — first frame is
  effectively instant (no ~2 s stall); scroll, scrollbar drag, and clicking to
  place the caret behave correctly; a file with very long lines clips (does not
  wrap) with word wrap off and wraps with word wrap on.
- Budgets: the runtime first-viewport path for 10 MiB (word wrap off) is under
  the existing 250 ms gate; re-measure the M10-1 startup harness — 10 MiB
  `first_frame` p50 drops from ~1.7 s to single-digit ms.
- Gates: `cmake --build build` clean; `ctest --test-dir build -E
  performance_measurement` green (incl. the equivalence oracle and re-classed
  wrap tests).
- Oracles: below.

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| VP-1 | `compute_viewport_unwrapped(text, dims, first_row, tab_width)` in the library: one-pass line scan for total line count + visible-line byte starts; `compute_cell_run` for visible lines only; assemble `ViewportViewState` (clip long lines at columns; document-absolute hit offsets; scrollbar from line-count total) | `include/ssg/viewport.h`, `src/viewport.cpp`; `tests/test_viewport.cpp` | (a) reference-impl equivalence: over a DETERMINISTIC GENERATED corpus (empty, 1 line, trailing newline, many short lines, exact-width lines, wide/combining/tab lines — all fitting the width) crossed with several `first_visual_row`, dimensions, and tab widths, `compute_viewport_unwrapped` `operator==` `compute_viewport(cell_runs(text),...)`; (b) hand-computed NO-WRAP long-line cases (a line wider than columns): exact clipped visible cells, hit-target count/offsets, `total_rows` = line count, clamping, and toggling wrap on→off | INV-projection-equivalence, INV-hit-offsets-absolute, INV-scrollbar-total |
| VP-2 | Runtime + selection use the wrap-gated path: `viewport()` and `scroll_fraction` call `compute_viewport_unwrapped` when `!word_wrap`; thread wrap mode through `TextModel::viewport_state`/`visual_row`/`visual_column` and vertical/page caret movement (`src/selection.cpp`) so no-wrap movement is by logical line; caret reveal uses logical-line index | `src/editor_runtime.cpp`, `src/runtime/presentation.cpp`, `include/ssg/selection.h`, `src/selection.cpp` | property: with word wrap off, existing runtime snapshot + hit-test + reveal + caret-movement tests pass for fitting-line documents; a long-line movement test asserts down-arrow moves one logical line (not a wrap row) | INV-caret-reveal, INV-viewport-bounded-work |
| VP-R | Project render: `render()` computes `compute_cell_run` only for the logical lines in `viewport.visible_rows`, not the whole document (`logical_lines` becomes windowed, keyed off the viewport) | `src/render.cpp`; `tests/test_render.cpp` | property: a render-side `compute_cell_run` counter (test hook) is ≤ visible rows for a tall document; the rendered grid for a fitting-line document is unchanged vs. today (existing render goldens green) | INV-render-projection, INV-viewport-bounded-work |
| VP-H | (Decision A) Minimal horizontal scrolling: `first_visual_column` in the viewport model; reveal keeps the caret column in `[first_col, first_col+width)`; render/hit-test honor the horizontal offset | `include/ssg/viewport.h`, `src/viewport.cpp`, `src/render.cpp`, `src/selection.cpp`, `src/editor_runtime.cpp`; tests | hand cases: caret past the pane width scrolls horizontally so its cell is visible; hit-testing accounts for the horizontal offset; a fitting-line document has `first_visual_column==0` and is unchanged | INV-caret-reveal | 
| VP-3 | Re-class always-wrap tests + goldens under word-wrap-ON; add a word-wrap-ON regression that long lines still wrap; re-measure the 10 MiB first frame | named files: `tests/test_viewport.cpp`, `tests/test_render.cpp`, `tests/test_tui_fixture.cpp`, `tests/runtime/test_runtime_presentation.cpp`, `tests/fixtures/render/wrapped.txt`, `benchmarks/startup_benchmark.cpp` | the startup harness reports 10 MiB `first_frame` p50 in single-digit ms; a wrap-ON test still yields multiple visual rows for a long line | INV-scrollbar-total |

Instrumentation note: the `≤ rows` cell-run counters (VP-R, VP-2) are a
test-only mechanism (a thread-local or an injected counter compiled in only under
a test flag), NOT production state, so a full-document implementation cannot pass
the count oracle.

## Rationale (skippable)

The measured M10 result showed the 10 MiB first frame is dominated not by
deferrable enrichment but by the per-line `compute_cell_run` scan the snapshot
runs over the whole document — because the viewport computes exact wrapped
geometry, which needs every line's width. The clean fix is not to cache or
estimate that geometry but to stop computing it where it is not needed: with word
wrap off (the default), one line is one row, the total is a byte-cheap line
count, and only the visible lines need segmentation. This also surfaces and fixes
a latent inconsistency — `word_wrap=false` currently still wraps — so the change
both makes large files fast and makes the wrap setting mean what it says. The
word-wrap-ON path, where wrapped geometry genuinely needs all lines, is left
exact and identified as the future home of an incremental width cache. The
equivalence oracle (projected == full, byte-for-byte, for fitting lines) keeps the
hot-path change safe by construction.
