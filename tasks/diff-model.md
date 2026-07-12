# diff-model

- Spec: `doc/features/workspace-live-diffs.md`, Plan 3
- Depends: `filesystem-watchers`
- Branch: `diff-model-task`

## Scope

Implement Git-index and seeded non-Git line diff models with stable identity,
rename/delete retention, revision-tagged results, and the
`diff.next_hunk`, `diff.previous_hunk`, and `diff.open_file` command set.

## Files

`include/ssg/diff.h`, `src/diff.cpp`, `tests/fixtures/diff/`,
`tests/test_diff.cpp`, `cmake/components/diff-model.cmake`

## Oracle

Applying emitted hunks reconstructs target content; normalized changed-line
sets match independent tracked/untracked/rename/delete/index fixtures.

## Done

The mandatory workflow is complete without follow navigation.
