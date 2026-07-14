# Spec: find/replace, multiple selections, multi-cursor (M7)

## Goal

Deliver milestone M7's editing feature set in the TUI: find/replace in the
current document, multiple selections, and multi-caret editing. The library
already implements the behavior (controllers, commands, and multi-selection
editing); M7 is almost entirely **integration** — rendering, keymap bindings,
terminal input decoding, and the find-query path — plus a small render-model
change for multiple carets. No new editor semantics are invented here.

## Background: what already exists (verified)

- **Multi-selection editing works.** `SelectionSet::items()` holds all
  selections (primary + additional); `apply_text_input` iterates every selection,
  so `text.insert` / `text.delete_backward` / `text.newline` already edit at all
  carets. The `select.*` commands (`select.all`, `select.add_next_occurrence`,
  `select.add_cursor_up`, `select.add_cursor_down`, `select.split_into_lines`,
  and the `select.left/right/word_left/word_right/line_up/line_down/line_start/
  line_end/page_up/page_down/document_start/document_end` range extenders) all
  dispatch successfully with an empty payload and produce multiple selections
  (verified: after `select.all` + `select.add_next_occurrence` there are 2
  selections).
- **Find/replace controller is complete.** `FindReplaceController`
  (`include/ssg/find_replace.h`) has `open`/`open_replace`/`close`/
  `update_query`/`toggle_case`/`toggle_whole_word`/`toggle_regex`/
  `toggle_selection`/`next`/`previous`/`replace_current`/`replace_all`, and
  publishes `FindReplaceViewState { generation, open, replace_mode, query,
  options, matches, active_match, ... }` on the snapshot. The `find.*`/`replace.*`
  commands are bound in `src/runtime/editing.cpp` (`bind_find_replace`); `find.open`
  accepts an optional `std::string` query payload and recomputes matches.
  `PromptKind::find` (2 reserved rows) and `PromptKind::replace` (3 rows) exist.
- **`SemanticRole::search_match`** exists (theme palette index 10, orange).

### What is missing (the M7 gap)

- **Selection highlights are never painted.** `paint_document` (`src/render.cpp`)
  paints all text in `SemanticRole::foreground`; it draws **no** selection
  background, so even a single selection is invisible today.
- **Only the primary caret renders.** `CellGrid.caret` is one
  `std::optional<GridPosition>` placed at the primary selection's active
  position; additional carets are invisible.
- **No find matches are painted.** `search_match` is defined but unused in the
  renderer.
- **The keyboard cannot create a selection or a second caret.** The curated
  runtime keymap (`default_terminal_keymap`) binds only single-caret motion
  (`ArrowUp/Down/Left/Right`); there are no `select.*` bindings, and the terminal
  decoder (`ssg::app::decode_input`) does not decode modified arrows
  (`Shift+Arrow`, `Ctrl+Arrow`), which terminals send as `ESC [ 1 ; m {A..D}`.
- **No find/replace UI.** Nothing opens a `PromptKind::find` prompt, routes the
  typed query into the controller, or navigates matches from the TUI.

## Design

M7 is **six independently shippable designs**, in order (S, D, M, F1, F2a, F2b).
Each ends in a hand-testable TUI improvement. S/D/M need no find/replace; F1,
F2a, and F2b build on the selection/match rendering from S.

### S — Selection and multi-caret rendering (library render only)

The renderer paints selection ranges and all carets, driven by the already
published `SelectionViewState`. No new commands or state.

- **Selection highlight.** In `paint_document`, for every `Selection` in
  `selections.items()` that is not a caret (anchor != active), fill the cells
  between its lower and upper document positions with the `selection` role
  background (the existing panel/tree selection fill approach generalized to the
  document). Multi-line selections fill to end-of-line on interior rows. This
  makes single **and** multiple selections visible.
- **Multiple carets.** A terminal has one hardware cursor, so:
  - `CellGrid.caret` (the single hardware-cursor hint) stays at the **primary**
    selection's active position, unchanged, so the OS cursor blinks there.
  - **Secondary** carets (every non-primary selection's active position) are
    painted as cells using a distinct visual — the `caret` role as a cell
    background on the one cell at each secondary active position — so they are
    visible without a second hardware cursor. The primary is not double-painted
    (it already has the hardware cursor).
- Caret-role co-visibility: `caret` as a cell background must stay legible
  against `foreground` text; a render test asserts the secondary-caret cell uses
  the caret role and the primary uses `grid.caret`.

Oracle: a render test builds a snapshot with two selections (one multi-line
ranged, one a caret) over a fixture with an ASCII line, a line containing a
wide/UTF-8 glyph, and asserts, by exact screen cell, that (a) the ranged
selection's cells carry the `selection` role including the end-of-line fill on
interior rows and correct byte→cell mapping across the wide glyph, (b)
`grid.caret` is at the primary active position, (c) the secondary caret cell (and
only that cell) carries the `caret` role, (d) a selection extending past the
viewport is clipped to the content rect, and (e) with no selection, the painted
cells are byte-for-byte identical to the pre-change document render (no
regression).

### D — Modified-arrow terminal decoding (app decode only)

Extend `decode_input` to decode the CSI modified-key form `ESC [ 1 ; m {A|B|C|D}`
(and the `ESC [ 1 ; m {H|F}` home/end form) where `m` encodes modifiers
(`2`=Shift, `3`=Alt, `5`=Ctrl, `6`=Ctrl+Shift, …, per xterm: modifier =
`1 + bitmask`, bit1 Shift, bit2 Alt, bit4 Ctrl). The decoded `KeyStroke` sets the
corresponding `shift`/`alt`/`control` flags on the arrow code. Plain
`ESC [ {A..D}` continues to decode as an unmodified arrow. An **unsupported or
unrecognized modifier value** (a modifier byte outside the Shift/Alt/Ctrl bit
combinations, e.g. `9`) decodes to the **plain, unmodified arrow** (the sequence
is consumed fully; no modifier flags set) — a single committed fallback, not
left to implementation. This is the terminal half; the keymap (M) maps the
modified strokes to `select.*`.

Oracle: a `test_ssg_app` decode table asserts, by exact decoded stroke,
`ESC[1;2A`→`Shift+ArrowUp`, `ESC[1;5C`→`Ctrl+ArrowRight`, `ESC[1;3D`→`Alt+
ArrowLeft`, `ESC[1;6B`→`Ctrl+Shift+ArrowDown`, `ESC[1;2H`/`ESC[1;2F`→`Shift+Home`/
`Shift+End`, plain `ESC[A`→`ArrowUp` (unmodified), an unsupported modifier value
`ESC[1;9A` → plain `ArrowUp` (committed fallback, sequence consumed fully), and
split reads at each parameter boundary (`ESC[`, `ESC[1`, `ESC[1;`, `ESC[1;2`)
are `incomplete` and consume nothing until the final byte arrives.

### M — Selection and multi-cursor bindings (keymap + reachability)

Add argument-free `select.*` bindings to `default_terminal_keymap` (editor
context) so selections and extra carets are reachable, and rely on S to render
them. To avoid a prefix conflict with the existing `settings.open` chord
`[Escape, KeyF, KeyT]`, F1/F2 use `[Escape, Slash]` for find (vim-style `/`) and
`[Escape, KeyR]` for replace, so **no existing binding moves** and the normative
`[Escape, KeyF, KeyT]` settings default in `doc/spec.md`/`doc/spec-keymap.md`
stands unchanged.

Concrete bindings (committed, not "a detail"):

| Sequence | Command | Context |
|---|---|---|
| `Shift+ArrowLeft` / `Shift+ArrowRight` | `select.left` / `select.right` | editor |
| `Shift+ArrowUp` / `Shift+ArrowDown` | `select.line_up` / `select.line_down` | editor |
| `[Escape, KeyA]` | `select.all` | * |
| `[Escape, KeyD]` | `select.add_next_occurrence` | * |
| `[Escape, KeyI]` | `select.split_into_lines` | * |
| `[Escape, KeyK]` / `[Escape, KeyJ]` | `select.add_cursor_up` / `select.add_cursor_down` | * |
| `[Escape, Slash]` | `find.open` | * |
| `[Escape, KeyR]` | `replace.open` | * |

`Shift+Arrow` strokes come from D's modified-arrow decoding. All bound commands
are argument-free (verified) except `find.open`/`replace.open`, which accept an
optional payload but dispatch fine with none. The assembled keymap is validated
with `validate_keymap` at `EditorRuntime::create` (already the case); a test
asserts the assembled keymap validates clean (no `ambiguous_prefix`/`duplicate`)
after these additions, so prefix-freeness is enforced, not assumed. Every global
(`*`) chord is a prefix-free length-2 `Escape`+key except `[Escape, KeyF, KeyT]`
(settings), whose `[Escape, KeyF]` prefix is used by no other binding, and the
`prompt`-context `[Escape, Escape]`, which shares no prefix with any `*` chord.

Oracle: the K2 resolution test asserts `Shift+ArrowRight@editor` → `select.right`,
`Shift+ArrowUp@editor` → `select.line_up`, and `[Escape, KeyD]@editor` →
`select.add_next_occurrence`; a keymap-validation test asserts the assembled
keymap has no errors; a runtime test dispatches the add-cursor chord's command and
asserts the snapshot has >1 selection; the K5 argument-free test still passes.

### F1 — Find: query, navigation, and match rendering

The find query is **authoritative server state** — it drives match computation,
the highlight set, and the active-match index — unlike the palette's ranking,
which is a pure client-local view. Because the in-process TUI dispatches
**synchronously** (a command applies before the next snapshot), there is **no
client-side query echo**: the client types, dispatches a query-update command,
and renders the query and matches from the freshly published
`FindReplaceViewState`. (Optimistic local echo is a browser-latency concern and
stays deferred; M7 has no second query copy to reconcile.)

Flow:

- `find.open` opens the `FindReplaceController` **and** a one-input
  `PromptKind::find` prompt (as `settings.open` opens a settings prompt), so focus
  becomes `prompt`. The prompt provides focus + row reservation only; its
  displayed input value is **projected at snapshot construction**: when the active
  prompt kind is `find`/`replace`, `prompt_status_view` (in `src/runtime/
  snapshot.cpp`) fills the reserved rows' input value(s) from
  `FindReplaceViewState.query` (and `replacement`) and the match-count field from
  `active_match`/`matches.size()`, overriding the `PromptSurface`'s stored input.
  No `PromptSurface` input-mutation API is added; the controller is the
  behavioral authority and the snapshot projects it into the existing
  `PromptViewState`/`compute_prompt_layout` rendering. The find command handlers
  touch only the controller, never the prompt inputs.
- **Query editing without a client copy.** The client holds **no** query state.
  On a printable in find focus it computes the next query as *the published
  `FindReplaceViewState.query` with the typed code point appended*, and on
  Backspace as *the published query with the last code point removed*, then
  dispatches the new bound command **`find.update_query`** (payload: the full
  next query). Because the TUI dispatches synchronously and the K3b loop refreshes
  the snapshot before every input event, the published query the client reads is
  always current; this is write-only input capture derived from the latest
  snapshot, not a display echo, so there is nothing to reconcile (M7-5).
  `find.update_query` calls `FindReplaceController::update_query`, which recomputes
  matches and re-projects the prompt input **without** resetting the
  toggles/options or re-opening the prompt (unlike `find.open`, which resets
  generation/active_match/options and is dispatched only once, on open).
- Navigation and operations are reachable via two mechanisms. **Client
  fulfilment** keyed on the active prompt kind (the bounded pattern already used
  for the palette), on decoded strokes: in a **find** prompt, `Enter`
  (`prompt.submit`) → `find.next`, `ArrowDown` → `find.next`, `ArrowUp` →
  `find.previous`, `[Escape, Escape]` → `find.close`. This is the F1 navigation
  set; F2 adds the replace-prompt fulfilment.
- **Render (matches only).** `paint_document` paints every
  `FindReplaceViewState.match` byte range with the `search_match` role background
  and the `active_match` range with the `selection` role background (both existing
  cataloged 16-color roles). The query text itself renders through the existing
  prompt rows, so F1's only render change is document match highlighting.

`find.update_query` requires a `std::string` payload, so it is **not**
argument-free and is **not** a keymap or palette target; like the other
argument-required user commands (`text.insert`, `cursor.set_position`) it is
`keymap:false`, `palette:false`, and — being a user-visible typed command —
`lua:true` (I20 Lua parity). It needs a string wire codec. Adding it is a catalog
cascade: `data/required-commands.json` (new entry + owner category count),
`tests/test_required_commands.cpp` (oracle list + count + `static_assert` + Lua
parity coverage), `tests/runtime/command_cases.h` (runtime case + count
`static_assert`), the codec registry in `src/protocol.cpp` (string codec, like
`palette.execute`), and a protocol round-trip test.

Oracle (F1): a runtime test opens find, dispatches `find.update_query("cat")`
against a document with `cat` at three hand-authored byte offsets, and asserts
`FindReplaceViewState.matches` equals exactly those three `{begin,end}` ranges,
that the projected prompt input value equals `"cat"`, and that `find.next`
advances `active_match` 0→1→2→0; a render test builds a snapshot with known
matches and asserts the exact visible cells of each match carry `search_match` and
the active match's cells carry `selection`, including a match clipped at the
viewport edge and a match containing a wide/UTF-8 glyph; a protocol round-trip
test for `find.update_query`.

### F2a — Replace workflow

Splits out the replacement state, editing, and execution (F2b adds the toggle
chords and option indicators).

- **Replacement is controller state, and it is published/serialized.** Add a
  `replacement` string field to `FindReplaceViewState` (after `query`). Because
  the view state crosses the wire in the snapshot and delta, F2a extends the
  `FindReplaceViewState` protocol codec (`to_value`/`decode_present` in
  `src/protocol.cpp`) to encode/decode `replacement`, and a delta-replay
  round-trip must preserve it. Add a bound **`replace.update_replacement`**
  command (string payload, reusing `FindQueryArguments` — its comment already
  covers the replacement case) that calls a new
  `FindReplaceController::update_replacement(std::string)` setting
  `state_.replacement` and bumping `generation`. Updating the replacement does
  **not** re-evaluate matches (replacement does not affect matching), and
  `evaluate()` must **not** clobber `state_.replacement`.
- **`replace.open`** opens the three-row `PromptKind::replace` prompt (existing
  `prompt_row_count(replace)==3`): row 0 = query (display-only), row 1 =
  replacement (editable), row 2 = option/match-count row. It sets
  `replace_mode` and seeds the query from the current find query (so a
  find→replace flow carries the query). `prompt_status_view` projects
  `FindReplaceViewState.query` into row 0 and `replacement` into row 1 at
  snapshot time (mirroring F1's query projection).
- **Replace-field routing (single active field).** In a replace prompt the
  **query row is display-only**; **all** printable text and Backspace edit the
  **replacement** via `replace.update_replacement` (next value = published
  `replacement` mutated by the event, the same no-client-copy rule as F1). To
  change the query, use find first (its value carries into replace). `paint_prompt`
  places the hardware text cursor on the **replacement** input row (row 1) for a
  replace prompt, not the first input. Client fulfilment in replace focus:
  `Enter` (`prompt.submit`) → `replace.current`, `ArrowDown` → `find.next`,
  `ArrowUp` → `find.previous`, `[Escape, Escape]` → `find.close`. The client
  tracks a replace-prompt flag parallel to F1's find flag, derived from the
  active prompt kind being `replace`.
- **Execution sources the replacement from state.** `replace.current` replaces
  the active match; `replace.all` replaces every match. Both already exist; F2a
  changes only the runtime call site to pass `view_state().replacement` (single
  source of truth) instead of the payload — no controller-signature change.
- **Reset-to-first semantics (committed).** After `replace.current`, the
  controller's `evaluate()` recomputes matches and resets `active_match` to the
  first remaining match (index 0). F2a keeps this behavior (no controller
  change); the active index does **not** "advance" — the replaced match is
  removed and index 0 points at the next remaining match.
- **Guards (destructive-op safety).** `find` can remain open behind a
  palette/settings prompt, so an open-only guard is insufficient. Guard exactly
  `replace.update_replacement`, `replace.current`, and `replace.all` to a benign
  **success no-op** unless `view_state().open` **and** `view_state().replace_mode`
  **and** the active prompt kind is `PromptKind::replace`. Do **not** guard
  `replace.open` or the standalone `replace.workspace_*` commands.
- **Reveal the successor.** After `replace.current`/`replace.all`, follow the
  viewport to the newly active match using F1's non-destructive
  `reveal_active_find_match`.

Oracle (F2a): a runtime test opens replace with query `"cat"` over
`"cat cat cat"`, sets the replacement to `"dog"` via `replace.update_replacement`,
and asserts `view_state().replacement == "dog"` and that the replace prompt row 1
projects `"dog"`. It dispatches `replace.current` and asserts the document text
is exactly `"dog cat cat"`, `matches.size() == 2`, the replaced span `[0,3)` no
longer matches, and `active_match == 0` pointing at the match now at `[4,7)`. From
a fresh state (query `"cat"`, replacement `"dog"`), `replace.all` yields exactly
`"dog dog dog"` with `matches` empty and `active_match == nullopt`. A guard test
asserts `replace.current`/`replace.all`/`replace.update_replacement` are benign
success no-ops (document and state unchanged) when no replace prompt is active. A
protocol test round-trips a `FindReplaceViewState` carrying a non-empty
`replacement` through the snapshot codec and a delta replay. A render test asserts
the replace prompt's three rows show the query (row 0), replacement (row 1), and
the hardware cursor on row 1.

### F2b — Find/replace toggles and option indicators

- **Toggle chords in prompt context.** The toggle commands already exist; F2b
  binds them as prompt-context keymap chords (context `prompt`, not `*`):
  `[Escape, KeyC]` → `find.toggle_case`, `[Escape, KeyG]` →
  `find.toggle_whole_word`, `[Escape, KeyE]` → `find.toggle_regex`,
  `[Escape, KeyL]` → `replace.all`. `KeyC`/`KeyG`/`KeyE`/`KeyL` are not in the
  `*` chord set, so prefix-freeness holds.
- **Guards.** Guard `find.toggle_case`/`find.toggle_whole_word`/
  `find.toggle_regex`/`find.next`/`find.previous` to a benign **success no-op**
  unless `view_state().open` and the active prompt kind is `find` or `replace`,
  so a stray chord from a palette/settings prompt cannot mutate hidden find
  state. (`replace.all` is already guarded by F2a's destructive-op rule.)
- **Option indicators in both prompt kinds.** Add three toggle controls
  (case / whole-word / regex) seeded from `options` to **both** the find and
  replace `PromptRequest`s; `prompt_status_view` projects each toggle's `checked`
  from the authoritative `FindReplaceViewState.options`. The existing
  `paint_prompt`/`compute_prompt_layout` render the toggles (`[x]`/`[ ] label`)
  left-to-right on the bottom reserved row alongside the match count — the find
  prompt's row 1 and the replace prompt's row 2.

Oracle (F2b): a runtime test asserts `find.toggle_case` flips
`options.case_sensitive` and changes the match set for a mixed-case fixture, and
that dispatching `find.toggle_case` with no active find/replace prompt is a
benign success no-op (options unchanged). A keymap-validation test asserts the
assembled keymap has no errors or ambiguous prefixes with the new prompt-context
bindings. A render test asserts the find prompt's bottom row and the replace
prompt's third row display `[ ]`/`[x]` case/word/regex indicators reflecting
`options`, plus the match count, from the published view.

## Invariants

- M7-1 (library owns behavior): all find/replace and selection behavior is
  existing library command + controller state; the app adds only input decoding,
  keymap bindings, and cell rendering, and invents no editor semantics.
- M7-2 (published-state rendering): selection highlights, secondary carets, and
  find matches are painted from the published `SelectionViewState` /
  `FindReplaceViewState`; the renderer reads snapshot state and the client draws
  server-described cells (I7, I17).
- M7-3 (argument-free bindings): every new keymap binding is argument-free
  (dispatchable with an empty payload), consistent with the M6 K5 invariant.
- M7-4 (single hardware cursor): exactly one hardware cursor hint (`grid.caret`)
  is emitted, at the primary caret; all other carets are painted cells.
- M7-5 (find query authority): the authoritative find query and replacement live
  in the controller and drive the published match set and active index. The
  in-process TUI dispatches synchronously and holds **no** client-side query copy;
  it renders the query, replacement, and matches from the published
  `FindReplaceViewState`. (Optimistic client echo for distant clients is deferred
  and would layer on this without a second authority.)

## Considerations

- Selection highlight painting must clip to the document content rect and the
  visible viewport rows, reusing the existing row/cell mapping in
  `paint_document`; off-screen selections are not painted.
- Painting precedence in a cell: active find match (`selection` role) > find match
  (`search_match` role) > selection highlight (`selection` role) > syntax/
  foreground text; the caret (hardware `grid.caret` at the primary, or a painted
  secondary caret cell using the `caret` role) sits on top. A single cell resolves
  to exactly one background role by this order. Both `search_match` and
  `selection` are existing cataloged 16-color roles; no color is computed, so the
  exactly-16-color theme authority (I22) is preserved. The active match reusing
  the `selection` role is acceptable for M7 because find is modal (prompt focus);
  a dedicated `active_search_match` role can be added later without changing this
  precedence.
- Secondary carets are painted as `caret`-role cell backgrounds (not a second
  hardware cursor), so `grid.caret` still emits exactly one hardware-cursor hint
  at the primary caret (M7-4). Painting a secondary caret cell with the `caret`
  role co-exists with selection/match backgrounds via the precedence above (caret
  on top).
- The find prompt reserves rows via `prompt_row_count` (find=2, replace=3), which
  already shrinks the document area; the match highlights render in the
  (smaller) document area, consistent with the existing prompt reservation.
- Multi-caret edits already coalesce into one undo entry via the existing
  transaction path; M7 does not change history behavior.
- Mouse-driven selection/caret-add is M8 (mouse), not M7.

## Risks and mitigations

- **Invisible selection regression surface.** Adding selection painting touches
  the hot `paint_document` loop; a render golden/property test pins the
  single-selection and no-selection cases so ordinary text rendering is
  unchanged when there is no selection.
- **Caret precedence bugs.** A render test asserts the precedence order on a cell
  that is simultaneously a find match and inside a selection.
- **Modified-arrow decode ambiguity.** The `ESC [ 1 ; m X` form shares its prefix
  with plain `ESC [ X`; the decoder must treat `ESC [ 1` and each partial
  parameter as pending until the full parameter+final byte arrives, and fall back
  cleanly. A decode test covers the split-read case at each boundary.

## Acceptance (Definition of Done)

- Observable in the TUI: Shift+arrows extend a **visible** selection; an
  add-cursor chord places a **visible** second caret and typing edits at both;
  find opens a query prompt, matches highlight as you type, and next/previous move
  the active match; replace changes the current/all matches.
- Gates: `cmake --build build` clean; `ctest --preset dev` green; full
  `ctest -E performance_measurement` green.
- Oracles: per-step Oracle column below.

## Plan

| Step | Work | Files | Oracle |
|---|---|---|---|
| S | Paint selection highlights for all selections and secondary carets as `caret`-role cells; keep `grid.caret` at the primary; apply the cell precedence order | `src/render.cpp`, `tests/test_render.cpp` | render test (exact cells): ranged + multi-line selection cells carry `selection` role with correct byte→cell mapping across a wide/UTF-8 glyph and end-of-line fill; `grid.caret` at primary; only the secondary caret cell carries `caret`; off-viewport selection clipped; no-selection render byte-identical to before |
| D | Decode modified arrows/home/end (`ESC [ 1 ; m {A-D,H,F}`, modifier = 1+bitmask: Shift/Alt/Ctrl) into flagged strokes; plain arrows unchanged; pending on every partial-parameter read | `apps/ssg_terminal.{h,cpp}`, `tests/test_ssg_app.cpp` | decode table (exact strokes) for Shift/Ctrl/Alt/Ctrl+Shift arrows, Shift+Home/End, unsupported-modifier fallback, and split-read incompleteness at each boundary |
| M | Bind `Shift+Arrow`→`select.left/right/line_up/line_down` (editor) and `[Escape, KeyA/KeyD/KeyI/KeyK/KeyJ]`→`select.all/add_next_occurrence/split_into_lines/add_cursor_up/add_cursor_down` plus `[Escape, Slash]`→`find.open`, `[Escape, KeyR]`→`replace.open` (*); settings binding unchanged | `src/editor_runtime.cpp`, `tests/runtime/test_runtime_snapshot.cpp` | resolution test: `Shift+ArrowRight@editor`→`select.right`, `Shift+ArrowUp@editor`→`select.line_up`, `[Escape,KeyD]@editor`→`select.add_next_occurrence`, `[Escape,Slash]`→`find.open`, `[Escape,KeyF,KeyT]`→`settings.open` (unchanged); keymap-validation test: assembled keymap has no errors; runtime test: add-cursor chord command yields >1 selection; K5 argument-free test green |
| F1 | Add `find.update_query` (string payload, `keymap:false`/`palette:false`/`lua:true`) + catalog cascade; `find.open` opens a one-input `PromptKind::find` prompt; `prompt_status_view` projects `FindReplaceViewState.query`+match-count into the reserved rows at snapshot time; client edits the query no-copy (next = published query ± event) and fulfils, in find focus, `Enter`/`ArrowDown`→`find.next`, `ArrowUp`→`find.previous`, `[Escape,Escape]`→`find.close`; paint matches (`search_match`) + active match (`selection`) in `paint_document` | `include/ssg/find_replace.h`/`src/find_replace.cpp`, `src/runtime/editing.cpp` (bind + open), `src/runtime/snapshot.cpp` (project query+match-count into find prompt rows), `data/required-commands.json`, `tests/test_required_commands.cpp`, `tests/runtime/command_cases.h`, `src/protocol.cpp` (+ round-trip test in `tests/test_protocol.cpp`), `src/editor_runtime.cpp` (keymap `[Escape,Slash]`), `src/render.cpp`, `apps/ssg_main.cpp`, `tests/runtime/test_runtime_editing.cpp`, `tests/test_render.cpp` | runtime: `find.update_query("cat")` over three hand-authored offsets yields exactly those three match ranges and the find prompt rows project `"cat"`+`1/3`; `find.next` advances active 0→1→2→0; render: exact match cells carry `search_match`, active-match cells carry `selection`, incl. clipped + wide-glyph; round-trip; PTY: type query → matches highlight, Enter advances |
| F2a | Add `FindReplaceViewState.replacement` (published + serialized) + `FindReplaceController::update_replacement` + `replace.update_replacement` command (string payload `FindQueryArguments`, `keymap:false`/`palette:false`/`lua:true`) + catalog cascade; `replace.open` opens the three-row `PromptKind::replace` prompt seeded with the current query and sets `replace_mode`; `prompt_status_view` projects query→row 0 and replacement→row 1; `paint_prompt` puts the cursor on the replacement row; `replace.current`/`replace.all` source `view_state().replacement`; guard `replace.update_replacement`/`replace.current`/`replace.all` to a success no-op unless `open && replace_mode && active prompt kind == replace`; reveal the successor match after replace; client (replace focus): text/Backspace→`replace.update_replacement` (replacement only; query display-only), `Enter`→`replace.current`, `ArrowDown/Up`→`find.next`/`find.previous`, `[Escape,Escape]`→`find.close` | `include/ssg/find_replace.h`/`src/find_replace.cpp`, `src/runtime/editing.cpp` (bind + open + guards + reveal), `src/runtime/snapshot.cpp` (project replacement + cursor row), `data/required-commands.json`, `tests/test_required_commands.cpp`, `tests/runtime/command_cases.h`, `src/protocol.cpp` (view-state `replacement` codec + `replace.update_replacement` arg codec, + round-trip/delta tests in `tests/test_protocol.cpp`), `src/render.cpp`, `apps/ssg_main.cpp` (replace fulfilment), `tests/runtime/test_runtime_editing.cpp`, `tests/test_render.cpp` | runtime: query `"cat"` over `"cat cat cat"`, `replace.update_replacement("dog")` → `view_state().replacement=="dog"` and replace row 1 projects `"dog"`; `replace.current` → doc `"dog cat cat"`, `matches.size()==2`, `[0,3)` no longer matches, `active_match==0` at `[4,7)`; fresh `replace.all` → `"dog dog dog"`, matches empty; the three guarded commands are benign success no-ops with no active replace prompt; protocol round-trip + delta replay preserve `replacement`; render: replace rows show query (row 0) + replacement (row 1) + cursor on row 1 |
| F2b | Bind `[Escape,KeyC/KeyG/KeyE]`→`find.toggle_case`/`find.toggle_whole_word`/`find.toggle_regex` and `[Escape,KeyL]`→`replace.all` in `prompt` context; guard `find.toggle_*`/`find.next`/`find.previous` to a success no-op unless `open && active prompt kind ∈ {find,replace}`; add three toggle controls seeded from `options` to both find and replace `PromptRequest`s; `prompt_status_view` projects each toggle's `checked` from `options` | `src/editor_runtime.cpp` (keymap), `src/runtime/editing.cpp` (guards), `src/runtime/editing.cpp`/`src/runtime/snapshot.cpp` (toggle controls + projection), `tests/runtime/test_runtime_editing.cpp`, `tests/runtime/test_runtime_snapshot.cpp`, `tests/test_render.cpp` | runtime: `find.toggle_case` flips `options.case_sensitive` and changes the match set on a mixed-case fixture; `find.toggle_case` with no active find/replace prompt is a benign success no-op; keymap-validation: assembled keymap has no errors/ambiguous prefixes with the new bindings; render: find bottom row and replace third row show `[ ]`/`[x]` case/word/regex indicators + match count from the published view |
