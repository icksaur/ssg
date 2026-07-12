# filesystem-watchers

- Spec: `doc/features/workspace-live-diffs.md`, Plan 2
- Depends: `platform-file-io`
- Branch: `filesystem-watchers-task`

## Scope

Implement inotify and `ReadDirectoryChangesW` adapters plus normalized
create/modify/rename/delete/overflow/coalescing behavior.

## Files

`include/ssg/watcher.h`, `src/watcher.cpp`,
`src/platform/linux_watcher.cpp`, `src/platform/windows_watcher.cpp`,
`tests/test_watcher.cpp`, `cmake/components/filesystem-watchers.cmake`

## Oracle

Identical normalized event scripts on Linux/Windows, save-correlation cases,
overflow/rescan fixtures, bounded-queue properties, and finite-time shutdown.
The watcher marks exact registered save results as `ssg_save`; the dependent
external-modification flow owns the resulting document/status behavior.

## Done

The mandatory workflow is complete without diff computation.
