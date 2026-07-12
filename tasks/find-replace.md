# find-replace

- Spec: `doc/features/language-services.md`, Plan 1
- Depends: `search-palette`, `edit-command-suite`, `undo-redo-history`
- Branch: `find-replace-task`

## Scope

Implement incremental current-file find, options/count/highlights,
replace-current/all, workspace preview/apply, and recovery integration.

## Files

`include/ssg/find_replace.h`, `src/find_replace.cpp`,
`tests/test_find_replace.cpp`, `cmake/components/find-replace.cmake`

## Oracle

Independent matcher comparisons cover literal/regex/case/word/selection and
zero-width termination; replace and workspace preview/apply/recover round trip.

## Done

The mandatory workflow is complete with each replace operation atomic.
