# lsp-language-features

- Spec: `doc/features/language-services.md`, Plan 4
- Depends: `lsp-sync-diagnostics`
- Branch: `lsp-language-features-task`

## Scope

Implement completion, hover, definition, references, and their command/view
state over the established synchronized LSP connection.

## Files

`include/ssg/lsp_features.h`, `src/lsp_features.cpp`,
`tests/fixtures/lsp/features/`, `tests/test_lsp_features.cpp`,
`cmake/components/lsp-language-features.cmake`

## Oracle

Scripted fake-server request/response cases, stale/cancelled result rejection,
completion ordering/acceptance, and definition/reference navigation fixtures.

## Done

The mandatory workflow is complete without rename/workspace edits.
