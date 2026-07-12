# tab-management

- Spec: `doc/features/recovery-file-lifecycle.md`, Plan 4b
- Depends: `file-commands`, `session-state`
- Branch: `tab-management-task`

## Scope

Implement tab mode/dirty/recovery badges, activate/next/previous, reorder,
close/close-others/close-all, recently-closed, and reopen commands over existing
file identities. Export the immutable `TabManagementCommandSet`, typed
`TabViewState`, and pure `TabDelta` derivation/replay seam. This task solely owns
the nine normative `tab.*` command IDs listed in the feature spec; file commands
do not own them.

All tab kinds participate in topology commands. Duplicate saved-path or
untitled identity activates the existing document tab. Dirty close delegates to
the injected recovery/durability lifecycle boundary and is failure-atomic.
Batch closes are left-to-right best effort. Active-tab replacement,
recently-closed LIFO/32-entry eviction, reopen insertion/activation, and
`Untitled N` allocation follow the feature spec.

## Files

`include/ssg/tabs.h`, `src/tabs.cpp`, `tests/test_tabs.cpp`,
`cmake/components/tab-management.cmake`

## Oracle

Hand-authored tab transition tables cover duplicate identity, dirty close,
durability rejection, batch partial failure, reopen, ordering, active-tab
selection, untitled labels, all tab kinds, and bounded recently-closed eviction.

## Invariants

I2, I15, I16, and I19.

## Done

The mandatory workflow is complete without file I/O implementation changes;
all lifecycle effects are exercised through an injected test boundary.
