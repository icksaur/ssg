# spec-session-protocol

## Goals

Expose all interactions and views through one revisioned API that behaves identically in-process and over one Linux/Windows WebSocket connection.

## Design

`EditorSession`, command ordering, snapshots/deltas, replay, bounds, queues, and reconnect behavior follow `doc/spec.md`. Attach/authentication creates an immutable `InvocationPrincipal` containing client identity, origin, and host-granted capabilities; every command dispatch receives it and every per-client snapshot exposes its capability IDs. The protocol carries commands, capability state, clipboard, status actions, and binary payloads without an out-of-band behavior channel. `../http` gains a Linux/Windows platform socket seam and deadline-aware complete sends.

## Invariants

I1, I2, I3, I10, I11, I12, I16, I21 from `doc/spec.md`.

## Considerations

- Per-client viewport state differs while shared semantic state remains ordered.
- Slow or stale clients receive typed errors or disconnect reasons.
- Serialization owns payload bytes through send completion.

## Risks and Mitigations

- Platform divergence: run identical socket scripts on Linux and Windows.
- Hidden side channels: end-to-end test records every interaction on the one connection.

## Acceptance (Definition of Done)

- Observable: in-process and WebSocket scripts produce equal canonical semantic snapshots after every command.
- Budgets: queue and command-cycle limits from `doc/spec.md`.
- Gates: protocol, `../http`, and integration suites are green on Linux and Windows.
- Oracles: codec round trips/malformed corpus, parity scripts, partial-write/deadline tests, replay-vs-snapshot comparisons.

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Implement session command ordering, snapshots, and deltas | `include/ssg/session.h`, `src/session.cpp`, `tests/test_session.cpp` | command scripts and delta replay | I2, I3 |
| 2 | Implement bounded versioned protocol codecs | `include/ssg/protocol.h`, `src/protocol.cpp`, `protocol/schema/*`, `tests/test_protocol.cpp` | round-trip and malformed corpus | I11 |
| 3 | Port deadline-aware `../http` transport to Linux and Windows | `../http/http.*`, `../http/src/platform/*`, `../http/tests/test_http.cpp` | cross-platform socket scripts | I10, I21 |
| 4 | Implement the one-channel server adapter | `include/ssg/http_server.h`, `src/http_server.cpp`, `tests/test_http_server.cpp` | in-process/WebSocket parity and side-channel audit | I1, I16 |

## Rationale (optional, skippable)

One protocol seam prevents each client from becoming a separate editor.
