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

- **Header** -- full width, row 0. Status fields and, when a picker is open,
  the input line (below).
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
  caret in `editor`, the selected tree row in `panel`, the input position in
  `prompt`. A terminal has one hardware cursor, so secondary carets are painted
  as cells instead (`SemanticRole::Caret` background); only the primary uses
  the real cursor.

**The cursor is the primary focus affordance for a text-entry surface.** A
surface that accepts typing and does not show a cursor gives the user no way to
tell it has focus, which is why the rule above is normative rather than
advisory.

### Caret reveal

After any accepted command that moves or recreates the primary selection, a
displaying client scrolls minimally to keep the primary caret visible --
vertically always, and horizontally when word wrap is off and the line is
clipped. The reveal target is only `selections.primary().active`.

## Invariants

- **I23 (caret visibility, `doc/spec.md`)** -- after any accepted command that
  moves or recreates the primary selection, every displaying client view
  minimally scrolls to keep its primary caret visible.
- **The terminal cursor is placed in the focused surface**, per Focus above. A
  focusable text-entry surface without a cursor violates this.
- **Reserved gutters keep content width stable.** The panel's and pane's
  scrollbar columns are always reserved, so text does not reflow when a thumb
  appears or disappears.
- Layout is a pure function of its request and shell state. The same request
  produces the same geometry on every client.
- The library owns geometry; the client only paints it. A client does not
  invent regions or reposition them.

## Considerations

- **Header width is contended.** The input line, the leader-chord hint, and the
  status fields all want the header row. Leader entry and picker focus are
  mutually exclusive, so those two never compete -- but the input line and the
  status fields do, and the resolution determines whether fields move while the
  user types.
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
