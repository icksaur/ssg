# lsp-workspace-edits

- Spec: `doc/features/language-services.md`, Plan 4
- Depends: `lsp-sync-diagnostics`, `recovery-actions`
- Branch: `lsp-workspace-edits-task`

## Scope

Implement rename and validated multi-document workspace edits, including file
operations, revision checks, recovery records, and all-or-nothing application.

## Files

`include/ssg/lsp_workspace_edit.h`, `src/lsp_workspace_edit.cpp`,
`tests/fixtures/lsp/workspace_edits/`, `tests/test_lsp_workspace_edit.cpp`,
`cmake/components/lsp-workspace-edits.cmake`

## Oracle

Independent position fixtures and injected validation/write failures prove
atomicity and compensating recovery across documents/files.

## Done

The mandatory workflow is complete without changing LSP synchronization.
