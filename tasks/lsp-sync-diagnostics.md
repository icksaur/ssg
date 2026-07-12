# lsp-sync-diagnostics

- Spec: `doc/features/language-services.md`, Plan 4
- Depends: `session-state`, `document-transactions`, `core-websocket-slice`
- Branch: `lsp-sync-diagnostics-task`

## Scope

Implement injected LSP stream framing, initialize/shutdown, document
open/change/close version mapping, cancellation primitives, and bounded,
coalesced diagnostics. Export typed `LspSyncViewState` and `LspSyncDelta`
values with pure derive/replay functions for later session assembly. Lifecycle,
request cancellation, and stream decoding are bounded so a slow, hung, or
malformed server cannot stall the session or publish partial state. Do not edit
session aggregates, aggregate snapshots/deltas, or protocol codecs.

## Files

`include/ssg/lsp_sync.h`, `src/lsp_sync.cpp`,
`tests/fake_lsp_server.*`, `tests/fixtures/lsp/sync/`,
`tests/test_lsp_sync.cpp`, `cmake/components/lsp-sync-diagnostics.cmake`

## Oracle

Scripted fake-server lifecycle, UTF-8/UTF-16 position fixtures, version/stale
diagnostic cases, cancellation and timeout cases, bounded/coalesced diagnostic
publication, malformed-message tests, and nested-consumer optional-linkability.

## Invariants

I10 and I12 from `doc/spec.md`.

## Done

The mandatory workflow is complete without completion/navigation/rename.
