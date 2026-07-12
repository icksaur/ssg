# search-palette

- Spec: `doc/features/language-services.md`, Plan 1
- Depends: `document-transactions`, `prompt-status-surface`
- Branch: `search-palette-task`

## Scope

Implement command palette state, Goto Anything file/line/symbol modes,
navigation history, and cancellable ranked workspace search results.

## Files

`include/ssg/search.h`, `src/search.cpp`, `tests/fixtures/search/`,
`tests/test_search.cpp`, `cmake/components/search-palette.cmake`

## Oracle

Accepted ranking/query goldens, cancellation/stale-result cases, and
back/forward navigation transition tables.

## Done

The mandatory workflow is complete without find/replace mutation.
