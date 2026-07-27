# spec-ux

The document of record for the user interface: what occupies the screen, what
holds focus, how focus is shown, and where the cursor goes.

Written from the code, not from prior docs. Where a feature spec owns the
details of one surface's behavior, this document says so rather than restating
it -- the failure mode being avoided is `doc/spec-color.md`'s predecessors,
which described machinery that had been deleted.

Color is not covered here. `doc/spec-color.md` is the document of record for
palettes, roles, and background washes.

## Goals

One place that answers: where does a thing appear on screen, which surface has
focus, how does the user know, and where is the cursor.

## Design

### Screen regions

Laid out by `computeShellLayout` (`src/ShellState.cpp`) from the top down. The
viewport must be at least 20 columns by 4 rows; below that, layout is rejected
rather than degraded.

- **Header** -- full width, row 0. Status fields anchored at the left edge,
  then the input line when a picker is open (below).
- **Panel** -- left edge, rows 1..height-2, when requested AND the viewport can
  afford it. Targets 24 columns, never renders below 12, and is dropped
  entirely before the editor is squeezed under 20 columns. Its rightmost column
  is a reserved scrollbar gutter, so tree text width does not change as the
  thumb appears.
- **Tab bar** -- one row at the top of the main column, right of the panel.
- **Prompt rows** -- zero to three rows below the tab bar, sized by the active
  prompt kind (`PromptSurface::promptRows`): Find 2, Replace 3, Path/Settings/
  CommandArgument 1, **Palette 0**. The palette kind reserves no rows because
  its query renders in the header and its results project into the pane.
  The Path prompt is where a filename is typed for save, save-as, rename, open
  and new-directory; it is a prompt row, not the header input line.
- **Pane** -- the remaining main-column space. Its rightmost column is a
  reserved scrollbar gutter, for the same stability reason as the panel's.
- **Footer** -- full width, last row. Status fields from the left; footer
  actions packed from the right.

**Distraction-free mode** drops the header and footer; the editor takes the
full viewport.

### Status fields collapse by rank, and are laid out left to right

Header and footer fields come from `data/ui/status_fields.json`, each with a
`collapse_rank`. `addFields` retains fields in rank order until the available
width runs out (lower rank survives), then lays the retained set out left to
right in declaration order.

Header fields today: `path` (rank 0), `branch` (rank 1). Footer: `status`
(rank 0), `follow` (rank 1).

The consequence that matters: **a field's position depends on the width
available to `addFields` and on the widths of the fields before it.** Anything
that consumes header width ahead of the fields moves them.

### The input line

The single-row text input that appears in the header when a picker is open --
the command palette or the file finder. Vim's ex-line, at the top.

It is one surface serving every picker, not one per picker: the picker
abstraction (`PickerKind`, `doc/spec-file-finder.md`) chooses what is being
searched; the input line is where the query is typed regardless.

It carries three things: a `>` sigil, the query text the user has typed, and a
dim ghost completion of the top-ranked candidate trailing it. The query is
client-owned; the candidate list and ghost come from the server.

**It is laid out AFTER the status fields, and never takes space back from
them.** The fields are placed first at the header's left edge, with the input
line's budget subtracted from their region up front rather than clawed back
afterwards. Two consequences, both load-bearing:

- Typing cannot move or collapse the path and branch, because their layout does
  not depend on the query at all.
- Header nodes never overlap. Hit-testing returns the FIRST node containing a
  cell, so an input line drawn on top of a field would render the query while
  dispatching the field's command on click.

The reserved budget is a fixed size, deliberately independent of the query: a
reservation that grew as the user typed would shrink the field region on every
keystroke and start collapsing fields again, which is the behavior this ordering
exists to prevent.

When the query outgrows its region the input line **scrolls its own text**,
showing the tail so the insertion point stays visible -- the same principle as
caret reveal in the editor. The `>` sigil stays fixed as the surface's identity
while the text slides under it. Slicing is on grapheme boundaries, never bytes.

Layout holds one column back for the caret, because a terminal cursor has to
land on a real cell.

Results do not appear here. They project into the active pane
(`PaletteProjection`), which is a projection rather than an overlay -- the
underlying document state is preserved and restored on close.

### Focus, and how it is shown

`FocusTarget` is exactly `Editor`, `Panel`, or `Prompt` (`include/ssg/focus.h`).
It doubles as the keymap context name, so the focused surface decides which
bindings apply.

Focus is indicated two ways:

- **The active/inactive role split.** The focused surface renders in its
  `*Active` semantic role and the unfocused one in `*Inactive` (for example
  `PanelActive` vs `PanelInactive`).
- **The terminal cursor.** It is placed in the focused surface: the primary
  caret in `editor`, the selected tree row in `panel`, and the input position in
  `prompt` -- which includes a picker's input line, whose caret sits one column
  past the last DRAWN character (at the end of the visible text when the query
  has scrolled). A terminal has one hardware cursor, so secondary carets are
  painted as cells instead (`SemanticRole::Caret` background); only the primary
  uses the real cursor.

**The cursor is the primary focus affordance for a text-entry surface.** A
surface that accepts typing and does not show a cursor gives the user no way to
tell it has focus, which is why the rule above is normative rather than
advisory. The input line is the case that proved it: it reserves zero prompt
rows, so the prompt painter produced no caret for it and the cursor was left
wherever painting happened to finish, several rows away.

### Caret reveal

After any accepted command that moves or recreates the primary selection, a
displaying client scrolls minimally to keep the primary caret visible --
vertically always, and horizontally when word wrap is off and the line is
clipped. The reveal target is only `selections.primary().active`.

### Scrolling and scrollbars

Three surfaces scroll: the **document pane**, the **picker list** (command
palette and file finder), and the **panel** (file explorer and git trees).

**Scrollbars and scroll gestures are vertical only.** There is no horizontal
scrollbar and no horizontal scroll gesture on any surface. The document pane
does move horizontally when word wrap is off, but only as a consequence of
revealing the caret (`requestedFirstVisualColumn`, driven by caret reveal); the
user cannot scroll horizontally, and nothing paints a horizontal track. See
`doc/spec-viewport-projection.md` for that mechanism.

The user-visible contract is identical for all three, and stated here once
because "the bar behaves unlike the document" is exactly the drift this
document exists to prevent:

- **A reserved gutter.** Every scrollable surface reserves its rightmost column
  for a scrollbar, always, whether or not a thumb is currently shown. Content
  width therefore never changes as a list grows or shrinks.
- **One appearance.** A thumb is `#`, a track is `|`, drawn in
  `SemanticRole::ScrollbarThumb` / `ScrollbarTrack`. Every surface paints
  through one function, so they cannot diverge. Thumb *size* is proportional to
  the visible fraction and legitimately differs between surfaces.
- **An empty gutter means nothing to scroll.** When the content fits, the
  gutter is blank rather than showing a full-height thumb. A collapsed tree
  shows an empty gutter; this is the surface saying "there is no more", not a
  missing scrollbar.
- **The same gestures, everywhere.** The wheel scrolls the surface under the
  pointer. Clicking a gutter jumps to that position. Dragging a thumb scrolls
  live. A surface that responds to one of these responds to all of them.- **Selection may leave the viewport.** An explicit scroll gesture (wheel,
  gutter click, thumb drag, page keys) is the one interaction that decouples
  the view from the selection, and never snaps back. Moving the selection, by
  contrast, always reveals it.

`doc/spec-scroll.md` owns the mechanism: offsets, the shared scroll-view
function, reveal policy, and the consolidation work that makes the gesture rule
above true of all three surfaces.

## Invariants

- **I23 (caret visibility, `doc/spec.md`)** -- after any accepted command that
  moves or recreates the primary selection, every displaying client view
  minimally scrolls to keep its primary caret visible.
- **The terminal cursor is placed in the focused surface**, per Focus above. A
  focusable text-entry surface without a cursor violates this.
- **Reserved gutters keep content width stable.** The panel's and pane's
  scrollbar columns are always reserved, so text does not reflow when a thumb
  appears or disappears.
- **Every scrollable surface answers the same gestures.** Wheel, gutter click,
  and thumb drag work on the document, the picker, and the panel alike. Held by
  construction: one catalog (`ssg::app::scrollable_regions()`) drives both the
  wheel and the gutter routing, and a test asserts every entry answers press and
  drag. A surface that scrolls but ignores a gesture is a defect, not a design
  choice.
- **One scrollbar appearance.** All scrollbars are painted by one function from
  one pair of semantic roles; only thumb size varies.
- Layout is a pure function of its request and shell state. The same request
  produces the same geometry on every client.
- The library owns geometry; the client only paints it. A client does not
  invent regions or reposition them.

## Considerations

- **Header width is contended.** The input line, the leader-chord hint, and the
  status fields all want the header row. Leader entry and picker focus are
  mutually exclusive, so those two never compete -- the leader hint occupies the
  same slot as the input line, so the header does not jump between two layouts
  depending on which is active. The fields are always laid out first, so they
  are never the ones that move.
- **Header nodes must stay disjoint.** Hit-testing takes the first node
  containing a cell, so overlapping nodes route clicks to whichever was emitted
  first regardless of what is drawn on top.
- **The prompt rows and the input line are different surfaces.** Find and
  replace render their query in the reserved rows below the tab bar; a picker
  renders its query in the header and reserves no rows. Both are
  `FocusTarget::Prompt`.
- **Minimum viewport is a hard floor,** not a degradation path: below 20x4,
  layout returns an error and the client shows a too-small screen.

## Where feature detail lives

This document does not restate these; they remain authoritative for their own
behavior:

- `doc/spec-color.md` -- palette, roles, background washes.
- `doc/spec-file-finder.md` -- picker kinds, candidate sources, fuzzy ranking.
- `doc/spec-palette.md` -- palette candidate sourcing and execution.
- `doc/spec-m7.md` -- find/replace surfaces, selection and multi-caret
  rendering precedence.
- `doc/spec-keymap.md`, `doc/spec-mod-keys.md` -- key resolution and contexts.
- `doc/spec-scroll.md`, `doc/spec-viewport-projection.md` -- scroll offsets and
  viewport projection, including horizontal scroll when word wrap is off.
- `doc/spec-navigation.md` -- focus routing commands.
- `doc/features/presentation-shell.md` -- the server/client contract for
  emitting shell state.
