# spec-presentation-shell

## Goals

Emit renderer-neutral wrapped text, scrollbars, hit targets, fixed shell geometry, accessible labels, and exactly 16 authoritative colors for every client.

## Design

The backend emits cell runs, semantic roles, rectangles, and hit-test metadata; clients render them. `Theme` is the only color source. Its scoped `SemanticRole` catalog and syntax-scope catalog are exhaustive, unknown syntax scopes resolve to the cataloged plain-text role, and every theme maps both catalogs to palette indices. The shared co-visibility relation in `tests/fixtures/theme_roles.json` applies to every theme rather than being theme-defined. Theme snapshots expose palette and mappings in index/catalog order so equal themes produce byte-for-byte deterministic snapshots. The shell, collapse priorities, wrap behavior, and per-client viewport rules are defined in `doc/spec.md`.

Normative commands owned by this feature:

- `pane.split_horizontal`, `pane.split_vertical`, `pane.close`, `pane.next`, `pane.previous`, `pane.focus_left`, `pane.focus_right`, `pane.focus_up`, `pane.focus_down`
- `panel.toggle`, `panel.focus`, `panel.next_provider`, `panel.previous_provider`
- `view.toggle_distraction_free`

The `shell-layout` task owns the pane, panel, and distraction-free commands
above. The later `prompt-status-surface` task owns `prompt.submit`,
`prompt.cancel`, `status.next`, `status.previous`, `status.dismiss`, and
`status.invoke_action`.

`PromptSurface` is a non-modal one-to-three-row view below the shared tab bar. Path prompts use one input; find uses one input plus toggles/count; replace uses find and replacement inputs plus toggles/count. Footer statuses are a bounded priority queue rather than one lossy slot.

## Shell layout contract

The minimum supported viewport is 20 columns by 4 rows; smaller viewports
produce a typed `viewport_too_small` result and no shell snapshot. Normal mode
uses one full-width header row, one full-width footer row, and a middle region
with one shared tab row. Distraction-free mode suppresses header, footer, tab
row, and panel nodes and gives the full viewport to editor panes; it does not
change pane or panel state.

Prompt/status behavior is not part of shell layout. Layout accepts an opaque
reserved prompt-row count from zero through three and reserves those rows
below the tab row. Region collapse is separate from field collapse: the panel
targets 24 columns, has a 12-column minimum, and collapses before the editor
would become narrower than 20 columns. Pane topology remains authoritative;
when a client viewport cannot give every pane at least one content cell and a
one-column scrollbar, that client shows only the active pane.

`data/ui/status_fields.json` is an array of objects with `id`, `region`,
`collapse_rank`, and non-empty `accessible_label`. Lower ranks are retained
first. Header order is active command/palette query, current path, then mode.
Footer order is actionable status, follow state/resume binding, background
activity, encoding, line ending, Git branch, Git repository, file type, then
file size.

Accessible shell nodes comprise the header and visible fields, footer and
visible fields/actions, shared tab row and tabs, panel/provider, visible panes,
each pane scrollbar, prompt reservation, and empty-state surface. Every node
has a non-empty label and `SemanticRole`. Shell layout reserves scrollbar
columns only; thumb/track geometry belongs to `viewport-wrap-scrollbar`.

## Invariants

I7, I8, I15, I17, I22, I23 from `doc/spec.md`.

## Considerations

- Zoom/font size changes client viewport dimensions only.
- Wrapped visual rows never change document line identity.
- Every status/action node has a non-empty accessible label.
- Every cursor, selection, edit, undo/redo, and find-result transition keeps the primary caret visible in each displaying viewport.

## Risks and Mitigations

- Client disagreement: pin Unicode and geometry fixtures.
- Hidden colors: scan source/config and property-check snapshots.

## Acceptance (Definition of Done)

- Observable: equal viewport inputs produce equal shell/cell snapshots across clients.
- Budgets: unchanged viewports emit no cell-run payload.
- Gates: layout/theme tests and browser accessibility snapshots are green.
- Oracles: the official Unicode 15.0.0 `GraphemeBreakTest.txt` corpus, hand-authored Unicode/wrap/geometry/scrollbar goldens, accessibility snapshots, and color-origin properties.

## Cell-width rules (normative, referenced by Plan steps 1a and 1b)

Unicode version pinned at **15.0.0**. Grapheme cluster boundaries follow **UAX #29 extended grapheme clusters** using a full GCB property state machine sourced from `data/unicode/GraphemeBreakProperty.txt` (generated via `tools/gen_gcb_table.py`). Rules implemented: GB6–GB8 (Hangul), GB9 (× Extend/ZWJ), GB9a (× SpacingMark), GB9b (Prepend ×), GB11 (ExtPic Extend* ZWJ × ExtPic), GB12/GB13 (RI × RI). Width follows **UAX #11 East Asian Width** (`EAW=W` or `EAW=F`) plus **Emoji_Presentation** and **Emoji+VS-16 sequence** rules.

Per-cluster width rules (applied to the **effective base** code point after Prepend absorption):

- **EAW = W or F** (`is_eaw_wide`, from `data/unicode/east_asian_width.txt` via `tools/gen_eaw_table.py`): 2 cells.
- **Emoji_Presentation = Yes** (`is_emoji_pres`, from `emoji-data.txt`): 2 cells. This covers Regional Indicators (U+1F1E6–U+1F1FF, EAW=N) and all other characters whose default presentation is emoji-style.
- **Emoji + VS-16 sequence:** a cluster whose effective base has `Emoji=Yes` (from `emoji-data.txt`) and into which U+FE0F (Variation Selector 16) was absorbed (via GB9) → 2 cells. This upgrades text-default emoji such as `#` (U+0023), `*` (U+002A), `0`–`9` (U+0030–U+0039), U+2702 ✂ (BLACK SCISSORS), and similar characters.
- **All other printable code points:** 1 cell (EAW=N, Na, H, A, or unassigned).

Per-code-point rules:

- **Printable ASCII (U+0020–U+007E):** 1 cell (covered by the last rule above, since none are EAW=W/F or Emoji_Presentation).
- **Horizontal tab (U+0009):** advances to the next column index that is a multiple of `tab_width` relative to the logical line start (cell 0); minimum advance 1 cell.
- **GCB=Control:** each code point is its own cluster (NOT absorbed into a preceding cluster). Two sub-cases by Unicode general category:
  - **C0 (U+0000–U+001F), DEL (U+007F), C1 (U+0080–U+009F):** `kind=control, width=1` (visible replacement glyph).
  - **Non-C0/C1 GCB=Control (cp > U+009F):** `kind=control, width=0` (Unicode Cf format characters: U+00AD Soft Hyphen, U+200B ZWSP, U+202A–U+202E bidi controls, U+2060–U+206F Word Joiners, U+FEFF BOM, and others listed in `GraphemeBreakProperty.txt` as GCB=Control).
- **GCB=Extend, ZWJ, SpacingMark:** extends the preceding grapheme cluster (GB9/GB9a). A lone Extend/ZWJ/SpacingMark at line start is a cluster: `kind=text, width=N` if the code point has nonzero display width (e.g. wide emoji modifiers U+1F3FB–U+1F3FF, EAW=W, produce `kind=text, width=2`); otherwise `kind=combining, width=0`. This preserves the invariant: `CellKind::combining` always has `cell_width=0`.
- **GCB=Prepend:** 0 cells until a following non-control code point is absorbed (GB9b), at which point the cluster takes the following code point's width. A lone Prepend at end-of-line has width 0.
- **Invalid UTF-8:** each maximal invalid byte unit yields **one** replacement glyph of 1 cell; every byte of the malformed unit is reported individually, one span per byte.

A grapheme cluster's display width equals its effective base code point's width (after VS-16 upgrade if applicable). Combining/zero-width extending code points within the cluster contribute 0 additional cells.

**Data sources and separation of concerns:**
- `k_extpic[]` (Extended_Pictographic) is used for GB11 segmentation only — never for width.
- `k_wide[]` (EAW=W/F) is used for width only — not for segmentation.
- `k_emoji_pres[]` (Emoji_Presentation) is used for width only.
- `k_emoji[]` (Emoji property) is used for VS-16 sequence detection only.
These must not be conflated.

**GB11 state machine:** `GB11State ∈ { None, ExtPic, Zwj }`. Initial state: `ExtPic` if the cluster base is Extended_Pictographic (per `emoji-data.txt`), else `None`. Transitions on GB9 Extend absorption: `ExtPic→ExtPic`, `Zwj→None`, `None→None`. Transitions on GB9 ZWJ absorption: `ExtPic→Zwj`, `Zwj→None`, `None→None`. GB9a SpacingMark absorption: any state → `None`. GB11 fires only when `state==Zwj` and the next code point is Extended_Pictographic; result state → `ExtPic`.

The `compute_cell_run` function is a **pure function** taking a `std::string_view` (one logical line, must not contain `\n` or `\r`) and an `int tab_width` in `[1, 16]`. It returns a `CellRun` containing one `CellSpan` per grapheme cluster. `CellIndex` (from `types.h`) identifies a zero-based column in the unwrapped logical line. No `LayoutViewState`, `LayoutDelta`, or viewport type is introduced by this step; those belong to the `viewport-wrap-scrollbar` task (Wave 2).

`include/ssg/layout.h` and `src/layout.cpp` are owned by `unicode-cell-layout` (Wave 1) and extended additively by `viewport-wrap-scrollbar` (Wave 2). They are not file-disjoint but are dependency-ordered, so the parallel constraint does not apply.

## Plan

| # | Step | Task | Files | Oracle | Invariants |
|---|------|------|-------|--------|------------|
| 1a | Implement grapheme segmentation and per-logical-line cell runs (no wrapping, no scrollbar, no viewport) | `unicode-cell-layout` (Wave 1) | `include/ssg/layout.h`, `src/layout.cpp`, `data/unicode/`, `tests/fixtures/layout/cells/`, `tests/test_cell_layout.cpp`, `tests/test_gcb_oracle.cpp`, `cmake/components/unicode-cell-layout.cmake` | Official Unicode 15.0.0 `GraphemeBreakTest.txt` corpus plus hand-authored combining, emoji, double-width, tab, control, and invalid-UTF-8 cell-run goldens | I7 |
| 1b | Implement wrap model and scrollbar model (adds wrap and scrollbar to layout.h/layout.cpp) | `viewport-wrap-scrollbar` (Wave 2) | `include/ssg/layout.h`, `src/layout.cpp`, `tests/test_layout.cpp` | Unicode/wrap/scrollbar goldens | I7 |
| 2 | Implement fixed shell, prompt/status queue geometry, caret reveal, and accessible labels | `shell-layout` (Wave 2) | `include/ssg/ui_layout.h`, `src/ui_layout.cpp`, `data/ui/status_fields.json`, `tests/test_ui_layout.cpp` | rectangle, prompt, queue, caret-visibility, and accessibility goldens | I15, I17, I23 |
| 3 | Implement the sole-source 16-color theme model | `theme-model` (Wave 1) | `include/ssg/theme.h`, `src/theme.cpp`, `data/themes/*`, `tests/fixtures/theme_roles.json`, `tests/test_theme.cpp` | exact indexed cardinality; exhaustive semantic/syntax mappings; shared co-visible-role distinctness; deterministic snapshots; source/config scans rejecting literal or computed colors outside theme data | I8, I22 |

## Rationale (optional, skippable)

Layout and colors are presentation data contracts, not rendering implementations.
