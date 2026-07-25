# theme-model

- Spec: `doc/features/presentation-shell.md`, Plan 3
- Depends: `foundation-harness`
- Branch: `theme-model-task`

## Scope

Implement immutable exactly-16-color themes and semantic role mappings as the
only color source.

## Files

`include/ssg/theme.h`, `src/theme.cpp`, `src/DefaultTheme.cpp`,
`tests/fixtures/theme_roles.json`, `tests/test_theme.cpp`,
`cmake/components/theme-model.cmake`

## Oracle

Cardinality/property tests reject missing, duplicate, and extra palette indices;
exhaustive catalog tests require every semantic role and syntax scope exactly
once; the shared co-visible-role fixture applies to every theme; equal values
produce deterministic palette/catalog-ordered snapshots; and source/config
scans reject literal or computed colors outside theme data.

## Done

The mandatory workflow is complete for theme values only.
