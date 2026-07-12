# scratch-session-locking

- Spec: `doc/features/recovery-file-lifecycle.md`, Plan 2
- Depends: `platform-file-io`
- Branch: `scratch-session-locking-task`

## Scope

Implement workspace-hash/session-ID namespaces, Linux/Windows OS locks, live
session isolation, crash-remnant discovery, and newest-restorable selection.

## Files

`include/ssg/scratch_session.h`, `src/scratch_session.cpp`,
`tests/test_scratch_session.cpp`,
`cmake/components/scratch-session-locking.cmake`

## Contract

- This component consumes `try_lock_file` and its move-only
  `ExclusiveFileLock`. It does not add another operating-system lock primitive
  or platform lock translation unit.
- Callers provide an explicit scratch root and an already-canonical absolute
  workspace path. Default cache-root lookup and workspace canonicalization
  belong to their composing layers.
- Workspace directories are keyed by lowercase SHA-256 of the canonical
  workspace path's native UTF-8 representation. Session IDs contain a
  fixed-width UTC creation timestamp followed by 128 random bits, so lexical
  session-ID order is a total newest-first order.
- The namespace is
  `<root>/workspaces/<workspace-hash>/sessions/<session-id>/`, with
  files named **session.lock**, **journal.bin**, and an optional **restored**
  marker. Every created directory and file is owner-only.
- A remnant is restorable when it is unlocked, has no **restored** marker, and
  replaying **journal.bin** yields at least one document. This component
  consumes the existing journal replay API but does not add journal encoding.
- Selection locks the chosen remnant and returns a move-only claim. Concurrent
  selectors therefore cannot choose it twice. After successful import, the
  caller marks the claim restored atomically; a crash before that marker leaves
  the remnant eligible for retry. Cleanup and quota policy remain Plan 2c.

## Oracle

Concurrent-process, crash-release, stale-remnant, multiple-workspace, and
newest-unlocked selection scripts on Linux/Windows.

## Done

The mandatory workflow is complete without journal encoding or cleanup policy.
