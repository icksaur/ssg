# spec-scrollbar-grab

Status: draft (design + plan for review)

## Goals

Make scrollbar dragging behave like a real scrollbar:

- **Grab the thumb** and it moves *with the cursor at a fixed offset*: the point
  of the thumb you grabbed stays under the pointer for the whole drag, so a short
  mouse travel can move the thumb its full range (dragging the thumb to the
  bottom no longer requires dragging the mouse to the very bottom of the view).
- **Click in the well** (the track, not on the thumb) jumps the thumb to the
  cursor and then behaves as if the thumb were grabbed at its centre, so a
  continued drag keeps tracking.
- Applies uniformly to every scrollable gutter (editor, tree panel, palette),
  which already share one gutter-drag path.

## Design

### The bug

`HitTester::scrollbarHit` (`src/HitTester.cpp:16-27`) turns a gutter press into a
scroll fraction of `relative / (gutter.height - 1)`, where `relative` is the
pointer's row *within the gutter*. The app loop re-runs this on **every** drag
motion (`apps/ssg_main.cpp:~1263`, `inGutter(...)`), so the fraction always
equals the pointer's absolute position in the track. The thumb therefore snaps
its top (or the mapped point) to the cursor, and reaching the document bottom
requires the cursor at the track bottom. There is no notion of *where on the
thumb* the grab happened.

### The model: grab offset against the thumb travel

The scrollbar's thumb has `thumbStart` (its top row within the gutter) and
`thumbSize` (its height in rows), already published in `ViewportViewState::
scrollbar` (`ScrollbarMetrics`, `include/ssg/Viewport.h`) and mirrored for the
tree and palette. The scrollable range of the thumb's TOP is `travel =
gutterHeight - thumbSize`; a fraction `f in [0,1]` maps to `firstRow = round(f *
maximumFirstRow)` server-side (unchanged).

On **press**, the client computes a `grabOffset` — the distance from the thumb's
top to the grabbed point — and remembers it for the whole gesture:

- Press **on the thumb** (`thumbStart <= rel < thumbStart + thumbSize`):
  `grabOffset = rel - thumbStart` (grab in place; the thumb does not jump).
- Press **in the well**: the thumb jumps to centre on the cursor, so
  `grabOffset = thumbSize / 2` (a "jump to here", after which a continued drag
  tracks from the centre).

where `rel = pointerRow - gutter.y`.

On **press and every drag motion**, the desired thumb top is `desiredTop =
clamp(rel - grabOffset, 0, travel)` and the scroll fraction is `desiredTop /
travel` (or `0` when `travel == 0`, i.e. the thumb fills the gutter and nothing
scrolls). This is the whole fix: the fraction is derived from the thumb's target
position minus the grab offset, not from the raw pointer row.

### Where the change lives

- **The press and the drag share one client-side fraction computation.** Both a
  gutter press and every drag motion compute the fraction from the grab state and
  the region's published thumb geometry; `HitTester::scrollbarHit`'s
  absolute-row→fraction mapping is no longer the source of the gutter fraction.
  Because `route_pointer`/`gutterScroll` consume `hit.scrollNumerator/
  Denominator` for BOTH the press branch (`pointer_routing.cpp:~88`) and the drag
  branch (`~150`), the app loop must compute the grab fraction and populate those
  fields BEFORE calling `route_pointer` — the grab state is therefore captured on
  the press *before* routing, not after (today `draggingGutter` is set after
  dispatch, which is too late for the new model). A press on the thumb is a no-op
  (`desiredTop == thumbStart`), so press-in-place does not jump; a well-press
  jumps (grabOffset = thumbSize/2). The press and the first drag motion run the
  identical computation, so there is no jump on the first move.
- **`travel` and the thumb come from `ScrollbarMetrics`, not the gutter Rect.**
  The server computes `thumbStart`/`firstRow` against `ScrollbarMetrics.
  viewportRows` (`Viewport.cpp` `scrollbarMetricsImpl`, `toFraction`), so the
  client must use the SAME basis: `travel = metrics.viewportRows -
  metrics.thumbSize`, with `thumbStart`/`thumbSize` from that metrics struct. The
  gutter Rect is used ONLY for its `y` (to turn the absolute pointer row into
  `rel = pointerRow - gutter.y`). This binds client and server to one basis
  instead of the implicit "Rect height == viewportRows" coupling.
- **Per-region metrics need a switch mirroring `HitTester::inGutter`.** The
  metrics for the hit region live in three places — editor
  `client().viewport.scrollbar` + `panes.front().scrollbar` rect; tree
  `sections().tree...scrollbar` + `panelScrollbar`; palette `palette->scrollbar`
  + `palette->scrollbarRect`. Step 2 reads them via a `switch(region)` kept
  beside `inGutter` so the two cannot drift (INV-gutter-uniform).
- **`draggingGutter` gains the gesture state**: a struct holding the region,
  `gutter.y`, `travel`, `thumbSize`, and `grabOffset`, captured at press and
  consulted on each drag motion instead of re-hit-testing the absolute row.
- The **server side is untouched**: it still receives a `numerator/denominator`
  fraction and converts it to a first row. The round-trip `desiredTop -> fraction
  -> server firstRow -> next-frame thumbStart` is a stable fixed point up to a
  static ±1 row (integer thumb quantisation, verified by sweep); it does not
  creep under a held-still cursor because the client derives the fraction purely
  from `(rel, grabOffset, travel)`, never reading the server thumb back.

The mechanism (grab offset + travel) is chosen over sending a raw `firstRow`
because the fraction API already exists end-to-end and switching the wire would
touch both server handlers for no correctness gain (the ≤1-row quantisation is
inherent to a thumb drag regardless of the wire type).

## Invariants

- **INV-gutter-uniform**: editor, tree, and palette gutters share one drag code
  path; the fix must not fork per surface.
- **INV-fraction-wire**: the client→server scroll contract stays a
  `numerator/denominator` fraction; the change is only in how the client derives
  it.
- **INV-drag-horizontal-tolerance**: once a gutter drag starts, the pointer may
  wander off the one-column gutter horizontally without dropping the drag
  (existing behaviour, `apps/ssg_main.cpp` comment) — the grab-offset gesture must
  preserve it (classify by held region, not by re-hit-testing the column).

## Considerations

- **`travel == 0`**: a document that fits the viewport has a full-height thumb and
  no scroll range; a press must be a no-op (fraction 0), never a divide-by-zero.
- **Clamping**: `desiredTop` clamps to `[0, travel]`, so grabbing near the thumb's
  bottom and dragging up/down still bottoms/tops out correctly.
- **Well-jump direction**: centring the thumb on the cursor (`grabOffset =
  thumbSize/2`) makes a well-click land the thumb where clicked and read
  identically whether the user then holds still or drags — simpler and less
  surprising than paging by a viewport.
- **Stale geometry across the drag**: the thumb SIZE can change only if the
  document or viewport changes mid-drag (it does not during a pointer drag), so
  capturing `travel`/`grabOffset` once at press is correct for the gesture.
- **Press vs. drag parity**: the initial press already applies the same
  `desiredTop` computation, so a click and the first drag motion agree (no jump
  on the first move).

## Risks and Mitigations

- *Grab offset captured from the wrong surface's metrics.* — The press resolves
  the region via the existing hit test; the metrics are read from that same
  region's scrollbar in the snapshot. An oracle checks each surface.
- *Off-by-one between thumb rows and gutter rows.* — `rel = pointerRow -
  gutter.y` is the same normalisation the current code uses; the oracle pins
  exact `(thumbStart, thumbSize, pointerRow, grabOffset) -> fraction` cases.
- *Regression of the horizontal-tolerance behaviour.* — Kept: the drag still
  classifies by the held region, not by re-hit-testing.

## Acceptance (Definition of Done)

- Observable: grabbing the thumb near its top and dragging down by a few rows
  moves the thumb (and document) proportionally to the drag, and the thumb
  reaches the bottom when the *grabbed point* reaches the track bottom — not when
  the cursor does. A well-click jumps the thumb under the cursor. Visual signoff
  required (interactive).
- Budgets: n/a (no new per-frame cost; one small struct on press).
- Gates: `bash scripts/check.sh` green (build + suites, 0 warnings, -Werror).
- Oracles: a pure unit test of the fraction function over a table of
  `(gutterHeight, thumbStart, thumbSize, pressRow, dragRow) -> fraction`,
  covering grab-in-place, well-jump, top/bottom clamp, and `travel==0`.

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Add pure helpers: `scrollbarGrabOffset(rel, thumbStart, thumbSize)` (in-place if on thumb, else thumbSize/2) and `gutterFraction(rel, grabOffset, travel)` -> `{numerator=clamp(rel-grabOffset,0,travel), denominator=travel}` (0 when travel==0) | apps/pointer_routing.{h,cpp}, tests/test_ssg_app.cpp | ref-table over (viewportRows, thumbStart, thumbSize, pressRow, dragRow) -> fraction: grab-in-place, well-jump-centre, top/bottom clamp, travel==0 | INV-fraction-wire |
| 2 | Replace `draggingGutter` (region-only) with {region, gutterY, travel, thumbSize, grabOffset}; capture at press via a `switch(region)` reading that region's ScrollbarMetrics (viewportRows/thumbStart/thumbSize) + gutter rect y, kept beside `HitTester::inGutter` | apps/ssg_main.cpp | app-loop: press on the thumb records grabOffset = pressRow - thumbStart | INV-gutter-uniform |
| 3 | Compute the fraction from the grab state for BOTH the press and each drag motion, populating `hit.scrollNumerator/Denominator` before `route_pointer` runs; a press on the thumb is a no-op, a well-press jumps | apps/ssg_main.cpp | app-loop: dragging the grabbed thumb down N rows moves firstRow proportionally; the thumb bottoms out when the grabbed point reaches the track bottom, not when the cursor does | INV-drag-horizontal-tolerance |
| 4 | Retire the absolute-row fraction: drop the unread `scrollbarFraction` double; decide `scrollbarHit`/`inGutter`'s fate (drag no longer re-hit-tests) and update `tests/test_hit_test.cpp`, which currently pins the absolute-row numerator/denominator | src/HitTester.{h,cpp}, tests/test_hit_test.cpp, apps/ | grep: no path derives a drag/press fraction from the absolute row; test_hit_test reflects the new model | INV-gutter-uniform |

## Rationale (optional, skippable)

A scrollbar is a direct-manipulation control: the invariant a user expects is
"the pixel I grabbed stays under my finger." The current code implements a
different control — an absolute position slider where the track maps linearly to
the document — which is why the thumb can only reach the bottom when the cursor
does. The fix is not new machinery but the missing piece of state: the offset
between the grab point and the thumb top, which converts an absolute mapping into
a relative one. Everything else (thumb geometry, the fraction wire, the
server-side conversion) already exists.

---

## Amendment (backlog item 7): the thumb skips gutter rows and the top is unreliable

**Symptoms reported.** Dragging the thumb, the glyph "jumps over certain rows
(never rendering on that row), depending on where I start." The top of the
scrollbar does NOT reliably scroll to the first line, though the bottom reliably
reaches the last line. The document scroll is close but wrong.

**Root cause (measured).** The two directions of the scrollbar mapping both use
integer FLOOR, which is asymmetric:

- Render: `thumbStart = floor(firstRow · travel / maximumFirstRow)`
  (`Viewport.cpp` `scrollbarMetricsImpl`), where `travel = viewportRows -
  thumbSize` and `maximumFirstRow = totalRows - viewportRows`.
- Drag inverse: `firstRow = floor(maximumFirstRow · numerator / denominator)`
  (`ScrollOffset::toFraction`), with the gutter gesture sending `numerator =
  desiredTop` (the grabbed thumb-top row), `denominator = travel`.

When `maximumFirstRow > travel` (any document more than ~2× the viewport — the
common case, and always so for a one-row thumb), floor makes the round-trip
`thumbTop → firstRow → thumbTop` LOSE the top rows: I measured, across regimes
(total,V) ∈ {(200,10),(1000,30),(25,20),…}, that `travel` of the `travel+1`
gutter rows are reachable — one row (the topmost band) never renders, and 3–28
interior rows are skipped. Floor biases every conversion downward, so the bottom
(where the clamp pins `firstRow = maximumFirstRow`) looks correct while the top
and middle drift. That is exactly the reported asymmetry.

**Fix — symmetric rounding, via one shared primitive.** Replace floor with
round-half-up in BOTH directions, expressed as a single pure library helper so
the two conversions cannot use different rounding:

```
scrollScaleRounded(value, numerator, denominator)  // round(value·num/den); den==0 → 0
  = (value · numerator + denominator/2) / denominator   (all uint64)
```

- Render: `thumbStart = scrollScaleRounded(firstRow, travel, maximumFirstRow)`.
- Drag inverse: `firstRow = min(maximumFirstRow, scrollScaleRounded(
  maximumFirstRow, numerator, denominator))`.

No new wire, no anchors, no per-gesture state. The client keeps sending the plain
`desiredTop / travel` grab fraction (`apps/pointer_routing.cpp` `gutter_fraction`,
unchanged). Measured with symmetric round: **every** gutter row is reachable
(`skips = 0`, all `travel+1` rows render), the top cell maps to `firstRow = 0`,
and the bottom cell maps to `firstRow = maximumFirstRow`, in every regime tested.

This supersedes an earlier (unmerged) attempt that added an anchor-delta model
(`gutter_first_row`, published `firstRow`/`maximumFirstRow` on `GutterThumb`,
gesture anchoring). That was over-complex and did not address the skipped-row
rendering, which lives in the render direction, not the drag. It is discarded.

### Amendment invariants

- **INV-thumb-surjective**: for a fixed `(total, viewport)`, every gutter thumb
  row `t ∈ [0, travel]` is the rendered `thumbStart` of some `firstRow` — the
  thumb can sit on any row it is dragged to (no skipped rows). This holds because
  in this scrollbar model `maximumFirstRow ≥ travel` always (`travel =
  viewportRows - thumbSize ≤ viewportRows ≤ totalRows - viewportRows =
  maximumFirstRow` whenever anything scrolls), so the drag inverse expands and the
  render compresses by the same ratio; symmetric round-half-up then makes
  `scrollThumbStart(scrollFirstRow(t)) == t`. Verified exhaustively for all `T ∈
  [1,80]`, `M ∈ [T,3000]` (~9.8M cases, zero failures).
- **INV-endpoints**: `thumbTop = 0 → firstRow = 0` (top line shown) and `thumbTop
  = travel → firstRow = maximumFirstRow` (last line shown); symmetrically
  `firstRow = 0 → thumbStart = 0`, `firstRow = maximumFirstRow → thumbStart =
  travel`.
- **INV-thumb-fits**: `thumbStart + thumbSize ≤ viewportRows` (unchanged; round
  cannot exceed `travel` because the input is capped at `maximumFirstRow`).

### Amendment oracles (pure, isolated — the user's explicit ask)

Unit-test the two conversions as an isolated matched pair (no runtime, no app),
sweeping `(total, viewport)` across regimes {(200,10),(1000,30),(100,20),(40,20),
(25,20),(57,13),(500,50),(80,79)}:

- **No skipped rows / thumb surjective**: for every `thumbTop ∈ [0, travel]`,
  `scrollThumbStart(scrollFirstRow(thumbTop)) == thumbTop`.
- **Endpoints**: `scrollFirstRow(0) == 0`, `scrollFirstRow(travel) ==
  maximumFirstRow`; `scrollThumbStart(0) == 0`, `scrollThumbStart(
  maximumFirstRow) == travel`.
- **Monotonic**: both conversions are non-decreasing in their input (no backward
  jump mid-drag).
- **Fits**: for every `firstRow ∈ [0, maximumFirstRow]`, `thumbStart + thumbSize
  ≤ viewportRows`.
- **Degenerate**: `travel == 0` (document fits, or thumb fills the gutter) →
  `firstRow == 0`, no divide-by-zero.
- **Shared-semantics (non-editor path)**: because the rounding lives in the
  shared `ScrollOffset::toFraction`, one oracle exercises a tree/palette-shaped
  fraction (a list scrollbar, not the editor) to confirm the rounded mapping is
  intentional and uniform across every surface, not editor-only.

### Amendment plan

Note: this branch was reset to `master` before implementing, so no artifacts of
the discarded anchor-delta attempt (`gutter_first_row`, anchored `GutterDrag`
state, extra `GutterThumb` fields) exist to roll back — the starting point is the
plain `gutter_fraction` drag. The change below is purely additive/`floor→round`.

| # | Step | Files | Oracle |
|---|------|-------|--------|
| B1 | Add pure `scrollScaleRounded(value, num, den)` (round-half-up, den==0→0) and thin wrappers `scrollThumbStart(firstRow, maximumFirstRow, travel)` / `scrollFirstRow(thumbTop, travel, maximumFirstRow)` in the library; unit-test in isolation | `include/ssg/Viewport.h`, `src/Viewport.cpp`, `tests/test_viewport.cpp` | the amendment oracles above |
| B2 | Route `scrollbarMetricsImpl` (thumbStart) and `ScrollOffset::toFraction` (firstRow) through the shared primitive; regenerate the ui_layout + protocol goldens (thumbStart values change) | `src/Viewport.cpp`, `tests/fixtures/**` | existing viewport/render suites stay green; goldens reflect rounded thumbStart |
