# selection-navigation

- Spec: `doc/features/core-editing.md`, Plan 3
- Depends: `document-transactions`, `viewport-wrap-scrollbar`
- Branch: `selection-navigation-task`

## Scope

Implement normalized selections, semantic cursor/range commands, word/line/page
movement, extend selection, multi-cursor creation including next occurrence,
matching-bracket selection, and caret reveal.

## Files

`include/ssg/selection.h`, `src/selection.cpp`,
`tests/test_selection.cpp`, `cmake/components/selection-navigation.cmake`

## Oracle

Reference-editor command scripts plus viewport intersection properties after
every movement or selection transition.

## Done

The mandatory workflow is complete without text transform commands.
