# document-transactions

- Spec: `doc/features/core-editing.md`, Plan 3
- Depends: `piece-tree`, `reference-editor`
- Branch: `document-transactions-task`

## Scope

Expose UTF-8 document modes, revisions, atomic non-overlapping transactions,
dirty state, and canonical snapshots over the private piece tree.

`Document` owns a monotonic content revision but no undo/redo storage in this
task. History remains Plan 6; each accepted document transaction is one atomic
boundary that the history layer can record. A transaction carries its observed
base revision and is rejected as stale unless it equals the current revision.

The public result distinguishes stale revision, read-only mode, diff mode,
empty transaction, invalid range, overlapping edits, invalid UTF-8, UTF-8
boundary splits, and revision exhaustion. Every rejection leaves text,
revision, and dirty state unchanged. A canonical document snapshot owns
`text`, `revision`, `mode`, and `dirty`. Documents start clean; the first
accepted transaction makes them dirty. Clearing dirty state belongs to the
later file-lifecycle/save integration.

Document mode is fixed at construction. Construction and inserted text require
well-formed UTF-8 without NUL; invalid file bytes are decoded or represented by
the encoding/file-lifecycle layer before constructing a read-only document.
Each edit contains a pre-transaction byte offset, erase length, and replacement
text. Edit ranges must be distinct, non-overlapping UTF-8 boundary ranges.

## Files

`include/ssg/document.h`, `src/document.cpp`, `tests/test_document.cpp`,
`cmake/components/document-transactions.cmake`

## Oracle

Reference-editor transaction scripts map each offset replacement to a
selection replacement, applied highest-offset first. Randomized multi-edit
snapshots compare text with that independent editor. Hand-authored properties
cover revision monotonicity, dirty state, canonical snapshot ownership,
read-only/diff rejection, stale base revisions, UTF-8 boundaries, and
failure atomicity.

## Done

The mandatory workflow is complete without selection commands or history.
