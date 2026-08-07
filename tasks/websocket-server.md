# websocket-server

- Spec: `doc/features/session-protocol.md`, Plan 7
- Depends: `protocol-codec`, `http-cross-platform`, `clipboard-register`, `editor-session-assembly`
- Branch: `websocket-server-task`

## Scope

Implement `HttpEditorServer`, session mapping, bounded queues,
one writer, replay/reconnect, slow-client disconnect, and the single-channel
interaction contract. The host resolves each connection into an immutable
`InvocationPrincipal`; client payloads cannot grant capabilities.
Replace the temporary `CoreEditorSlice` constructor with the assembled
`EditorSession`, complete command codec, and explicit `HttpEditorSessionHost`
attach/snapshot/interaction seam. Carry forward the already-proven
queue, writer, and deadline mechanism rather than creating a second path.

## Files

`include/ssg/http_server.h`, `src/http_server.cpp`,
`include/ssg/protocol.h`, `src/protocol.cpp`, `protocol/schema/README.md`,
`tests/fixtures/protocol/command_result.hex`, `tests/test_protocol.cpp`,
`tests/test_http_server.cpp`, `cmake/components/websocket-server.cmake`,
`cmake/components/core-websocket-slice.cmake`

## Oracle

In-process versus socket snapshot parity, reconnect replay/fresh snapshot,
backpressure/timeout, host-issued capability isolation, and an audit proving
command, clipboard, status-action, and binary interactions use one channel.

## Done

The mandatory workflow is complete on Linux and Windows.
