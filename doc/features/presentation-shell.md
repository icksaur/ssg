# spec-presentation-shell

## Goals

Emit renderer-neutral wrapped text, scrollbars, hit targets, fixed shell geometry, accessible labels, and exactly 16 authoritative colors for every client.

## Design

The backend emits cell runs, semantic roles, rectangles, and hit-test metadata; clients render them. `Theme` is the only color source. Its scoped `SemanticRole` catalog and syntax-scope catalog are exhaustive, unknown syntax scopes resolve to the cataloged plain-text role, and every theme maps both catalogs to palette indices. The shared co-visibility relation in `tests/fixtures/theme_roles.json` applies to every theme rather than being theme-defined. Theme snapshots expose palette and mappings in index/catalog order so equal themes produce byte-for-byte deterministic snapshots. The shell, collapse priorities, wrap behavior, and per-client viewport rules are defined in `doc/spec.md`.

Normative commands owned by this feature:

- `pane.split_horizontal`, `pane.split_vertical`, `pane.close`, `pane.next`, `pane.previous`, `pane.focus_left`, `pane.focus_right`, `pane.focus_up`, `pane.focus_down`
- `panel.toggle`, `panel.focus`, `panel.next_provider`, `panel.previous_provider`
- `view.toggle_distraction_free`
- `prompt.submit`, `prompt.cancel`
- `status.next`, `status.previous`, `status.dismiss`, `status.invoke_action`

`PromptSurface` is a non-modal one-to-three-row view below the shared tab bar. Path prompts use one input; find uses one input plus toggles/count; replace uses find and replacement inputs plus toggles/count. Footer statuses are a bounded priority queue rather than one lossy slot.

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
- Oracles: hand-authored Unicode/wrap/geometry/scrollbar goldens, accessibility snapshots, and color-origin properties.

## Cell-width rules (normative, referenced by Plan steps 1a and 1b)

Unicode version pinned at **15.0.0**. Grapheme cluster boundaries follow **UAX #29 extended grapheme clusters** including ZWJ sequences (GB11), regional-indicator flag pairs (GB12/GB13), emoji modifier sequences, and variation-selector attachment. Width follows **UAX #11 East Asian Width** (EAW = Wide or Fullwidth → 2 cells) plus `emoji-data.txt` emoji-presentation sequences.

Per-code-point rules (applied to the **base** code point of each grapheme cluster):

- **Printable ASCII (U+0020–U+007E):** 1 cell.
- **Horizontal tab (U+0009):** advances to the next column index that is a multiple of `tab_width` relative to the logical line start (cell 0); minimum advance 1 cell.
- **C0 controls (U+0000–U+0008, U+000A–U+001F) and DEL (U+007F):** rendered as a visible 1-cell replacement glyph.
- **C1 controls (U+0080–U+009F):** rendered as a visible 1-cell replacement glyph.
- **Combining / zero-width code points** (General_Category Mn, Me, Cf zero-width; U+0300–U+036F and the full set from `data/unicode/combining_zero_width.txt`): 0 cells; the code point extends its preceding grapheme cluster.
- **Wide code points** (EAW = W or F, per `data/unicode/east_asian_width.txt`): 2 cells.
- **All other printable code points:** 1 cell.
- **Invalid UTF-8:** each maximal invalid byte unit (lone lead byte, overlong sequence lead, lone continuation byte, or truncated sequence lead at end-of-input) yields **one** replacement glyph of 1 cell; every byte of the malformed unit is reported individually, one span per byte.

A grapheme cluster's display width equals its base code point's width. Combining/zero-width extending code points within the cluster contribute 0 additional cells. A lone combining mark (no preceding base in the current logical line) is a cluster of kind `combining` with width 0.

The `compute_cell_run` function is a **pure function** taking a `std::string_view` (one logical line, must not contain `\n` or `\r`) and an `int tab_width` in `[1, 16]`. It returns a `CellRun` containing one `CellSpan` per grapheme cluster. `CellIndex` (from `types.h`) identifies a zero-based column in the unwrapped logical line. No `LayoutViewState`, `LayoutDelta`, or viewport type is introduced by this step; those belong to the `viewport-wrap-scrollbar` task (Wave 2).

`layout.h`/`layout.cpp` are owned by `unicode-cell-layout` (Wave 1) and extended additively by `viewport-wrap-scrollbar` (Wave 2). They are not file-disjoint but are dependency-ordered, so the parallel constraint does not apply.

## Plan

| # | Step | Task | Files | Oracle | Invariants |
|---|------|------|-------|--------|------------|
| 1a | Implement grapheme segmentation and per-logical-line cell runs (no wrapping, no scrollbar, no viewport) | `unicode-cell-layout` (Wave 1) | `include/ssg/layout.h`, `src/layout.cpp`, `data/unicode/`, `tests/fixtures/layout/cells/`, `tests/test_cell_layout.cpp`, `cmake/components/unicode-cell-layout.cmake` | Hand-authored combining, emoji, double-width, tab, control, and invalid-UTF-8 cell-run goldens (Unicode 15.0.0) | I7 |
| 1b | Implement wrap model and scrollbar model (adds wrap and scrollbar to layout.h/layout.cpp) | `viewport-wrap-scrollbar` (Wave 2) | `include/ssg/layout.h`, `src/layout.cpp`, `tests/test_layout.cpp` | Unicode/wrap/scrollbar goldens | I7 |
| 2 | Implement fixed shell, prompt/status queue geometry, caret reveal, and accessible labels | `shell-layout` (Wave 2) | `include/ssg/ui_layout.h`, `src/ui_layout.cpp`, `data/ui/status_fields.json`, `tests/test_ui_layout.cpp` | rectangle, prompt, queue, caret-visibility, and accessibility goldens | I15, I17, I23 |
| 3 | Implement the sole-source 16-color theme model | `theme-model` (Wave 1) | `include/ssg/theme.h`, `src/theme.cpp`, `data/themes/*`, `tests/fixtures/theme_roles.json`, `tests/test_theme.cpp` | exact indexed cardinality; exhaustive semantic/syntax mappings; shared co-visible-role distinctness; deterministic snapshots; source/config scans rejecting literal or computed colors outside theme data | I8, I22 |

## Rationale (optional, skippable)

Layout and colors are presentation data contracts, not rendering implementations.
