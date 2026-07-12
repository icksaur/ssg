# edit-command-suite

- Spec: `doc/features/core-editing.md`, Plan 3
- Depends: `text-input-commands`, `core-websocket-slice`
- Branch: `edit-command-suite-task`

## Scope

Implement indent/outdent, line duplicate/move/delete/join, comment toggle, case
transforms, sorting, and transpose as one immutable
`EditCommandSuiteCommandSet` plus a pure apply function. Matching-bracket
selection belongs to `selection-navigation`; stateful registration, clipboard,
and history coalescing are out of scope.

## Files

`include/ssg/edit_commands.h`, `src/edit_commands.cpp`,
`tests/test_edit_commands.cpp`, `cmake/components/edit-command-suite.cmake`

## Oracle

Every normative transform command compares against independent hand fixtures
for single/multiple selections, LF/CRLF/CR and final-unterminated input,
tabs/spaces, comment settings, invalid settings/selections, non-edit modes, and
explicit no-op atomicity. ASCII case and UTF-8 grapheme transpose fixtures pin
the deterministic boundaries from the feature contract.

## Done

The mandatory workflow is complete without clipboard or history coalescing.
