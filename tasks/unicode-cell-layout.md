# unicode-cell-layout

- Spec: `doc/features/presentation-shell.md`, Plan 1
- Depends: `foundation-harness`
- Branch: `unicode-cell-layout-task`

## Scope

Implement UTF-8 grapheme segmentation and deterministic terminal-cell runs
without wrapping, scrolling, shell geometry, or rendering.

## Files

`include/ssg/grapheme_layout.h`, `src/grapheme_layout.cpp`, `data/unicode/`,
`tests/fixtures/layout/cells/`, `tests/test_cell_layout.cpp`,
`cmake/components/unicode-cell-layout.cmake`

## Oracle

Hand-authored combining, emoji, invalid-input, tab, control, and double-width
cell-run goldens (`tests/test_cell_layout.cpp`), plus the official Unicode 15.0.0
GraphemeBreakTest.txt corpus (`tests/test_gcb_oracle.cpp`).

## Done

The mandatory workflow is complete and no renderer API is introduced.
