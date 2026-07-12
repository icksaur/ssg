# lsp-workspace-edits

- Spec: `doc/features/language-services.md`, Plan 4c
- Depends: `lsp-sync-diagnostics`, `lsp-language-features`, `recovery-actions`
- Branch: `lsp-workspace-edits-task`

## Scope

Implement rename and validated multi-document workspace edits, including file
operations, revision checks, recovery records, and all-or-nothing application.
Export the immutable `LspWorkspaceEditCommandSet`. File resource operations use
an injected workspace file-operation seam; the workspace-edit component owns
validation and compensation while the adapter owns platform I/O.

## Invariants

I5, I10, I12 from `doc/spec.md`.

## Files

`include/ssg/lsp_workspace_edit.h`, `src/lsp_workspace_edit.cpp`,
`tests/fixtures/lsp/workspace_edits/`, `tests/test_lsp_workspace_edit.cpp`,
`cmake/components/lsp-workspace-edits.cmake`

## Oracle

Independent position fixtures and injected validation/write failures prove
atomicity and compensating recovery across documents/files.

## Done

The mandatory workflow is complete without changing LSP synchronization.
