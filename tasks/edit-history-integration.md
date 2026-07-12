# edit-history-integration

- Spec: `doc/features/core-editing.md`; invariant I5
- Depends: `edit-command-suite`, `undo-redo-history`, `clipboard-register`, `find-replace`
- Branch: `edit-history-integration-task`

## Scope

Integrate every mutating command with transaction-to-undo-unit policy, including
typing/delete coalescing and newline, paste, cut, line transforms, and replace
boundaries.

## Files

`src/edit_history_integration.cpp`,
`tests/test_edit_history_integration.cpp`,
`cmake/components/edit-history-integration.cmake`

## Oracle

Deterministic-clock scripts compare complete command sequences against
hand-authored undo boundaries and verify selection/content round trips.

## Done

The mandatory workflow is complete without adding new edit behavior.
