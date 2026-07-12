# protocol-codec

- Spec: `doc/features/session-protocol.md`, Plan 6
- Depends: `editor-session-assembly`, `core-websocket-slice`
- Branch: `protocol-codec-task`

## Scope

Extend the early codec without sockets. Add a bounded versioned
`ProtocolValue` and immutable `CommandArgumentCodecRegistry` whose IDs must
exactly equal the assembled P0 descriptors and whose entries convert typed
`std::any` command payloads without a fallback. Cover ingress-only commands.

Round-trip the concrete per-client `SessionSnapshot` and `SessionDelta`,
including identity, capabilities, viewport, and every typed section. Add the
minimal protocol-owned construction seam needed to decode `SessionDelta`.
Encode clipboard request/response and status-action invocation as distinct
message kinds. Add owned binary-frame envelope/metadata framing and bounds;
binary payload producers remain out of P0 scope.

## Files

`include/ssg/protocol.h`, `src/protocol.cpp`,
`include/ssg/session_snapshot.h`, `protocol/schema/`,
`tests/fixtures/protocol/`, `tests/test_protocol.cpp`,
`cmake/components/protocol-codec.cmake`

## Oracle

Canonical round-trip fixtures; malformed/truncated/oversized corpus;
unknown-version rejection; exact command-codec registry coverage; decoded
byte-lifetime; two-client capability/viewport isolation; and decoded
snapshot/delta replay equivalence.

## Done

The mandatory workflow is complete without network I/O. The new component
manifest registers only the protocol test target because the core WebSocket
slice already registers `src/protocol.cpp`.
