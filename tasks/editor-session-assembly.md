# editor-session-assembly

- Spec: `doc/spec.md` command-registration and snapshot-composition seams
- Depends: `edit-history-integration`, `follow-edits`, `tab-management`, `lsp-language-features`, `lsp-workspace-edits`, `browser-input-conformance`, `lua-command-host`, `treesitter-syntax`, `tree-providers`, `theme-model`, `viewport-wrap-scrollbar`, `settings-model`, `external-modification-flow`
- Branch: `editor-session-assembly-task`

## Scope

Own explicit registration of every P0 `CommandSet`, aggregate every typed
feature view/delta section, extend the early snapshot envelope, and prove no
feature requires client-side or out-of-band composition. Per-client snapshot
state includes only that principal's host-granted capability IDs.

## Files

`src/editor_session_assembly.cpp`, `include/ssg/snapshot.h`,
`src/snapshot.cpp`, `include/ssg/session_snapshot.h`,
`src/session_snapshot.cpp`, `tests/test_editor_session_assembly.cpp`,
`cmake/components/editor-session-assembly.cmake`

## Oracle

Required-command catalog equals the assembled registry exactly; a full
state-transition script compares fresh aggregate snapshots with replayed deltas
and verifies every feature section and per-client viewport.

## Done

The mandatory workflow is complete; this is the sole convergence task for
command dispatch and snapshot/delta aggregation.
