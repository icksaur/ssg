# edit-history-integration

- Spec: `doc/features/core-editing.md`; invariant I5
- Depends: `text-input-commands`, `edit-command-suite`, `undo-redo-history`,
  `clipboard-register`, `find-replace`
- Branch: `edit-history-integration-task`

## Scope

Provide the reusable history-routing seam for text-input and edit-command
mutations. Verify that this seam and the already history-integrated clipboard
and find-replace mutations produce the required typing/delete coalescing and
newline, paste, cut, line-transform, and replace boundaries.

## Files

`include/ssg/edit_history_integration.h`,
`src/edit_history_integration.cpp`,
`tests/test_edit_history_integration.cpp`,
`cmake/components/edit-history-integration.cmake`

## Oracle

Deterministic-clock scripts compare complete command sequences against
hand-authored undo boundaries and verify selection/content round trips.

## Done

The mandatory workflow is complete without adding new edit behavior.
