# treesitter-syntax

- Spec: `doc/features/language-services.md`, Plan 2
- Depends: `document-transactions`
- Branch: `treesitter-syntax-task`

## Scope

Implement optional incremental Tree-sitter parsing, syntax roles, bracket,
comment, and indentation metadata with plain-text fallback. Use an injected
parser/grammar boundary so the core has no required Tree-sitter initialization
or link dependency. Export immutable `SyntaxViewState` and `SyntaxDelta` plus
pure derive/replay functions for later session assembly; do not edit session
aggregates, protocol codecs, LSP, or rendering.

## Files

`include/ssg/syntax.h`, `src/syntax.cpp`, `tests/test_syntax.cpp`,
`cmake/components/treesitter-syntax.cmake`

## Oracle

An injected deterministic parser compares full and incremental snapshots and
exercises stale-result cancellation and plain-text fallback fixtures.

## Done

The mandatory workflow is complete without LSP or rendering.
