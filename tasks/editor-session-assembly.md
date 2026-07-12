# editor-session-assembly

- Spec: `doc/features/session-protocol.md` and `doc/spec.md`
- Depends: `core-websocket-slice`, `session-state`, `required-command-catalog`, `edit-history-integration`, `follow-edits`, `tab-management`, `lsp-language-features`, `lsp-workspace-edits`, `browser-input-conformance`, `lua-command-host`, `treesitter-syntax`, `tree-providers`, `theme-model`, `viewport-wrap-scrollbar`, `settings-model`, `external-modification-flow`
- Branch: `editor-session-assembly-task`

## Scope

Own explicit adaptation and registration of every descriptor in every P0
feature `CommandSet` into generic `CommandRegistration` values. Construction
binds feature handlers and proves command effects and capabilities equal the
independently reviewed `data/required-commands.json` metadata, rejects missing
or extra bindings, and exposes the public `EditorSessionBuilder` seam. Extend the
existing dispatch context/session seam in place for feature state, transaction,
status, and delta services; do not create a second dispatch path.

Aggregate every typed feature view/delta section and extend the early snapshot
envelope so no feature requires client-side or out-of-band composition. Each
snapshot is scoped to one attached client and contains that principal's
host-granted capability IDs plus that client's view identity, viewport
dimensions, scroll state, visible rows, and hit targets. It must not expose
another principal's capabilities or viewport.

## Files

`include/ssg/editor_session_assembly.h`,
`src/editor_session_assembly.cpp`, `include/ssg/command_registry.h`,
`src/command_registry.cpp`, `include/ssg/session.h`, `src/session.cpp`,
`include/ssg/snapshot.h`, `src/snapshot.cpp`,
`include/ssg/syntax.h`, `src/syntax.cpp`, `tests/test_syntax.cpp`,
`include/ssg/session_snapshot.h`, `src/session_snapshot.cpp`,
`data/required-commands.json`,
`tests/test_editor_session_assembly.cpp`,
`cmake/components/editor-session-assembly.cmake`

## Oracle

Required-command catalog equals the assembled registry exactly; a full
state-transition script compares fresh aggregate snapshots with replayed deltas
and verifies every feature section and per-client viewport. Aggregate replay
includes the incremental document section. Two-client tests prove capability
and viewport isolation.

`snapshot.h` remains the document-section contributor seam.
`session_snapshot.h` owns the complete `SessionSnapshot`/`SessionDelta`
aggregate. Replacing the thin slice's standalone document adapter is deferred
to Gate 10 protocol/server composition; this task extends the shared
`EditorSession::dispatch` path and does not edit `http_server.*`.

## Done

The mandatory workflow is complete; this is the sole convergence task for
command dispatch and snapshot/delta aggregation.
