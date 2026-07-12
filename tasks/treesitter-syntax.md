# treesitter-syntax

- Spec: `doc/features/language-services.md`, Plan 2
- Depends: `document-transactions`
- Branch: `treesitter-syntax-task`

## Scope

Implement optional incremental Tree-sitter parsing, syntax roles, bracket,
comment, and indentation metadata with plain-text fallback.

## Files

`include/ssg/syntax.h`, `src/syntax.cpp`, `tests/test_syntax.cpp`,
`cmake/components/treesitter-syntax.cmake`

## Oracle

Full parse versus incremental parse snapshots, stale-result cancellation, and
plain-text fallback fixtures.

## Done

The mandatory workflow is complete without LSP or rendering.
