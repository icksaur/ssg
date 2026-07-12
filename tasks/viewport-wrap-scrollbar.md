# viewport-wrap-scrollbar

- Spec: `doc/features/presentation-shell.md`, Plan 1
- Depends: `unicode-cell-layout`
- Branch: `viewport-wrap-scrollbar-task`

## Scope

Add visual-row wrapping, viewport slicing, scrollbar metrics, scrolling, and
cell hit targets on top of deterministic cell runs.

## Files

`include/ssg/viewport.h`, `src/viewport.cpp`,
`tests/fixtures/layout/viewports/`, `tests/test_viewport.cpp`,
`cmake/components/viewport-wrap-scrollbar.cmake`

## Oracle

Hand-authored wrap/scrollbar/hit-target goldens and bounds properties for empty,
short, wide, wrapped, and tiny viewports.

## Done

The mandatory workflow is complete without shell or client rendering.
