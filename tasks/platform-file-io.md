# platform-file-io

- Spec: `doc/features/recovery-file-lifecycle.md`, Plan 1
- Depends: `foundation-harness`
- Branch: `platform-file-io-task`

## Scope

Implement Linux/Windows path validation, stable identity, locking, permissions,
cache roots, and atomic same-directory replacement.

## Files

`include/ssg/platform_files.h`, `src/platform/linux_files.cpp`,
`src/platform/windows_files.cpp`, `tests/test_platform_files.cpp`,
`cmake/components/platform-file-io.cmake`

## Oracle

Equivalent temporary-directory scripts on Linux and Windows, including Windows
reserved names, invalid characters, trailing dots/spaces, and long-path policy.

## Done

The mandatory workflow is complete without document encoding or recovery.
