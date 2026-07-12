# core-websocket-slice

- Spec: `doc/spec.md` thin-slice risk mitigation; `doc/features/session-protocol.md`
- Depends: `session-state`, `text-input-commands`, `http-cross-platform`
- Branch: `core-websocket-slice-task`

## Scope

Establish the stable command-registry seam, minimal document snapshot/delta
envelope, minimal bounded codec, and loopback WebSocket route for
the existing `text.insert` command set only.

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
