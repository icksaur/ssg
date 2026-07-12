# theme-model

- Spec: `doc/features/presentation-shell.md`, Plan 3
- Depends: `foundation-harness`
- Branch: `theme-model-task`

## Scope

Implement immutable exactly-16-color themes and semantic role mappings as the
only color source.

## Files

`include/ssg/theme.h`, `src/theme.cpp`, `data/themes/`,
`tests/fixtures/theme_roles.json`, `tests/test_theme.cpp`,
`cmake/components/theme-model.cmake`

## Oracle

Cardinality/property tests, co-visible role fixtures, and scans rejecting
literal or computed colors outside theme data.

## Done

The mandatory workflow is complete for theme values only.
