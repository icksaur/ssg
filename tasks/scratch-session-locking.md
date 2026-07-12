# scratch-session-locking

- Spec: `doc/features/recovery-file-lifecycle.md`, Plan 2
- Depends: `platform-file-io`
- Branch: `scratch-session-locking-task`

## Scope

Implement workspace-hash/session-ID namespaces, Linux/Windows OS locks, live
session isolation, crash-remnant discovery, and newest-restorable selection.

## Files

`include/ssg/scratch_session.h`, `src/scratch_session.cpp`,
`src/platform/linux_scratch_lock.cpp`,
`src/platform/windows_scratch_lock.cpp`,
`tests/test_scratch_session.cpp`,
`cmake/components/scratch-session-locking.cmake`

## Oracle

Concurrent-process, crash-release, stale-remnant, multiple-workspace, and
newest-unlocked selection scripts on Linux/Windows.

## Done

The mandatory workflow is complete without journal encoding or cleanup policy.
