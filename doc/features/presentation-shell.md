# spec-presentation-shell

## Goals

Emit renderer-neutral wrapped text, scrollbars, hit targets, fixed shell geometry, accessible labels, and exactly 16 authoritative colors for every client.

## Design

The backend emits cell runs, semantic roles, rectangles, and hit-test metadata; clients render them. `Theme` is the only color source. The shell, collapse priorities, wrap behavior, and per-client viewport rules are defined in `doc/spec.md`.

Normative commands owned by this feature:

- `pane.split_horizontal`, `pane.split_vertical`, `pane.close`, `pane.next`, `pane.previous`, `pane.focus_left`, `pane.focus_right`, `pane.focus_up`, `pane.focus_down`
- `panel.toggle`, `panel.focus`, `panel.next_provider`, `panel.previous_provider`
- `view.toggle_distraction_free`
- `prompt.submit`, `prompt.cancel`
- `status.next`, `status.previous`, `status.dismiss`, `status.invoke_action`

`PromptSurface` is a non-modal one-to-three-row view below the shared tab bar. Path prompts use one input; find uses one input plus toggles/count; replace uses find and replacement inputs plus toggles/count. Footer statuses are a bounded priority queue rather than one lossy slot.

## Invariants

I7, I8, I15, I17, I18, I22 from `doc/spec.md`.

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

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Implement grapheme, cell, wrap, and scrollbar models | `include/ssg/layout.h`, `src/layout.cpp`, `tests/test_layout.cpp` | Unicode/wrap/scrollbar goldens | I7 |
| 2 | Implement fixed shell, prompt/status queue geometry, caret reveal, and accessible labels | `include/ssg/ui_layout.h`, `src/ui_layout.cpp`, `data/ui/status_fields.json`, `tests/test_ui_layout.cpp` | rectangle, prompt, queue, caret-visibility, and accessibility goldens | I15, I17, I23 |
| 3 | Implement the sole-source 16-color theme model | `include/ssg/theme.h`, `src/theme.cpp`, `data/themes/*`, `tests/test_theme.cpp` | cardinality, role, and literal-color properties | I8, I22 |

## Rationale (optional, skippable)

Layout and colors are presentation data contracts, not rendering implementations.
