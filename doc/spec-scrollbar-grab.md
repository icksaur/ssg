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
