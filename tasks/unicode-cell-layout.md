# unicode-cell-layout

- Spec: `doc/features/presentation-shell.md`, Plan 1
- Depends: `foundation-harness`
- Branch: `unicode-cell-layout-task`

## Scope

Implement UTF-8 grapheme segmentation and deterministic terminal-cell runs
without wrapping, scrolling, shell geometry, or rendering.

## Files

`include/ssg/layout.h`, `src/layout.cpp`, `data/unicode/`,
`tests/fixtures/layout/cells/`, `tests/test_cell_layout.cpp`,
`cmake/components/unicode-cell-layout.cmake`

## Oracle

Pinned hand-authored combining, emoji, invalid-input, tab, control, and
double-width cell-run fixtures.

## Done

The mandatory workflow is complete and no renderer API is introduced.
