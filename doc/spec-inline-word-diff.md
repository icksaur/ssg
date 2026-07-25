# spec-inline-word-diff

## Goals

A Modified diff line renders as ONE row (git `--word-diff` style): unchanged
text plus the specific removed word(s) in red and added/changed word(s) in
green, inline, instead of today's two-row baseline-above/target-below split.
This replaces the render-only spike (`InlineWordSegment`, current
`Renderer.cpp` branch) with a version where click, caret movement, and
selection all behave correctly around the inline ghost (removed) text —
same bar as the existing whole-row `PhantomRow` mechanism, at finer (span)
granularity.

Non-goal (explicit scope cut): word-wrap support for a merged row. A
wrapped Modified line keeps rendering as today's two-row split (this is the
existing spike's fallback, already in place); only unwrapped rows get the
merged inline treatment. Revisit only if this proves too limiting in
practice.

## Design

**Ownership**: `Viewport` already owns the single row/column↔byte mapping
(`RealRow`/`PhantomRow`/`ProjectedRow`, per spec-syntax-and-diffs.md's
COORDINATE OWNERSHIP invariant). The merged-text/segment computation moves
OUT of `Renderer.cpp` (today's spike does it ad hoc, paint-time-only) and
INTO `Viewport`, which is the only file allowed to decide what a row's
cells and their byte-offset mapping are. `Renderer` goes back to being a
pure paint step over what `Viewport` already computed — the same
relationship it has with `PhantomRow` today.

**Mechanism**: `RealRow` gains an optional ordered list of ghost spans (a
sibling concept to `PhantomRow`, at span-within-a-row granularity instead
of whole-row): each ghost span carries its display text, visual cell width,
and the single real document byte offset it is anchored to (the position
immediately before it — the same offset for every cell inside that ghost
span, mirroring `PhantomRow::followingByteOffset`'s "resolve to nearest
real position" convention). `Viewport` populates this only for an
unwrapped Modified row backed by `DiffLineChange::inlineWordSegments`
(computed already; unchanged from the spike).

**Hit-testing**: a click landing inside a ghost span resolves to that
span's anchor byte offset with `byteLen=0` (a caret position, no
selection) — exactly how clicking a whole `PhantomRow` already resolves
today, just at span instead of row granularity. Real cells after a ghost
span keep their own correct, monotonically increasing real byte offsets;
only the VISUAL column axis stretches to make room for the ghost, the byte
axis does not.

**Caret movement**: Left/Right treat a ghost span as one atomic unit stepped
over in a single move, never landing inside it — the same "phantom content
is skipped, real content is not" rule the project already applies to whole
`PhantomRow`s during vertical movement, generalized to horizontal movement
at span granularity. (Scoped to Left/Right for this phase; Home/End/word-
jump motions are not required to have special ghost-aware behavior yet —
they already land on real-content boundaries by construction, since a
ghost span never starts or ends a line.)

**Selection**: a drag whose start/end straddle a ghost span selects only
the real bytes on either side of it — the ghost contributes zero bytes to
the selection, consistent with its `byteLen=0` hit targets. This mirrors
the existing "a DRAG across phantom rows selects only the spanned REAL
buffer text" rule.

**Reveal/follow**: no change. `FollowEditsModel`'s row-counting math
doesn't see intra-row content, only row counts.

## Invariants

- Every rendered cell still traces back to `Viewport`'s single row
  projection; `Renderer` does not invent layout.
- A ghost span is never editable, never selectable, and a click into it
  always resolves to a real byte offset — no dead zones, no crashes, no
  ambiguous byte offsets.
- Real content's byte offsets are never altered by ghost spans; only
  visual column position shifts.
- Identity mapping (no ghost spans) when a row is not an unwrapped
  Modified inline row — zero behavior change for every other row kind.

## Considerations

- `positionForProjectedCell`/`projectedVisualColumn` (Selection.cpp) key
  off `RealRow.startCell/endCell`, which will now include ghost cell
  width. The translation from "visual column" to "real cell index" (skip
  ghost width) must happen once, in one place, not be re-derived ad hoc at
  each call site.
- The existing spike's `Renderer.cpp` branch (paint-time merged-text
  construction) is DELETED, not left as a second code path alongside the
  new `Viewport`-owned one.
- `DiffLineChange::inlineWordSegments` (data) is unchanged from the spike;
  only WHERE it gets turned into cells/hit-targets moves (Renderer ->
  Viewport).

## Risks and Mitigations

- Risk: hit-target generation for wrapped/unwrapped modes is duplicated
  code (`Viewport::compute` and `Viewport::computeUnwrapped`) — a ghost-span
  fix applied to one path and not the other would silently regress the
  other. Mitigation: since ghost spans are unwrapped-only (non-goal above),
  this only touches `computeUnwrapped`'s path; `compute` (wrapped) needs no
  change, eliminating the duplication risk for this feature.
- Risk: existing tests hardcode a strict `byteOffset == column` formula for
  ordinary rows (per investigation, `test_viewport.cpp`). Mitigation: those
  fixtures are for rows WITHOUT ghost spans and must keep passing
  unmodified — this is the identity-mapping invariant above, directly
  testable by running the existing suite unchanged.

## Acceptance (Definition of Done)

- Observable: in the real TUI, clicking anywhere inside a Modified diff
  line's red (ghost/removed) text places the caret at the boundary between
  the removed and added words (or the nearest real position), never inside
  dead space; left/right arrow keys skip over the red text in one step
  from either side; dragging a selection across the red text selects only
  the real (target) bytes before/after it.
- Gates: `bash scripts/check.sh` green.
- Oracles:
  - hit-test: a click at every column within a ghost span resolves to the
    SAME anchor byte offset with `byteLen=0`; a click just past the ghost
    span's last column resolves to the first real byte after it.
  - caret: pressing Right immediately before a ghost span lands immediately
    after it (never inside); pressing Left immediately after lands
    immediately before it.
  - selection: a drag from a real column before a ghost span to a real
    column after it selects exactly the real bytes spanned, none of the
    ghost's.
  - wrapped-row scope cut: a Modified line long enough to WRAP still
    renders as today's two-row baseline/target split (no ghost spans, no
    merged inline row) -- an explicit regression oracle, not just a
    narrative non-goal, so this scope cut can't silently drift.
  - regression: every existing `test_viewport.cpp`/`test_hit_test.cpp`/
    `test_selection.cpp` case for a row WITHOUT ghost spans is unmodified
    and still passes (identity-mapping invariant).

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Move merged-text/ghost-span computation from `Renderer.cpp`'s paint-time branch into `Viewport`'s unwrapped row computation; `RealRow` gains its ghost-span list | `include/ssg/Viewport.h`, `src/Viewport.cpp`, `src/Renderer.cpp` (delete spike branch, paint from Viewport's output instead) | existing visual output (colors/text) unchanged from the spike, confirmed via the pty harness | coordinate ownership |
| 2 | Hit-target generation for a ghost span (anchor byte offset, byteLen=0, real cells after it keep correct offsets) | `src/Viewport.cpp` (`computeUnwrapped`) | hit-test oracle above | ghost never editable |
| 3 | Caret horizontal step-over (Left/Right treat a ghost span as one atomic hop) | `src/Selection.cpp` | caret oracle above | ghost never editable |
| 4 | Selection drag correctness across a ghost span | `src/Selection.cpp` | selection oracle above | ghost never editable |
| 5 | Regenerate/update any fixtures coupled to unwrapped hit-target byte formulas for the specific rows now carrying ghost spans (should be none outside new test fixtures, per the non-goal scope cut) | `tests/test_viewport.cpp`, `tests/test_hit_test.cpp`, `tests/test_selection.cpp`, `tests/test_renderer_diff_overlay.cpp` | regression oracle above | identity mapping |
| 6 | End-to-end confirmation via the real binary (pty harness) | - | real captured bytes show one merged row; keyboard click/arrow-key session confirms caret/selection behavior | - |
