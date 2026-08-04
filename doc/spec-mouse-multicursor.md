# Spec: mouse multi-cursor (Alt+click / Alt+drag)

## Problem / goal

SSG has multi-cursor editing (Alt+D add-next-occurrence, Alt+J/K add-cursor,
Alt+I split) but no way to place cursors with the mouse. Sublime/VS Code use
Ctrl/Cmd+click; SSG cannot, because terminals hijack Ctrl (link-following) and
Shift (native selection) for the mouse. **Alt** is free and is SSG's modifier
for every multi-cursor command already, so:

- **Alt+left-click** adds a caret at the clicked cell (ADDS to the selection
  set; does not replace it).
- **Alt+left-drag** adds a RANGE from the press cell to the current cell (ADDS;
  the other cursors are preserved and the new range extends live as you drag).

A plain (no-Alt) click/drag keeps today's behavior exactly: it collapses to a
single caret / single selection.

## Background (verified in code)

- SGR mouse decode (`apps/ssg_terminal.cpp`, the `'<'` case ~line 1072) builds a
  `PointerEvent` from the Cb button byte. The modifier bits in Cb are bit2=Shift
  (4), bit3=**Alt/Meta (8)**, bit4=Ctrl (16). Bit3 is currently discarded.
- `PointerEvent` (`apps/ssg_terminal.h`) has column/row/button/kind — no
  modifier field.
- `route_pointer` (`apps/pointer_routing.cpp`) is the pure router. On an editor
  left **press** it emits `cursor.set_position` and sets `begins_drag`; on a
  **drag** it emits `select.set_range` (anchor→active); release ends the drag.
  Both `cursor.set_position` and `select.set_range` REPLACE the whole selection
  set with a single selection.
- Library selection commands (`src/Selection.cpp`): `select.set_range` replaces
  the set with one range; `select.add_range` PUSHES a range onto the set. Both
  take `SelectionCommandArguments{ selection }`. The `SelectionSet` constructor
  SORTS by lower byte-offset and MERGES overlapping/identical ranges;
  `primary()` returns the highest-positioned selection (`.back()`). A collapsed
  range (anchor == active) is a bare caret.

## Design

### 1. Carry the Alt modifier on the pointer event

- `PointerEvent` gains `bool alt = false`.
- The SGR decoder sets `event.alt = (cb & 8) != 0` in the `'<'` mouse case.
  (Only Alt is surfaced; Shift/Ctrl remain unused by SSG's mouse and stay
  discarded, so no behavior rides on them.)
- The legacy X10 mouse path (`ESC [ M b x y`) does not carry usable modifier
  bits in this build and is left as-is (it only ever produced wheel scroll).

### 2. Thread Alt into the pure router

`route_pointer` gains a `bool alt` parameter (placed next to `button`/`kind`).
The app passes `event.alt`. Pure and unit-testable as today.

### 3. Alt+click adds a caret

On an editor left **press** with `alt == true` and a resolved
`document_position`:
- dispatch `select.add_range` with a COLLAPSED selection
  `Selection{pos, pos}` (adds a caret), instead of `cursor.set_position`.
- set `begins_drag = true` and mark the drag as an **Alt-drag** (see §4) so a
  subsequent motion extends the new caret rather than replacing the set.

A no-Alt press is unchanged (`cursor.set_position`, normal drag).

Idempotence / de-dup: `add_range` of a caret already in the set merges to a
no-op (the set de-dups identical selections), so Alt+clicking the same cell
twice does not create duplicates. (Alt+click to REMOVE an existing caret — the
Sublime toggle — is an explicit non-goal here; see below.)

### 4. Alt+drag extends by full-recompute from a client-held baseline

The hard part: a drag motion must update ONLY the just-added selection while
preserving the others, but `select.set_range` replaces the whole set. Rather
than track "the added selection" by a fragile post-normalization index (which
goes stale the moment the `SelectionSet` ctor re-sorts/merges — e.g. dragging
the new caret upward past an existing one), the drag is **recomputed in full**
every motion from a stable baseline, so there is no target identity to track:

**New library command `select.set_ranges`** — replaces the whole selection set
with an explicit LIST of ranges, then normalizes (sort + merge) like every other
mutation. It is the natural plural of `select.set_range` and the only new
primitive needed. `SelectionCommandArguments` gains a
`std::vector<Selection> selections` field used by this command (round-tripped in
the Protocol codec); the existing single-`selection` commands are unchanged.

Client drag state (in `apps/ssg_main.cpp`, alongside the existing
`dragging`/`drag_anchor`): on an **Alt-press** the app captures
`alt_drag_baseline` = the current selection set from the snapshot's selection
section, plus `drag_anchor = pos`, and sets `alt_drag = true`. The router then
drives:

- **Alt-press**: `select.add_range{collapsed pos}` (adds the caret immediately,
  so a plain Alt+click that never moves already reads as an added caret) +
  `begins_drag`.
- **Alt-drag motion** over an editor cell with a target: the app dispatches
  `select.set_ranges` with `alt_drag_baseline + Selection{drag_anchor, active}`.
  Because the set is rebuilt from the immutable baseline each time, dragging in
  any direction (including upward past existing cursors) is always correct; the
  baseline cursors never move.
- **Release**: ends the drag; clears `alt_drag`/`alt_drag_baseline`. No command.

The router (`route_pointer`, pure) still decides the COMMAND per event from an
`alt` flag, but the drag's Alt-nature is fixed at PRESS, not re-decided per
motion: the app feeds the router the EFFECTIVE alt, which is `event.alt` for a
press (this establishes the gesture) and the established `alt_drag` state ALONE
for a subsequent drag or release (the per-motion `event.alt` is ignored once a
drag is under way). So once a gesture starts as an Alt-drag it stays one even if
a terminal drops the modifier bit on a later motion report, and a plain drag
stays plain even if a spurious `alt` appears on one motion. The app supplies the
`alt_drag_baseline` list into the dispatched `select.set_ranges` payload (as it
already supplies `drag_anchor` into `select.set_range`).

**Edge auto-scroll (same seam).** `apps/ssg_main.cpp` also extends the selection
during edge auto-scroll with a direct `select.set_range` dispatch (~line 1274).
That branch MUST honor the Alt-drag: when `alt_drag` is set it dispatches
`select.set_ranges(alt_drag_baseline + {drag_anchor, edgeCell})`, exactly like a
motion; only a non-Alt drag keeps `select.set_range`. Without this an Alt-drag
that reaches the viewport edge would collapse to a single selection.

**Overlap contract (by design).** When the dragged range overlaps a baseline
selection, `SelectionSet` normalization MERGES them. So "preserve the other
cursors" precisely means "preserve the baseline selections that do not overlap
the dragged range"; an overlapped baseline caret is absorbed into the drag
range, which is the intended, Sublime-like behavior.

### Non-goals (explicit)

- Alt+click on an existing caret to REMOVE it (Sublime toggle): deferred. It
  needs a `select.remove_range_at`-style command and a hit test of the click
  against existing carets; recorded as a follow-up backlog item.
- Middle/right-button multi-cursor; column (box) selection.

## Oracles / tests

Decoder (`tests/test_ssg_app.cpp`): an SGR press with the Alt bit set
(`\x1b[<8;3;4M`, Cb = left|Alt = 0+8) decodes to a `PointerEvent` with
`alt == true`, correct cell, `PointerKind::press`; the same without bit3
(`\x1b[<0;3;4M`) has `alt == false`. A drag with Alt (`\x1b[<40;..M`,
32|8) decodes `kind == drag`, `alt == true`.

Router (`tests/` pointer-routing unit test): 
- Alt press on editor → one `select.add_range` with a collapsed selection at the
  position, `begins_drag == true`; no-Alt press → `cursor.set_position`.
- Alt drag (dragging, alt) → the app dispatches `select.set_ranges` with the
  baseline plus `{anchor,active}`; no-Alt drag → `select.set_range` (unchanged).
- **Mid-drag modifier drop**: a gesture whose PRESS was Alt-drag, then a drag
  motion arrives with `alt == false`, still routes to `select.set_ranges(baseline
  + {anchor, active})` — for a drag the app feeds the router the established
  `alt_drag` alone (ignoring the per-motion bit). Symmetrically, a plain drag
  with a spurious `alt` on one motion stays a plain `select.set_range`
  (`alt_drag` is false for the whole gesture).
- Alt release → `ends_drag`, no command.

Selection command (`tests/` selection test): `select.set_ranges` replaces the
set with the given list and normalizes (sort + merge); a baseline of two carets
plus a dragged range yields three selections when disjoint, and MERGES to fewer
when the dragged range overlaps a baseline caret (the overlap contract).
Dragging the added caret UPWARD past an existing cursor (a reorder boundary)
still preserves the existing cursor and updates only the dragged range — proving
the full-recompute design has no stale-target failure.

Edge auto-scroll (`tests/` or app): an Alt-drag whose pointer is held past the
viewport edge extends via `select.set_ranges(baseline + {anchor, edgeCell})` and
preserves the other cursors (guards the §4 edge-scroll seam); a non-Alt edge
drag still uses `select.set_range`.

End-to-end (app or runtime): Alt+click at two cells yields two carets; a plain
click after collapses to one; Alt+drag yields a ranged selection plus the
prior carets.

Protocol/catalog: `select.set_ranges` added to the command catalog and its
`SelectionCommandArguments` (now carrying `std::vector<Selection> selections`)
round-trips (the existing catalog/codec round-trip tests cover new commands and
the new field); regenerate `doc/commands.md`.

## Plan

1. This spec; review; fold.
2. `PointerEvent.alt` + SGR decoder bit3.
3. `route_pointer` gains `alt`; Alt press→add_range(collapsed), Alt
   drag→`select.set_ranges`; app threads `event.alt`, and an `alt_drag` +
   `alt_drag_baseline` drag state, into the motion AND edge-auto-scroll seams.
4. `select.set_ranges` library command + `SelectionCommandArguments.selections`
   list field + Protocol codec round-trip.
5. Tests (decoder, router, selection incl. reorder/overlap, edge-scroll, e2e);
   regenerate commands.md.
6. Gate; visual signoff (mouse gesture is visual); code review; fold; merge.
