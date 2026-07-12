# tab-management

- Spec: `doc/features/recovery-file-lifecycle.md`, Plan 3
- Depends: `file-commands`, `session-state`
- Branch: `tab-management-task`

## Scope

Implement tab mode/dirty/recovery badges, activate/next/previous, reorder,
close/close-others/close-all, recently-closed, and reopen commands over existing
file identities.

## Files

`include/ssg/tabs.h`, `src/tabs.cpp`, `tests/test_tabs.cpp`,
`cmake/components/tab-management.cmake`

## Oracle

Hand-authored tab transition tables cover duplicate identity, dirty close,
reopen, ordering, active-tab selection, and bounded recently-closed eviction.

## Done

The mandatory workflow is complete without file I/O implementation changes.
