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

## Files

`include/ssg/ui_layout.h`, `src/ui_layout.cpp`,
`data/ui/status_fields.json`, `tests/fixtures/ui_layout/`,
`tests/test_ui_layout.cpp`, `cmake/components/shell-layout.cmake`

## Oracle

Hand-authored geometry/accessibility goldens and non-overlap/cardinality
properties across supported grid sizes.

## Done

The mandatory workflow is complete without prompt/status queue behavior.
