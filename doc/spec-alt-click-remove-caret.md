# spec-alt-click-remove-caret

Status: draft

## Goals

Alt+click on an EXISTING caret or selection removes it — the Sublime/VS Code
toggle. This closes the one remaining non-goal of `doc/spec-mouse-multicursor.md`:
today an Alt+left-click always ADDS a collapsed caret (de-duping if the cell
already holds one), so there is no mouse way to take a cursor back out of a
multi-cursor set. After this change:

- **Alt+left-click on a cell already covered by a selection** (a collapsed caret
  at that cell, or any cell inside a ranged selection) REMOVES that whole
  selection from the set, leaving the others untouched — provided more than one
  selection exists.
- **Alt+left-click on a cell not covered by any selection** ADDS a collapsed
  caret there, exactly as today.
- **Alt+left-click on the sole remaining selection** is a no-op: the document
  always keeps at least one caret (matches Sublime — you cannot remove the last
  cursor).
- A plain (no-Alt) click is unchanged: it collapses to a single caret at the
  cell.

Non-goal (unchanged from the parent spec): Shift/Ctrl mouse chords. This spec
adds no new library command and no protocol change.

## Design

### Where the decision lives

The add-vs-remove choice is made entirely inside the pure router
`route_pointer` (`apps/pointer_routing.cpp`), which already receives both inputs
it needs on an Alt press:

- `targets.document_position` — the resolved click cell.
- `alt_drag_baseline` — the current selection set, which the app already captures
  from the snapshot's selection section BEFORE dispatching, on every Alt editor
  press (`apps/ssg_main.cpp` ~1488-1495). The router today only consults this on
  a drag; this spec also consults it on the press.

So NO new `PointerTargets` field and NO app-side hit-test are required: the app's
existing baseline capture already hands the router the set to test against. The
router stays pure and unit-testable.

### The removal mechanism

Removal REUSES the existing `select.set_ranges` library command (added by the
parent spec) — there is no `select.remove_range_at`. On an Alt press that hits an
existing selection and where the set has more than one selection, the router:

- builds `ranges = alt_drag_baseline` with the hit selection ERASED,
- dispatches `select.set_ranges{ranges}`,
- sets `begins_drag = false` (a removal is a discrete click, not the start of a
  drag; nothing is being extended).

This mirrors how Alt+drag already rebuilds the whole set from the immutable
baseline: the app owns the set snapshot, the router computes the new list, and
the library normalizes (sort + merge) on apply. Because the guard requires more
than one selection, `ranges` is never empty; and even if it were,
`select.set_ranges` already rejects an empty list (Selection.cpp:1019), so the
"a document always has ≥1 caret" invariant is enforced in two places.

### The hit test (pure, in the router)

A click position `P` (a `DocumentPosition`) HITS a selection `S` by comparing
BYTE OFFSETS only — `P.byteOffset` against `S.lower().byteOffset` (`lo`) and
`S.upper().byteOffset` (`hi`), the same `Selection::lower()/upper()` accessors the
`SelectionSet` normalizer uses. (`DocumentPosition` has equality but no ordering
operator; the router compares the `byteOffset` members explicitly, never line/cell
values.) With that:

- `S` is collapsed (`lo == hi`): hit iff `P.byteOffset == lo` (click exactly on
  the caret cell), OR
- `S` is ranged (`lo < hi`): hit iff `lo ≤ P.byteOffset < hi` (click anywhere
  inside the range; the exclusive upper bound means clicking the cell just past
  the range's end does NOT hit it, matching the caret-between-cells model).

**Precedence when two selections hit.** `SelectionSet` normalization merges only
when `current.lower() < previous.upper()` (STRICT), so a collapsed caret sitting
exactly at a range's lower bound is NOT merged away: a caret `[5,5]` and a range
`[5,10)` coexist, and both match at `P.byteOffset == 5`. This is the ONLY way two
baseline selections can share a hit (the set is otherwise disjoint). The router
removes the one that appears FIRST in the baseline's normalized order. Because the
normalizer sorts by `lower` then `upper`, the zero-width caret (`upper == 5`)
sorts before the range (`upper == 10`) at the shared lower bound, so **the
collapsed caret is removed in preference to a range that starts at the same
cell** — clicking precisely on a caret takes that caret out. This tie-break is
pinned by an oracle. In every other case exactly one selection hits, and the
router removes it.

The router scans `alt_drag_baseline` in order and removes the first hit; since
the app hands the router the set already in normalized order (it comes straight
from `SelectionSet::items()`), "first in scan order" IS "first in normalized
order".

### Add path unchanged

When the Alt press does NOT hit any baseline selection (or the set has exactly
one selection — the last-caret guard), the router keeps today's behavior:
`select.add_range{collapsed P}` with `begins_drag = true` (so an Alt+press-drag
still extends the newly added caret). Re-adding a caret that already exists still
de-dups to a no-op in the set; the guard means the sole-caret case falls through
to this add path, where re-adding the same caret is the intended no-op.

## Invariants

- **A document always has at least one selection.** The router removes only when
  `alt_drag_baseline.size() > 1`; `select.set_ranges` independently rejects an
  empty set. Neither path can empty the selection set.
- **The router is pure.** `route_pointer` depends only on its arguments; the
  add-vs-remove decision reads `targets.document_position` + `alt_drag_baseline`,
  both already passed. No new I/O, no snapshot access inside the router.
- **No new command, no protocol/wire change.** Removal reuses `select.set_ranges`;
  the command catalog, the codec, and `doc/commands.md` are unchanged.
- **A removal starts no drag.** `begins_drag = false` on the remove path, so the
  app does not arm `dragging`/`altDrag` and a stray motion after a remove-click
  extends nothing.
- **Plain (no-Alt) mouse behavior is byte-identical.** Only the `alt` press
  branch changes; every non-Alt path and every drag/release path is untouched.

## Considerations

- **The baseline is captured before the (old) add, and is exactly the set to test
  and to reduce.** The app assigns `altDragBaseline` from
  `snapshot->sections().selection.selections.items()` before dispatch, so it is
  the pre-click set — precisely what "does this click land on an existing caret"
  must consult, and precisely the list to erase the hit from. No ordering change
  to the app is needed; the router simply also uses it on the press.
- **Overlap / adjacency.** After normalization the baseline ranges are disjoint
  EXCEPT that a collapsed caret may sit exactly at a range's lower bound (the
  strict-`<` merge rule, Selection.cpp:735). That single coexistence case is
  resolved by the normalized-order precedence above (caret before range); every
  other click hits at most one selection. Two collapsed carets can never share a
  byte offset (identical selections dedup in the normalizer, line 732), so a
  caret-cell click removes exactly one caret.
- **Ranged-selection removal.** Alt+clicking inside a multi-cell selection removes
  the entire range (not a sub-span) — the whole selection is one set member. This
  is the intended Sublime behavior and needs no sub-range surgery.
- **Interaction with double-click.** A double-click is dispatched via
  `double_click_dispatch` BEFORE the router (`doubleClickPosition` short-circuit),
  so an Alt+double-click still selects the word; the remove path is only reached
  by a single Alt press. No change to the double-click seam — and an oracle pins
  that Alt+double-click routes to word-selection, not removal, so a future
  pointer-loop refactor cannot silently regress it into a remove.

## Risks and Mitigations

- **Off-by-one in the hit test** (clicking just past a range removing it, or a
  caret cell not registering): pinned by hand-case router oracles at the exact
  boundary offsets (P == hi must NOT hit a range; P == caret must hit).
- **Removing the last caret** slipping through: guarded in the router
  (`size > 1`) AND by `select.set_ranges`'s own empty-set rejection; an oracle
  asserts an Alt-click on the sole caret dispatches NOTHING (or an idempotent
  add), never a `set_ranges`.
- **A remove accidentally arming a drag** (so the next motion mutates the set):
  oracle asserts `begins_drag == false` on the remove path.

## Acceptance (Definition of Done)

- Observable: with two carets, Alt-clicking one leaves a single caret at the
  other; with a ranged multi-cursor selection, Alt-clicking inside one range
  drops that range; Alt-clicking empty space still adds a caret; Alt-clicking the
  only caret does nothing. Because this is a visual mouse gesture, the end-to-end
  result needs VISUAL SIGNOFF before merge.
- Budgets: n/a (a pure O(n) scan over the selection set per Alt press).
- Gates: `bash scripts/check.sh` green.
- Oracles:
  - Router hit test (`tests/test_ssg_app.cpp`) — reference/hand cases: Alt press
    on a cell holding one of two collapsed carets → one `select.set_ranges` whose
    list is the OTHER caret, `begins_drag == false`; Alt press inside a ranged
    selection (with a second selection present) → `set_ranges` without that range;
    Alt press on empty space → `select.add_range` (unchanged), `begins_drag ==
    true`; Alt press on the SOLE caret → `select.add_range` of that same caret
    (the no-op add path), never `set_ranges`; boundary: `P == hi` of a range does
    NOT hit (adds instead), `P == lo` collapsed DOES hit; precedence: a caret
    `[5,5]` beside a range `[5,10)`, Alt press at `P==5` removes the CARET (leaves
    the range), pinning the normalized-order tie-break.
  - Double-click precedence (`tests/test_ssg_app.cpp`) — an Alt+double-click on
    the editor routes through `double_click_dispatch` to word selection, NOT the
    remove path (guards the app-loop short-circuit against a future refactor).
  - Selection invariant (`tests/test_selection.cpp` or the router oracle) — a
    `set_ranges` produced by a removal is non-empty and, after the library
    normalizes, contains exactly the baseline minus the removed member.
  - End-to-end (app/runtime) — Alt+click two cells → 2 carets; Alt+click one of
    them → 1 caret at the survivor; the surviving caret is the one NOT clicked.
  - Regression — every existing pointer/selection oracle stays green with no
    golden regeneration (plain click, Alt+add, Alt+drag, edge-scroll unchanged).

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | This spec; review to the standing session; fold. | `doc/spec-alt-click-remove-caret.md` | — | — |
| 2 | Add a pure `caret_hit_index(baseline, P)` helper (byte-offset compare; collapsed `P==lo`, ranged `lo≤P<hi`; on the caret-at-range-lower coexistence, return the FIRST normalized-order hit = the caret) + extend the Alt-press branch of `route_pointer`: on a hit with `baseline.size() > 1` dispatch `select.set_ranges{baseline erase hit}` with `begins_drag = false`; else the existing `select.add_range` add path. | `apps/pointer_routing.h`, `apps/pointer_routing.cpp` | router hand-cases: remove-one-of-two, remove-range, add-on-empty, no-op-on-sole, boundary `P==hi`/`P==lo`, caret-vs-range precedence | router purity; ≥1 caret; no drag on remove |
| 3 | Router + selection oracles for every case above (incl. precedence + Alt+double-click-is-word-select); end-to-end two-caret add-then-remove. | `tests/test_ssg_app.cpp`, `tests/runtime/test_runtime_editing.cpp` (or e2e) | the Acceptance oracles | ≥1 caret; plain/double-click paths unchanged |
| 4 | Gate; VISUAL SIGNOFF of the gesture; code review to the standing session; fold; merge to master and push (feature branch not pushed). | — | `bash scripts/check.sh` green | no wire/API change |

## Rationale (optional, skippable)

The parent spec deliberately shaped the mouse multi-cursor plumbing so this
toggle would be additive: it already threads an effective-Alt flag and an
immutable `alt_drag_baseline` set into the pure router, and it already added the
`select.set_ranges` plural primitive. Removal therefore needs neither a new
command nor a new data channel — only a hit test the router runs against inputs
it already receives, and a choice of which existing command to emit. Keeping the
decision in the pure router (rather than branching in the app loop) is what makes
the whole feature a handful of table-driven unit cases instead of an interactive
one.
