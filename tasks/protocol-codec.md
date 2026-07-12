# protocol-codec

- Spec: `doc/features/session-protocol.md`, Plan 2
- Depends: `editor-session-assembly`, `core-websocket-slice`
- Branch: `protocol-codec-task`

## Scope

Extend the early codec to cover every assembled command argument and typed
snapshot/delta/status/clipboard/diff/follow/service section plus binary-frame
metadata, without sockets.

## Files

`include/ssg/protocol.h`, `src/protocol.cpp`, `protocol/schema/`,
`tests/fixtures/protocol/`, `tests/test_protocol.cpp`,
`cmake/components/protocol-codec.cmake`

## Oracle

Round-trip fixtures, malformed/truncated/oversized corpus, unknown-version
rejection, and byte-lifetime tests.

## Done

The mandatory workflow is complete without network I/O.
