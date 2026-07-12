# lsp-sync-diagnostics

- Spec: `doc/features/language-services.md`, Plan 4
- Depends: `session-state`, `document-transactions`, `core-websocket-slice`
- Branch: `lsp-sync-diagnostics-task`

## Scope

Implement injected LSP stream framing, initialize/shutdown, document
open/change/close version mapping, cancellation primitives, and diagnostics.

## Files

`include/ssg/lsp_sync.h`, `src/lsp_sync.cpp`,
`tests/fake_lsp_server.*`, `tests/fixtures/lsp/sync/`,
`tests/test_lsp_sync.cpp`, `cmake/components/lsp-sync-diagnostics.cmake`

## Oracle

Scripted fake-server lifecycle, UTF-8/UTF-16 position fixtures, version/stale
diagnostic cases, cancellation, and malformed-message tests.

## Done

The mandatory workflow is complete without completion/navigation/rename.
