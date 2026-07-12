# clipboard-register

- Spec: `doc/features/core-editing.md`, Plan 4
- Depends: `edit-command-suite`, `undo-redo-history`
- Branch: `clipboard-register-task`

## Scope

Implement structured internal copy/cut/paste, empty-selection line behavior,
multi-caret distribution, best-effort system requests, and stale responses.

## Files

`include/ssg/clipboard.h`, `src/clipboard.cpp`,
`tests/test_clipboard.cpp`, `cmake/components/clipboard-register.cmake`

## Oracle

Hand-authored fragment distribution and line-copy cases, plus denied,
disconnected, and stale browser response scripts with undo round trips.

## Done

The mandatory workflow is complete without browser API calls in the backend.
