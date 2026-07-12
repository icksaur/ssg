# undo-redo-history

- Spec: `doc/features/core-editing.md`, Plan 5
- Depends: `selection-navigation`, `document-transactions`
- Branch: `undo-redo-history-task`

## Scope

Implement bounded per-file history, compatible typing/deletion coalescing,
selection restoration, redo invalidation, and byte-budget eviction.

## Files

`include/ssg/history.h`, `src/history.cpp`, `tests/test_history.cpp`,
`cmake/components/undo-redo-history.cmake`

## Oracle

Deterministic-clock coalescing cases and forward/undo/redo canonical snapshot
round trips against the reference editor.

## Done

The mandatory workflow is complete without persistence across restart.
