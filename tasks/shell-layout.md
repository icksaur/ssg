# shell-layout

- Spec: `doc/features/presentation-shell.md`, Plan 2
- Depends: `unicode-cell-layout`, `theme-model`
- Branch: `shell-layout-task`

## Scope

Implement fixed header/middle/footer geometry, shared tab row, panels, panes,
per-pane scrollbar columns, collapse priorities, and accessible labels.
Own the `pane.*` and `panel.*` command sets and
`view.toggle_distraction_free`.
Prompt/status queue behavior is owned by `prompt-status-surface`; caret reveal
is owned by `selection-navigation`; this task owns their reserved rectangles
and label-bearing shell nodes only.

The minimum supported viewport is 20 columns by 4 rows. Smaller dimensions
produce `viewport_too_small` as a typed error and no snapshot. Prompt geometry
enters as an opaque reserved-row count in `[0, 3]`; this task validates and
reserves that rectangle but does not own prompt behavior.

Region collapse is independent of status-field collapse. The panel targets 24
columns, has a 12-column minimum, and is hidden when showing it would leave
fewer than 20 editor columns. If a pane topology cannot give every pane at
least one content cell plus its scrollbar column, only the active pane is
visible; topology is retained. `data/ui/status_fields.json` declares each
header/footer field's `id`, `region`, `collapse_rank`, and non-empty
`accessible_label`.

Label-bearing nodes are the header and its visible fields, footer and its
visible fields/actions, shared tab row and tabs, panel/provider, visible panes,
each pane scrollbar, prompt reservation, and empty-state surface. Non-status
labels come from typed shell inputs or stable built-in labels. Every node has a
`SemanticRole`. This task reserves one scrollbar column per visible pane,
including the empty-state pane; scrollbar thumb/track calculation remains
owned by `viewport-wrap-scrollbar`.

## Files

`include/ssg/ui_layout.h`, `src/ui_layout.cpp`,
`data/ui/status_fields.json`, `tests/fixtures/ui_layout/`,
`tests/test_ui_layout.cpp`, `cmake/components/shell-layout.cmake`

## Oracle

Hand-authored geometry/accessibility goldens and non-overlap/cardinality
properties across supported grid sizes.

## Done

The mandatory workflow is complete without prompt/status queue behavior.
