# spec-input-line

## Goals

The header's text input is named for what it is rather than for one of its two
callers, sits where typing into it cannot push the working directory and branch
around, and shows a cursor so the user can see it has focus.

## Design

### Rename: the buffer is not the palette's

The surface is called `palette*` throughout -- `ShellLayoutRequest::paletteActive`
/ `paletteQuery` / `paletteGhost`, and the layout node ids `palette_query` and
`palette_ghost`. It was named when the command palette was its only caller. The
file finder now uses the same surface, and `PickerKind` exists precisely because
more pickers are expected.

Rename to **input line**: `inputLineActive`, `inputLineQuery`, `inputLineGhost`,
node ids `input_line.query` and `input_line.ghost`.

Chosen over the alternatives on the grounds that each is already taken or
misleading: `prompt` is the reserved-rows surface below the tab bar
(`PromptKind`, `PromptSurface`) and would collide; `picker` names WHAT is being
searched (`PickerKind::Command`/`File`), not the place the query is typed;
`command line` is wrong for a file picker.

`PaletteProjection`, `PaletteRow`, `PaletteSearcher` and `PaletteCandidate` keep
their names: they are the RESULTS surface and the ranking engine, not this input.
Renaming them is a larger, separate change with no user-visible payoff.

This is a rename with no behavior change. It touches the wire
(`ShellLayoutRequest` is not serialized, but the layout NODE IDS are published
in `ShellViewState` and asserted by tests and goldens), so it is sequenced first
and separately, and the render goldens must be regenerated in that step alone --
mixing a rename with a layout change would make the golden diff unreviewable.

### Position: fields first, input line after

Today the input line is laid out FIRST at the header's left edge and then
`headerX` is advanced past it, so `addFields` receives a shrinking region that
starts further right on every keystroke. Both effects are visible: the fields
slide right as the query grows, and they can COLLAPSE entirely (`addFields`
drops fields that no longer fit) so the branch disappears mid-typing.

Invert it. Lay the status fields out first, at the header's left edge, in their
natural widths. Then place the input line in whatever header space remains, to
their right, growing rightward toward the header's right edge.

Consequences, all of them the point:

- Typing never moves `path` or `branch`, because their layout no longer depends
  on the query at all.
- Typing never collapses them, because `addFields` is no longer given a
  query-dependent width.
- The input line's own start column is stable while typing. It moves only when
  `path` or `branch` themselves change, which does not happen mid-keystroke.

The input line is left-aligned within its region and grows rightward, rather
than right-aligned to the header edge: a right-aligned buffer would slide its
own text leftward on every keystroke, trading one kind of jitter for another.

Rejected alternative -- giving the input line the whole header and moving the
fields to the footer -- because the footer is already the busier row and the
user asked for the fields to stay put, not to move somewhere else.

**Overflow.** When the query is longer than the space left after the fields, the
input line does NOT reclaim field space. It scrolls its own text so the cursor
stays visible, on the same principle as caret reveal in the editor: the thing
being typed at is what must remain on screen. The leading `>` sigil is retained
as the surface's identity; the query text scrolls under it.

**Degenerate case.** If the fields consume the entire header (a very narrow
viewport, or a very long path), the input line gets no room. It must not vanish
silently while still holding focus and accepting keys. In that case the input
line takes priority and the fields collapse by their existing rank mechanism --
the ONE case where typing may move the fields, because the alternative is a
focused, invisible text input.

### Cursor: a spec violation, not a new feature

`doc/spec-navigation.md` and `doc/spec-ux.md` both state that the terminal
cursor is placed in the focused surface, including "the prompt input position in
`prompt`". A picker is `FocusTarget::Prompt`.

Today that rule is violated. `Renderer.cpp` sets `grid.caret` from `paintPrompt`
when focus is `Prompt`, but `PromptSurface::promptRows` returns 0 for
`PromptKind::Palette` -- the palette reserves no rows and paints nothing there --
so `paintPrompt` yields no caret and `grid.caret` is left unset. Measured on a
real frame: with the picker open the terminal cursor sits at row 24, column 1,
which is simply where painting finished.

The fix is to publish the caret at the input line's cursor position when a
picker holds focus. The cursor belongs one column past the last typed character
-- before the ghost completion, not after it, because the ghost is a suggestion
the user has not typed and the insertion point is at the end of what they have.

Mechanism: the caret is derived from the published layout geometry of the
`input_line.query` node plus the query's display width, so it is computed from
the same numbers that positioned the text. Deriving it independently would let
the two drift.

**The boundary rule, stated once so it cannot be read two ways.** The caret
column is `query_node.x + display_width(sigil + query)`. That is the cell the
next typed character will occupy. When the query exactly fills its rect this
lands on the first cell of the ghost node -- which is correct and intended, not
an off-by-one: the ghost is unaccepted suggestion text, and the insertion point
belongs on top of it. The caret is therefore NOT constrained to lie inside the
query node's rect; it is constrained to lie inside the HEADER row. If the
computed column would fall outside the header, the input line has overflowed and
step 5's scrolling is what keeps it in view.

**Width, not byte count.** The cursor offset is the query's DISPLAY width. The
query may contain multi-byte or wide characters, and the project already owns
grapheme/cell measurement; using `size()` would misplace the cursor for any
non-ASCII query.

## Invariants

- **The terminal cursor is placed in the focused surface** (`doc/spec-ux.md`).
  This change brings the picker into compliance; it is the reason the change is
  not merely cosmetic.
- Layout stays a pure function of its request and shell state.
- The library owns geometry; the client paints what it is given and derives no
  region of its own.
- Reserved gutters keep content width stable -- unaffected here, but the same
  principle motivates stable field positions.

## Considerations

- **Node ids are published contract.** `input_line.query` / `input_line.ghost`
  appear in `ShellViewState` nodes, are hit-tested, and are asserted by tests
  and TUI goldens. The rename is mechanical but not local.
- **The leader hint shares the header start.** It renders where the input line
  used to, in the same `else if` branch. Leader entry and picker focus are
  mutually exclusive, so they never collide -- but the leader hint should follow
  the input line's new position for consistency, or the header will jump between
  two different layouts depending on which is active. This spec moves BOTH.
- **`addFields` mutates its retained set by width.** Handing it the full header
  width unconditionally changes which fields survive at narrow viewports
  compared to today; that is the intended fix, and the existing collapse-rank
  tests pin the behavior.
- **Two goldens cover this.** `tests/fixtures/tui/runtime-palette.txt` shows the
  picker open and WILL change (position and cursor). The normal and too-small
  goldens must NOT change; if they do, layout leaked into the non-picker path.
- **Distraction-free mode has no header,** so a picker opened there has nowhere
  to draw the input line. Out of scope, but must not crash: verify a picker in
  distraction-free mode degrades rather than faults.

## Risks and Mitigations

- Rename churn hiding a behavior change -> the rename lands as its own step,
  with goldens regenerated ONLY there and no layout edit in the same commit.
- Cursor placed at the wrong column for non-ASCII queries -> use display width,
  with a test using a multi-byte query.
- Caret and text disagreeing about where the query ends -> both are computed
  from the same published node geometry, and the boundary rule above is stated
  once rather than re-derived per call site.
- Fields still moving in some path -> a direct oracle: lay out the header at a
  series of query lengths and assert the `path` and `branch` node rectangles are
  IDENTICAL across all of them.

## Acceptance (Definition of Done)

- Observable: with the picker open, the working directory and branch stay
  exactly where they are as the user types, and a cursor sits at the end of the
  typed text. Needs user signoff.
- Budgets: no per-frame cost; this is layout arithmetic already being done.
- Gates: `bash scripts/check.sh` green.
- Oracles: per Plan.

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Rename `paletteActive`/`paletteQuery`/`paletteGhost` and node ids `palette_query`/`palette_ghost` to the `inputLine*` / `input_line.*` forms. NO layout change | `include/ssg/ShellState.h`, `src/ShellState.cpp`, `src/runtime/snapshot.cpp`, `apps/ssg_main.cpp`, tests referencing the ids, `tests/fixtures/tui/*` | existing suite green with only id strings changed; TUI goldens regenerated in THIS step only, and the diff is id text alone | - |
| 2 | Lay out header status fields first at the header's left edge, then the input line (and the leader hint) in the remaining space to their right | `src/ShellState.cpp` | test: for query lengths 0, 1, 8, 40, the `path` and `branch` node rects are IDENTICAL (**verify by perturbation** -- restore the old order, confirm the test fails); test: the input line's start column is likewise identical across those lengths | - |
| 3 | Publish the caret at the input line's cursor when a picker holds focus, from the published node geometry plus the query's DISPLAY width | `src/Renderer.cpp` | test: with a picker focused, `grid.caret` is inside the header row at the query node's start plus the query's display width; a multi-byte query places it by width, not byte count; **verify by perturbation** (drop the caret publication, confirm failure) | cursor-in-focused-surface |
| 4 | Collapse fields by rank only when the fields would leave the input line no room at all | `src/ShellState.cpp` | test: at a viewport narrow enough that fields fill the header, the input line still receives a non-empty rect while focused | - |
| 5 | Scroll the input line's own text when the query exceeds its region, keeping the `>` sigil fixed. Sequenced AFTER the caret so its oracle can assert on a caret that exists | `src/ShellState.cpp`, `src/Renderer.cpp` | test: with a query far wider than the region, the node rect stays inside the header AND the published caret remains within it (a non-scrolling implementation pushes the caret past the right edge, which is what this catches) | - |
| 6 | Verify: normal and too-small TUI goldens UNCHANGED; palette golden updated once; a real PTY frame places the cursor in the header | `tests/fixtures/tui/*` | full gate green; PTY capture shows the final cursor position inside the header row, not row 24 | - |
| 7 | Update the docs to the new name, position and cursor rule | `doc/spec-ux.md`, `doc/features/presentation-shell.md` (its "Header order is active command/palette query, current path, then mode" line is now wrong), `doc/spec-library-contract.md` (names `palette_ghost` explicitly at line 62), `doc/spec-palette.md` | grep: no doc names `palette_query`/`palette_ghost`, and none describes the query as leftmost in the header | - |

## Rationale

The three requests are one change: the buffer is mis-named because it was built
for one caller, mis-placed because it was laid out before the fields that should
anchor the row, and missing a cursor because the surface that would have drawn
one reserves zero rows.

The cursor item is the one worth stating plainly: it is not a new feature but a
correction. The rule already exists in two documents, and the picker is the one
focusable text-entry surface that does not honor it.
