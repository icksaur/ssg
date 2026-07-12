# reference-editor

- Spec: `doc/features/core-editing.md`, Plan 1
- Depends: `foundation-harness`
- Branch: `reference-editor-task`

## Scope

Implement only the independent string-based reference editor primitives and
their hand-computed mutation, selection, clipboard-transform, and history
cases. Later command tasks own local independent fixtures for commands not
defined here and must not expand this task's scope retroactively.

## Files

`tests/reference_editor.h`, `tests/reference_editor.cpp`,
`tests/test_reference_editor.cpp`, `cmake/components/reference-editor.cmake`

## Oracle

Paper-computed command scripts must pass before the reference editor may be used
to judge SSG.

## Done

The mandatory workflow is complete and no production SSG code is shared.
