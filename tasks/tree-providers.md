# tree-providers

- Spec: `doc/features/workspace-live-diffs.md`, Plan 1
- Depends: `foundation-harness`
- Branch: `tree-providers-task`

## Scope

Implement the generic stable-ID tree provider model, explicit deterministic
filesystem scans, caller-fed Git/symbol snapshots, expansion persistence, and
bounded provider deltas without filesystem watching, repository access, or
Tree-sitter parsing. Export immutable `TreeCommandSet`, `TreeViewState`, and
`TreeDelta` values. This task owns `tree.toggle_expanded` and
`tree.invoke_node_command`.

## Files

`include/ssg/tree.h`, `src/tree.cpp`, `tests/test_tree.cpp`,
`cmake/components/tree-providers.cmake`

## Oracle

Temporary-directory and hand-authored provider snapshots verify expansion,
stable identity, deterministic filesystem/Git/symbol ordering, exact command
ownership, and bounded delta replay against an independently derived full view.

## Done

The mandatory workflow is complete for tree data only.
