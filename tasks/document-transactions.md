# document-transactions

- Spec: `doc/features/core-editing.md`, Plan 2
- Depends: `piece-tree`, `reference-editor`
- Branch: `document-transactions-task`

## Scope

Expose UTF-8 document modes, revisions, atomic non-overlapping transactions,
dirty state, and canonical snapshots over the private piece tree.

## Files

`include/ssg/document.h`, `src/document.cpp`, `tests/test_document.cpp`,
`cmake/components/document-transactions.cmake`

## Oracle

Reference-editor transaction scripts, randomized multi-edit snapshots,
read-only/diff rejection, stale-position, and failure-atomicity cases.

## Done

The mandatory workflow is complete without selection commands or history.
