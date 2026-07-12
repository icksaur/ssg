# websocket-server

- Spec: `doc/features/session-protocol.md`, Plan 4
- Depends: `protocol-codec`, `http-cross-platform`, `clipboard-register`, `editor-session-assembly`
- Branch: `websocket-server-task`

## Scope

Implement `HttpEditorServer`, authentication/session mapping, bounded queues,
one writer, replay/reconnect, slow-client disconnect, and the single-channel
interaction contract. Authentication resolves host policy into an immutable
`InvocationPrincipal`; client payloads cannot grant capabilities.

## Files

`include/ssg/http_server.h`, `src/http_server.cpp`,
`tests/test_http_server.cpp`, `cmake/components/websocket-server.cmake`

## Oracle

In-process versus socket snapshot parity, reconnect replay/fresh snapshot,
backpressure/timeout, and an audit proving every interaction uses one channel.

## Done

The mandatory workflow is complete on Linux and Windows.
