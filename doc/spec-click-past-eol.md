# spec-click-past-eol — Click / drag past end-of-line places the caret at line end

Status: PLANNED. A focused follow-up to Milestone 8 (mouse). Changes a documented
M8 limitation: today a pointer cell past a line's content (or on a blank line) has
no document position, so clicking there does nothing.

## Goal

Clicking on an editor row to the right of a line's content places the caret at the
**end of that line** (after the last character, before the newline). This works for
both cursor placement and drag-selection, and for **blank lines** (a line that is
only a newline): clicking or dragging from the empty area after the newline selects
from the end of that line.

- A left click past a line's content → caret at that line's end.
- A drag that starts on or passes over a past-content / blank-line cell → the
  selection extends to that line's end (no longer "dispatches nothing").
- An actual document cell still resolves to its exact byte span (no regression).

Non-goals: clicking in the empty area BELOW the last line of the document (rows with
no line at all) — see the Decision below. Virtual whitespace / past-EOL cursor
columns (caret sitting visually past the newline) — the caret still lands on a real
document position (the line-end offset).

## Current state (facts)

- `editor_hit` (`src/hit_test.cpp`) matches a click to an **exact**
  `(viewport_row, viewport_column)` entry in `viewport.hit_targets`. A cell past a
  short line's content, or any cell on a blank row, has no matching target, so it
  returns `HitRegion::none`. This is the documented M8 behavior (spec-m8.md M8-S:
  "A drag over a cell with no document target (past a short line's end, a blank
  row, ...) dispatches nothing").
- `route_pointer` (`apps/pointer_routing.cpp`) turns a `HitRegion::editor` hit
  (with the caller-resolved `document_position`) into `cursor.set_position` on
  press (+ begins a drag) and `select.set_range{anchor, active}` on drag. A `none`
  hit dispatches nothing. **So once `hit_test` returns an editor hit at the line
  end, the existing routing already does caret placement AND drag-selection with no
  app/routing change.**
- The app resolves `targets.document_position` from `hit.byte_offset` via
  `resolve_document_position(text, ByteOffset{hit.byte_offset})`; `hit.byte_len`
  does not affect caret placement.
- `VisualRow` (`include/ssg/viewport.h`) carries `logical_line`, span indices, and
  cell counts, but **no document byte offset**. `hit_targets` carry
  document-absolute byte offsets for real cells only; a blank row has none.
- `test_hit_test.cpp:97-99` asserts the current behavior ("A cell far past the end
  of the short first line has no document position" → `HitRegion::none`) and must
  be re-expected.

## Design

### Root cause

`editor_hit` only recognizes exact cell targets. Past-content and blank-row cells
have no target, so the caret can never be placed there. The fix is a library-side
clamp in `editor_hit`: a click on a valid editor content row that misses every cell
resolves to the **end of that visual row's line**.

### The missing datum: per-row end offset

To place the caret at the line end, `editor_hit` needs the document byte offset of
the row's end. For a non-empty row this equals the last cell's `byte_offset +
byte_len`, but a **blank row has no hit targets at all**, so that offset cannot be
recovered from `hit_targets`. Recomputing it by scanning the document text for the
line's start is O(document) per click — which Milestone 12 specifically eliminated
from the hot path; a click is human-paced, but re-introducing an O(document) scan
on a 10 MiB file is an avoidable regression and the library already knows the
answer.

So the viewport publishes it. Add to `VisualRow`:

```
uint32_t end_byte_offset;  // document-absolute byte offset of the row's end
```

- **Meaning:** the document position the caret takes when a click lands at or past
  the row's last content cell — the **end of the VISUAL ROW**, which is not always
  the logical end-of-line. For the FINAL visual row of a logical line it is the
  logical EOL (the newline byte, or `text.size()` for the last line with no
  trailing newline). For an INTERIOR wrapped row (word wrap ON) it is that row's
  **wrap boundary** (mid-logical-line), so clicking past a wrapped row targets the
  wrap point, not the far EOL. For an EMPTY line it is the line's start offset
  (start == end). Under word wrap OFF every visual row IS a full logical line, so
  the visual-row end always equals the logical EOL — the case in the user's
  request. It is always a valid document position (resolvable by
  `resolve_document_position`).
- **Populated by both viewport builders:**
  - `compute_viewport` (wrapped): the row's end = `line_document_start[logical_line]
    + (last content byte of the row within the line)`. The function already tracks
    `line_document_start[]` and each row's spans; the row end is
    `line_document_start[row.logical_line] + span.byte_offset + span.byte_len` of
    the row's last span, or `line_document_start[row.logical_line]` when the row is
    empty.
  - `compute_viewport_unwrapped`: the row's end = `document_start + (end - start)`
    where `start`/`end` are the line's byte range already computed in the loop
    (`end` is the newline byte, or `text.size()` for the last line). Equivalently
    `end_byte_offset = checked_u32(end)`. This is the line's TRUE end, computed
    from the full line and **independent of the horizontal offset / clipping**
    (M12 VP-H): clicking past the visible (clipped) content still targets the real
    line end. For an empty line `start == end`, so `end_byte_offset == document_start`.

This makes `editor_hit`'s clamp O(1) and keeps hit offsets document-absolute
(INV-hit-offsets-absolute).

### The clamp in editor_hit

```
RegionHit editor_hit(snapshot, content, column, row):
    viewport_row = row - content.y
    viewport_column = column - content.x
    # exact cell (unchanged)
    for target in viewport.hit_targets:
        if target matches (viewport_row, viewport_column): return editor hit(target)
    # NEW: a valid editor row past its content -> caret at the visual row's end
    if viewport_row < viewport.visible_rows.size():
        row = viewport.visible_rows[viewport_row]
        return editor hit { byte_offset = row.end_byte_offset, byte_len = 0 }
    # NEW (Decision B): a row BELOW the last line -> end of the last visible row,
    # so clicking/dragging in the empty area below the text reaches the last line.
    if not viewport.visible_rows.empty():
        return editor hit { byte_offset = visible_rows.back().end_byte_offset,
                            byte_len = 0 }
    return none   # empty viewport (no visible rows at all)
```

`byte_len = 0` (a zero-width end-of-line position); routing resolves the caret from
`byte_offset`, so drag-select and cursor placement both work via the existing
`route_pointer` paths with no app change.

### Decision (RESOLVED by the user): clicks BELOW the last line

A click on a pane row with **no line** (`viewport_row >= visible_rows.size()`, e.g.
a 3-line file in a 24-row pane) clamps to the **end of the last visible row**
(`visible_rows.back().end_byte_offset`) — **Decision B**. Clicking anywhere in the
empty area below the text places the caret at the end of the last line, so
dragging from below the text upward selects to the end of the document. (When the
pane has no visible rows at all — an empty viewport — the click returns `none`.)
Decision A (return `none` below the document) is rejected: the user wants
below-text clicks/drags to reach the last character for easy selection in small
files.

## Invariants

- **INV-click-reaches-eol:** a left click on any editor content row that hosts a
  visual row, at or past that row's content, resolves to `HitRegion::editor` at the
  **visual row's end position** — the logical EOL for a final/unwrapped row, the
  wrap boundary for an interior wrapped row, the line offset for an empty line — for
  non-empty AND empty (newline-only) rows. Under word wrap OFF (the default, and the
  request's case) this is always the logical end-of-line. Cursor placement and
  drag-selection therefore both reach the end of the row.
- **INV-hit-offsets-absolute (upheld):** `VisualRow.end_byte_offset` is
  document-absolute and resolves via `resolve_document_position` to the same
  end-of-line `DocumentPosition` the full document model yields.
- **INV-exact-cell-unchanged:** a click on a real document cell still returns that
  cell's exact `byte_offset`/`byte_len` (the exact-match path is untouched), so no
  existing click/drag behavior regresses.
- **INV-viewport-bounded (upheld):** the clamp is O(1) (a single `visible_rows`
  index); no per-click document scan is introduced.

## Considerations

- **Wire change:** `VisualRow` gains one `uint32_t`; update the protocol codec
  (`to_value`/`decode_present` for `VisualRow`, `src/protocol.cpp`) and regenerate
  the canonical `session_snapshot.hex` / `session_delta.hex` goldens
  (`SSG_REGEN_PROTOCOL_FIXTURES=1 ./build/test_protocol`).
- **Aggregate initializers:** every `VisualRow{...}` construction (viewport
  builders, and any test that builds a `VisualRow` positionally) gains the field.
- **Existing test to re-expect:** `test_hit_test.cpp:97-99` currently asserts
  past-content → `none`; it becomes an editor hit at the line-end offset.
- **Horizontal scroll (M12 VP-H):** under word wrap off with a horizontal offset,
  a visual row's cells start at `start_cell`; `end_byte_offset` is the line's true
  end regardless of horizontal offset (clicking past the visible content still
  targets the line end). Confirm the unwrapped builder sets `end_byte_offset` from
  the full line, not the clipped window.
- **Selection semantics:** the user says "the last character of the line is
  chosen." For a caret click this is the end-of-line caret (after the last char).
  For a drag, `select.set_range{anchor, line-end}` selects up to the line end
  (including the last character when dragging rightward past it). This matches the
  request; the caret sits at the line-end position, not on a virtual past-EOL cell.

## Risks and mitigations

- *Wrong offset on wrapped interior rows* → `end_byte_offset` is per VISUAL row, so
  an interior wrapped row clamps to its wrap boundary, not the logical line end; the
  reference-oracle test covers a wrapped long line under word-wrap-ON.
- *Empty-line offset off by one* → the reference oracle compares `end_byte_offset`
  to `resolve_document_position` of the hand-computed line start/end for empty,
  single-char, and multi-line fixtures.
- *Wire/goldens drift* → regenerate via the existing env-gated path; the protocol
  round-trip test covers the new field.

## Acceptance (Definition of Done)

- Observable (real binary): open a file with a short line and a blank line; click
  to the right of the short line → caret at its end; click on the blank line → caret
  on the blank line; drag from the blank line rightward/down → selection extends
  from the blank line's end. A click on an actual character still lands exactly.
- Gates: `cmake --build build` clean; `ctest --test-dir build -E
  performance_measurement` green (incl. the re-expected M8 test, the new hit-test
  cases, and the protocol round-trip).

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| CE-1 | Add `VisualRow.end_byte_offset` (both viewport builders + protocol codec + regenerated goldens + aggregate sites); update the `hit_test.h`/`RegionHit` doc contract to note editor hits may be zero-width EOL positions; clamp past-content/blank-row clicks in `editor_hit` to it (Decision B: rows below the last line clamp to the last visible row's end; an empty viewport stays `none`) | `include/ssg/viewport.h`, `src/viewport.cpp`, `include/ssg/hit_test.h`, `src/hit_test.cpp`, `src/protocol.cpp`, `tests/fixtures/protocol/*.hex`, aggregate sites in tests | **reference oracle over BOTH builders**: for a fixture with (a) a short non-empty line, (b) a blank newline-only line, (c) a wide/multi-cell line, (d) a final line with NO trailing newline, and (e) — via `compute_viewport` with real columns — a wrapped long line with an interior AND a final wrapped row, assert `end_byte_offset` per visible row equals the hand-computed row-end byte and that `resolve_document_position(text, end_byte_offset)` is the expected position; **and via `compute_viewport_unwrapped` with a nonzero `first_visual_column` (M12 horizontal offset)**, assert `end_byte_offset` is the FULL line's true end (unchanged by clipping). hit-test cases: a click past a short line → editor hit at its end; a click on a blank line → editor hit at the blank line offset; an exact cell → unchanged exact span; a pane row BELOW `visible_rows` → editor hit at the LAST visible row's end (locks Decision B); the old `test_hit_test.cpp` past-end assertion re-expected | INV-click-reaches-eol, INV-hit-offsets-absolute, INV-exact-cell-unchanged, INV-viewport-bounded |
| CE-2 | Drag + integration: `route_pointer` press-then-drag starting on a blank-line/past-content cell; a reproducible HEADLESS integration test that a click past EOL resolves through `hit_test` → `resolve_document_position` → `cursor.set_position` and lands the caret at the line end | `tests/test_ssg_app.cpp` (route_pointer), `tests/test_hit_test.cpp` or `tests/runtime/*` (integration) | `route_pointer` unit: a press on an editor hit at a line-end offset returns `cursor.set_position` + `begins_drag`; a subsequent drag returns `select.set_range{anchor, line-end}`; **integration (headless, reproducible)**: build a production snapshot, `hit_test` at (past-content column, a short line's row) and (a blank line's row), feed `hit.byte_offset` through `resolve_document_position` + dispatch `cursor.set_position`, take a fresh snapshot and assert the primary caret is at that line's end offset. (A real-binary pty click is OPTIONAL manual acceptance, not part of the gate: inject `ESC[<0;col;rowM`/`m` and observe the rendered caret.) | INV-click-reaches-eol |
