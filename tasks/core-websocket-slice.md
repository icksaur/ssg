# core-websocket-slice

- Spec: `doc/spec.md` thin-slice risk mitigation; `doc/features/session-protocol.md`
- Depends: `session-state`, `text-input-commands`, `http-cross-platform`
- Branch: `core-websocket-slice-task`

## Scope

Establish the stable command-registry seam, minimal document snapshot/delta
envelope, minimal bounded codec, and loopback WebSocket route for
the existing `text.insert` command ID only. The slice owns one `Document` and
single-caret `SelectionSet`; a registry handler closure is the only edit path.
Session/document revisions advance in lockstep, and assembly later replaces
this temporary single-document adapter.

`snapshot.h` is the minimal document-section seam, not the final aggregate.
Malformed input is rejected at the codec/WebSocket boundary; stale requests
are compared through direct and WebSocket dispatch. The route uses a finite
per-connection outbound queue and one writer thread; command callbacks do not
send on the socket. Replay, reconnect, authentication, other text commands,
and optional services are out of scope.

## Files

`include/ssg/snapshot.h`, `src/snapshot.cpp`,
`include/ssg/protocol.h`, `src/protocol.cpp`,
`include/ssg/http_server.h`, `src/http_server.cpp`,
`tests/test_core_websocket_slice.cpp`,
`cmake/components/core-websocket-slice.cmake`

## Oracle

The same insert script through direct API and loopback WebSocket yields an
identical revisioned document snapshot after each command; stale and malformed
commands fail identically.

## Done

The mandatory workflow is complete before any optional service integration.
