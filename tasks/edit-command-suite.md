# edit-command-suite

- Spec: `doc/features/core-editing.md`, Plan 3
- Depends: `text-input-commands`, `core-websocket-slice`
- Branch: `edit-command-suite-task`

## Scope

Implement indent/outdent, line duplicate/move/delete/join, comment toggle, case
transforms, sorting, transpose, and bracket selection as command sets.

## Files

`include/ssg/edit_commands.h`, `src/edit_commands.cpp`,
`tests/test_edit_commands.cpp`, `cmake/components/edit-command-suite.cmake`

## Oracle

Every normative transform command compares against independent hand fixtures
for single/multiple selections and line-ending/indent settings.

## Done

The mandatory workflow is complete without clipboard or history coalescing.
