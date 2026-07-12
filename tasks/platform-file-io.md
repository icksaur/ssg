# platform-file-io

- Spec: `doc/features/recovery-file-lifecycle.md`, Plan 1
- Depends: `foundation-harness`
- Branch: `platform-file-io-task`

## Scope

Implement Linux/Windows path validation, stable identity, locking, permissions,
cache roots, and atomic same-directory replacement.

This task owns the one shared move-only RAII OS advisory-lock primitive.
`scratch-session-locking` consumes it and owns session namespace and remnant
selection policy. Path validation here is syntactic; Plan 4 owns
canonicalization, symlink traversal, and CWD-boundary enforcement.

## Files

`include/ssg/platform_files.h`, `src/platform/linux_files.cpp`,
`src/platform/windows_files.cpp`, `tests/test_platform_files.cpp`,
`cmake/components/platform-file-io.cmake`

## Oracle

A platform-independent hand-authored Linux/Windows path decision table runs on
both platforms, including Windows reserved names, invalid characters, trailing
dots/spaces, and the pinned legacy/extended long-path limits. Equivalent native
temporary-directory scripts cover identity stability across reopen/rename,
distinct-file identity, lock contention/release, owner-only permissions, cache
roots, and atomic replacement that yields only complete old or new bytes.

## Invariants

I4 and I21.

## Done

The mandatory workflow is complete without document encoding or recovery.
