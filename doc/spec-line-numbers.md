# Spec: editor line-number gutter

Status: draft
Backlog item 2: "line numbers: spec first, default off, toggle command,
1-indexed current line with cursor has different background and foreground
colors, uses N+1 columns where N is the number of digits required to display the
largest line number in the whole file: ex '  1 '...'999 ', right-aligned numbers
with left-padded spaces."

## Summary

An optional left gutter in the editor pane showing 1-indexed logical line
numbers. Default OFF. A `view.toggle_line_numbers` command flips it (mirroring
`view.toggle_word_wrap`). The gutter is `N+1` columns wide, where `N` is the
digit count of the largest line number in the whole active document; numbers are
right-aligned within `N` columns with a single trailing space (`"  1 "`,
`" 42 "`, `"999 "`). A wrapped logical line shows its number only on its first
visual row; continuation rows show a blank gutter. The current (caret) line's
number is painted with distinct foreground AND background colors.

## Behavior

- Off by default; `view.toggle_line_numbers` toggles the workspace setting. When
  off, the editor pane is laid out and painted exactly as today (no gutter).
- On, the editor pane content is inset from the LEFT by the gutter width; the
  right-side scrollbar gutter is unchanged.
- Gutter width `= N + 1`, `N = digits(maxLineNumber)`, where `maxLineNumber` is
  the total logical line count of the whole document (NOT just the visible
  range), so the width is stable while scrolling. Examples: a 1-line file → width
  2 (`"1 "`); 9 lines → 2; 10–99 → 3; 100–999 → 4; 1000+ → 5.
- Each visible row shows `logicalLine + 1` right-aligned in `N` columns, then one
  space. A visual row that is a WRAPPED CONTINUATION of a logical line (not its
  first visual row) shows `N+1` blank cells instead of repeating the number.
- The number of the caret's LOGICAL line is painted with the current-line colors
  (distinct fg and bg); all other lines use the normal line-number color. When
  that line is wrapped, the number lives on its first visual row and is the cell
  that gets the current-line styling, even if the caret is on a continuation row.
- Clicking in the gutter does nothing (it is outside the editor content rect, so
  it produces no cursor placement). Selecting a line by clicking its number is a
  non-goal for v1.

## Design

### Setting + toggle command

Mirror word wrap exactly:

- `SettingKey::LineNumbers` added after `SettingKey::WordWrap`
  (`include/ssg/Settings.h`); `kSettingKeyCount` incremented; default `false`
  (`src/Settings.cpp` ordered list + default case).
- `EditorRuntime::Impl` gains `bool lineNumbers = false;`
  (`src/runtime/editor_runtime_internal.h`), synced from the setting in
  `syncRuntimeSettings` like `wordWrap`.
- `setLineNumbers` handler + `view.toggle_line_numbers` command registration
  (`src/runtime/presentation.cpp`), label/summary "Toggle Line Numbers".
- The setting already rides the snapshot in `SettingsViewState`, so no new
  plumbing for persistence.

### Gutter width — computed by the runtime, passed to layout

`computeShellLayout` must stay pure (no document access), so the RUNTIME computes
the width and passes it in the request, exactly as it already passes
`reservedPromptRows`:

- Add `int lineNumberGutterWidth = 0;` to `ShellLayoutRequest`
  (`include/ssg/ShellState.h`).
- In `snapshot.cpp` (where the request is built), set it to `0` when
  `!runtime.lineNumbers` or there is no active editor document; otherwise to
  `digits(totalLogicalLines) + 1`, where `totalLogicalLines = count('\n') + 1` of
  the active document text (already available there). `digits(n)` is
  `to_string(n).size()` (n ≥ 1).
- **Cache by document revision.** The newline scan is O(document) and runs every
  frame, a predictable hot path on large files. Cache `{revision, lineCount}` (or
  the derived width) on `Impl` and recompute only when the active document's
  revision changes, so a held-still large file costs nothing per frame. This
  still satisfies "largest line number in the whole file" (the cached count is
  the whole-document count, refreshed on every edit).

### Layout — carve the left gutter

- `PaneGeometry` (`include/ssg/ShellState.h`) gains `Rect lineNumbers;`
  (`{0,0,0,0}` when absent).
- `layoutPanes` (`src/ShellState.cpp`) takes the gutter width and, for a leaf
  pane, reserves it on the LEFT: `lineNumbers = {rect.x, rect.y, gw,
  rect.height}`; `content = {rect.x + gw, rect.y, rect.width - gw - scrollbarGw,
  rect.height}` (the right scrollbar reservation is unchanged). When `gw == 0`
  the content and scrollbar are exactly as today.
- **Minimum-width guard**: if reserving the gutter would leave the content below
  `editorMinimumWidth` (or ≤ 0), the gutter is NOT reserved for that frame
  (`lineNumbers` empty, content full) — the editor never becomes unusable because
  numbers were requested on a tiny pane. `computeShellLayout` decides this and
  the renderer keys off the published `lineNumbers` rect, so the two never
  disagree.
- The editor viewport columns come from `content.width` (already reduced), so
  wrapping and horizontal scroll use the narrower content; no separate change.

### Rendering — paint the gutter per visible row

In the editor pane painter (`src/Renderer.cpp` `paintDocument`), after resolving
the pane geometry:

- If `pane.lineNumbers.width > 0`, paint the gutter. For each visible row
  (`viewport.visibleRows`):
  - Determine the caret's logical line once: `caretLine =
    snapshot.sections().selection.selections.primary().active.line.value()`.
  - A row is the FIRST visual row of its logical line when `row.firstSpan == 0`
    (`ProjectedRow`/`VisualRow`, `include/ssg/Viewport.h`).
  - First-visual-row: render text `rightAlign(to_string(row.logicalLine + 1),
    width - 1) + " "`, padded with spaces to exactly `width` cells.
  - Continuation row: render `width` blank cells.
  - Colors: the gutter cells for the caret's logical line use the current-line
    roles (fg + bg below); every other row uses `SemanticRole::LineNumber` fg
    over the editor `Canvas` bg.
- The gutter is painted in its own rect and is independent of the horizontal
  scroll offset, so numbers stay put when the content scrolls sideways
  (word-wrap off).

### Theme roles for the current line

The current-line number needs a distinct fg AND bg (the user's words). Semantic
roles map to one color each, used as fg or bg by context, so add two:

- `SemanticRole::CurrentLineNumber` — the current line's number foreground.
- `SemanticRole::CurrentLineNumberBackground` — its gutter background.

Both are added to the `SemanticRole` enum in `include/ssg/Theme.h` (the sole
owner of `SemanticRole`, `kAllSemanticRoles`, and `kSemanticRoleCount` — bump the
count and extend the array; do NOT declare roles anywhere else), given defaults
in `src/DefaultTheme.cpp` (a brighter number over a subtle bar, e.g. Text-bright
over a dim highlight), included in the copy-paste dark-theme block in
`doc/config.md`, and CONSUMED by the renderer so the dead-color-role guard
(`test_render` `everyNonCaretSemanticRoleIsColorConsumedByTheRenderer`) passes.
`SemanticRole::LineNumber` already exists (mid-gray) for the non-current rows.

(If review prefers fewer roles: `CurrentLineNumber` fg over the existing
`Selection`/`Canvas` bg is an alternative, but the user explicitly asked for a
distinct background, so two roles is the default proposal.)

## Invariants

- **INV-off-is-parity**: with the setting off (`lineNumberGutterWidth == 0`),
  every pane rect, the editor viewport, hit-testing, and the rendered grid are
  byte-identical to today. The feature is inert when off.
- **INV-width-stable**: the gutter width depends only on the whole-document line
  count, not the scroll position, so it does not jitter as higher line numbers
  scroll into view.
- **INV-content-consistent**: the renderer paints numbers using the SAME
  `lineNumbers`/`content` rects the layout published; the gutter and the document
  columns can never overlap or disagree (including the min-width guard).
- **INV-wrapped-line-once**: a wrapped logical line shows its number only on its
  first visual row; continuation rows are blank.
- **INV-gutter-inert**: a pointer press in the gutter places no caret (it is
  outside `content`).

## Oracles

### Settings/toggle (`tests/runtime/test_runtime_*`)

- `view.toggle_line_numbers` flips the setting; default is off; a second toggle
  restores off; the snapshot's settings reflect the value.

### Width (`tests/test_ui_layout.cpp` or a small unit)

- `digits+1` table: 1→2, 9→2, 10→3, 99→3, 100→4, 999→4, 1000→5 lines.
- **Text-derived integration**: drive real document texts through the runtime →
  snapshot → `ShellLayoutRequest.lineNumberGutterWidth` to prove the text→count
  wiring (not just the digit function). The count is `count('\n') + 1`, matching
  the model (a trailing newline yields a final empty line the caret can sit on):
  empty document → 1 line → width 2; `"a\nb\nc"` (2 newlines) → 3 lines → width
  2; `"a\nb\nc\n"` (3 newlines) → 4 lines → width 2; a 9-line doc → width 2, a
  10-line doc → width 3, a 1000-line doc → width 5; setting off → width 0.

### Layout (`tests/test_ui_layout.cpp`)

- With `lineNumberGutterWidth = w`, the pane `content.x` increases by `w` and
  `content.width` decreases by `w` (scrollbar unchanged); `pane.lineNumbers` has
  width `w` at the pane's left edge.
- With `w = 0`, the pane geometry equals today's (parity).
- Min-width guard: on a pane too narrow to fit the gutter and
  `editorMinimumWidth`, `lineNumbers` is empty and content is full.

### Rendering (`tests/test_render.cpp`)

- A short unwrapped document with the gutter on paints `"  1 "`, `"  2 "`, …
  right-aligned for the visible rows, with the trailing space, at the pane's left
  columns; `SemanticRole::LineNumber` on non-caret rows.
- The caret's row paints its number with the current-line roles (fg + bg
  observed), and this keeps the dead-role guard satisfied (both new roles are
  consumed).
- A wrapped logical line (word wrap on, a line longer than the content width)
  shows the number on the first visual row and a blank gutter on the
  continuation row.
- **Caret on a wrapped continuation row**: when the caret sits on a continuation
  visual row of a wrapped logical line, that logical line's number (shown on its
  FIRST visual row) still gets the current-line styling — the highlight tracks
  the logical line, not the visual row. Pin this so the wrapped + current-line
  interaction is not mis-implemented.
- Horizontal scroll (word wrap off) leaves the gutter numbers unchanged.

### Hit-testing (`tests/test_hit_test.cpp`)

- A press in a gutter column yields no editor cursor placement (no `Editor`
  region hit / no `cursor.set_position`), while a press one column into `content`
  still resolves to a document position.

## Plan

| # | Step | Files | Oracle |
|---|------|-------|--------|
| 1 | `SettingKey::LineNumbers` (default off) + `view.toggle_line_numbers` command + `Impl::lineNumbers` + sync | `include/ssg/Settings.h`, `src/Settings.cpp`, `src/runtime/editor_runtime_internal.h`, `src/runtime/presentation.cpp` | toggle flips; default off; commands.md regenerated |
| 2 | Runtime computes `lineNumberGutterWidth = digits(lineCount)+1` (0 when off / no doc), cached by active document revision, into `ShellLayoutRequest` | `include/ssg/ShellState.h`, `src/runtime/snapshot.cpp`, `src/runtime/editor_runtime_internal.h` | digit-width table + text-derived integration oracle |
| 3 | `PaneGeometry.lineNumbers` + `layoutPanes` carves the left gutter; `computeShellLayout` threads the width; min-width guard | `include/ssg/ShellState.h`, `src/ShellState.cpp` | content shift; off=parity; min-width guard; ui_layout golden regen |
| 4 | Theme roles `CurrentLineNumber` (+`Background`) added, defaulted, documented in `doc/config.md`, consumed | `include/ssg/Theme.h` (or the SemanticRole header), `src/DefaultTheme.cpp`, `doc/config.md` | dead-role guard passes; theme round-trip |
| 5 | Paint the gutter per visible row (LineNumber; blank continuation; current-line roles) | `src/Renderer.cpp` | render oracles; protocol/ui goldens regen |
| 6 | Document in help prose + auto commands.md | `src/runtime/help.cpp`, `doc/commands.md` | help test mentions line numbers |

## Considerations / non-goals

- **Split panes**: v1 targets the editor pane painting the active document (the
  same single-document snapshot the renderer already uses). Per-pane independent
  gutters for simultaneously-different documents are a follow-up.
- **Click-to-select-line** on a number is a non-goal for v1 (the gutter is inert).
- **Relative line numbers** (Vim-style) are out of scope.
- **Very large files**: the newline count is an O(document) scan per frame; if
  that ever matters it can be cached against the document revision, but it is not
  a v1 concern (the model already scans newlines on construction).
