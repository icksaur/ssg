# piece-tree

- Spec: `doc/features/core-editing.md`, Plan 2
- Depends: `reference-editor`
- Branch: `piece-tree-task`

## Scope

Implement the private balanced piece tree with original/add buffers and subtree
byte/newline summaries, excluding public document transactions.

## Files

`src/piece_tree.h`, `src/piece_tree.cpp`, `tests/test_piece_tree.cpp`,
`cmake/components/piece-tree.cmake`

## Oracle

Randomized insert/erase/read/line-query sequences compare against
`std::string`, plus structural invariant properties.

## Done

The mandatory workflow is complete and storage does not leak into public APIs.
