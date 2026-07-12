# recovery-actions

- Spec: `doc/features/recovery-file-lifecycle.md`, Plan 3
- Depends: `scratch-compaction-quota`
- Branch: `recovery-actions-task`

## Scope

Implement bounded compensating records for close, reload, overwrite, rename,
delete, and workspace replacement.

## Files

`include/ssg/recovery.h`, `src/recovery.cpp`,
`tests/test_recovery.cpp`, `cmake/components/recovery-actions.cmake`

## Oracle

Each major action followed by its compensating command restores the accepted
canonical filesystem/document state under injected failures.

## Done

The mandatory workflow is complete without tab UI integration.
