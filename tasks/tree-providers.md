# tree-providers

- Spec: `doc/features/workspace-live-diffs.md`, Plan 1
- Depends: `foundation-harness`
- Branch: `tree-providers-task`

## Scope

Implement the generic stable-ID tree provider model and filesystem/Git/symbol
provider deltas without filesystem watching.

## Files

`include/ssg/tree.h`, `src/tree.cpp`, `tests/test_tree.cpp`,
`cmake/components/tree-providers.cmake`

## Oracle

Temporary-directory and hand-authored provider snapshots verify expansion,
stable identity, commands, and delta replay.

## Done

The mandatory workflow is complete for tree data only.
