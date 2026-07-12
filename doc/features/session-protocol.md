# spec-session-protocol

## Goals

Expose all interactions and views through one revisioned API that behaves identically in-process and over one Linux/Windows WebSocket connection.

## Design

`EditorSession`, command ordering, snapshots/deltas, replay, bounds, queues, and reconnect behavior follow `doc/spec.md`. Attach/authentication creates an immutable `InvocationPrincipal` containing client identity, origin, and host-granted capabilities; every command dispatch receives it and every per-client snapshot exposes its capability IDs. The protocol carries commands, capability state, clipboard, status actions, and binary payloads without an out-of-band behavior channel.

The initial session-state component owns the serialized executor, strong client,
workspace, and view identities, attached-client records, shared active
workspace/view identities, revision ordering, and the generic command dispatch
boundary. It does not own feature-specific viewport, selection, tab, or split
state. Snapshot/delta aggregation, replay, and serialization are later
`editor-session-assembly`, `core-websocket-slice`, and `protocol-codec` work.

The generic `CommandSet` defined by session state binds command descriptors to
handlers and is distinct from existing immutable `<Feature>CommandSet`
descriptor catalogs. `editor-session-assembly` explicitly enumerates every
feature catalog, adapts each descriptor and bound handler into a generic
registration, and proves its command effects and capabilities equal the
independently reviewed metadata in `data/required-commands.json`. The catalog
is an oracle rather than a runtime configuration dependency. Every P0 command
changes authoritative shared or per-client snapshot state, so the catalog's
reviewed default effect is `mutation`; `observation` remains available for
future commands that return information without changing any snapshot section.
Assembly rejects missing or extra bindings.
`EditorSessionBuilder` is the public construction seam; session state does not
refactor feature-owned command sets.

The core WebSocket thin slice precedes that general assembly with one explicit,
temporary adapter for `text.insert`. A slice-owned object contains one
`Document`, its single caret `SelectionSet`, and an `EditorSession`. Its
registered handler closure calls `apply_text_input`, applies the resulting
transaction to that document, and advances the caret before returning success.
Both direct and WebSocket entry points call `EditorSession::dispatch`; neither
may apply an edit directly. `editor-session-assembly` later replaces this
single-document adapter rather than creating a second dispatch path.

Every generic command descriptor declares whether it mutates authoritative
state and the capabilities required at dispatch. Mutating commands require a
base revision equal to the current session revision. Non-mutating commands may
run against an older observed revision. Capability checks happen before the
handler at the common registry boundary and use only the immutable principal
stored at attach. Tests construct both `in_process` and `websocket` principals
and exercise that same boundary; a real socket is not part of this component.

The initial bounded `CommandContext` exposes the current revision, immutable
principal, and staged active-workspace/view changes. Successful mutating
handlers atomically commit staged topology and advance the revision once.
Rejected, failed, or throwing handlers commit nothing and do not advance the
revision. Feature state, transaction, status, and delta sinks are added by the
later assembly task through an explicit caller-owned services interface without
creating a second dispatch path.

For this single-document slice, session and document revisions start at one and
advance in lockstep exactly once for each accepted non-empty insert. The
document revision is exposed in the envelope and is the next command's session
base revision. Rejected, stale, malformed, failed, and empty insert requests
advance neither revision. The slice owns the caret needed to make sequential
insert scripts deterministic.

`snapshot.h` defines the document contributor section and its incremental
delta. `session_snapshot.h` defines the eventual aggregate session snapshot and
delta. The assembly task adds document replay, invokes every typed feature
delta contributor, and remains the sole owner of aggregate derivation and
replay.

An aggregate snapshot is produced for one attached client. Shared feature
sections are combined with that client's immutable host-granted capability
IDs, view identity, viewport dimensions, scroll state, visible rows, and hit
targets. No aggregate exposes another client's capabilities or viewport.
Capabilities remain immutable for the lifetime of an attachment.

The assembly task extends the common session/command-context seam but does not
edit the temporary `HttpEditorServer` slice. Gate 10 protocol/server composition
replaces that slice-owned standalone document closure with the assembled
session; both paths continue to call `EditorSession::dispatch`.

`../http` owns one platform socket seam used by HTTP and WebSocket lifecycle,
receive, and write paths. A complete write loops over partial writes until all
bytes are sent or an absolute `std::chrono::steady_clock::time_point` deadline
is reached. It returns a typed status (`complete`, `timeout`, `closed`, or
`error`), the number of bytes transferred, and the native error code when
applicable. WebSocket `send` returns that typed result; its no-deadline overload
uses the server's finite write budget and remains condition-testable for source
compatibility. HTTP responses, upgrades, data, pong, and close frames use the
same complete-write primitive. Linux suppresses `SIGPIPE`; Windows owns Winsock
startup/cleanup and maps native timeout, peer-close, and error codes to the same
statuses.

## Invariants

I1, I2, I3, I10, I11, I12, I16, I21 from `doc/spec.md`.

## Considerations

- Per-client viewport state differs while shared semantic state remains ordered.
- Slow or stale clients receive typed errors or disconnect reasons.
- Serialization owns payload bytes through send completion.
- Duplicate command IDs are rejected while constructing a command set and while
  composing multiple sets into a registry, never deferred until dispatch.
- The thin slice registers exactly one command ID, `text.insert`; the other five
  descriptors in `TextInputCommandSet` remain for assembly.
- The thin-slice route owns a finite per-connection outbound queue and one
  writer thread. Command callbacks enqueue owned payloads and never call socket
  send directly. Queue overflow or a failed/deadline-expired write closes the
  connection. Broader replay, reconnect, authentication, and service
  integration remain later work.
- Malformed frames are a codec/WebSocket rejection oracle because the typed
  in-process API has no decode step. Stale requests are compared through both
  direct and WebSocket dispatch.

## Risks and Mitigations

- Platform divergence: run identical socket scripts on Linux and Windows.
- Deadline/partial-write nondeterminism: test the complete-write loop with a
  scripted write-attempt oracle, then exercise the native backend with loopback
  lifecycle tests. Linux runs locally; the existing MinGW compiler provides a
  source/build check when available, while native Windows CI is authoritative.
- Hidden side channels: end-to-end test records every interaction on the one connection.

## Acceptance (Definition of Done)

- Observable: in-process and WebSocket scripts produce equal canonical semantic snapshots after every command.
- Budgets: queue and command-cycle limits from `doc/spec.md`.
- Gates: protocol, `../http`, and integration suites are green on Linux and Windows.
- Oracles: hand-authored total-order, mutation-staleness, two-client isolation,
  handler-failure atomicity, duplicate-registration, and principal-origin parity
  scripts for session state; codec round trips/malformed corpus,
  partial-write/deadline tests, and replay-vs-snapshot comparisons for later
  protocol components.

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Implement the serialized session executor, identities, shared active workspace/view topology, immutable principals, and duplicate-rejecting generic command registry; keep snapshot/delta aggregation, replay, and serialization out of scope | `include/ssg/session.h`, `src/session.cpp`, `include/ssg/command_registry.h`, `src/command_registry.cpp`, `tests/test_session.cpp`, `cmake/components/session-state.cmake` | hand-authored scripts for interleaved two-client total order, mutation-only stale rejection, client isolation, failed/throwing-handler atomicity, duplicate IDs within/across sets, registered dispatch, and identical capability enforcement for in-process/WebSocket-origin principals | I2, I3 |
| 2 | Implement the minimal single-document snapshot/delta section and bounded versioned `text.insert` codec | `include/ssg/snapshot.h`, `src/snapshot.cpp`, `include/ssg/protocol.h`, `src/protocol.cpp`, `tests/test_core_websocket_slice.cpp` | direct/WebSocket insert parity after every command, codec round-trip, and malformed corpus | I2, I3, I11 |
| 3 | Extract the `../http` socket seam, add typed deadline-aware complete writes, then add its Windows backend | `../http/http.*`, `../http/src/platform/*`, `../http/tests/test_http.cpp` | existing suite after seam extraction; scripted partial/timeout/close/error writes; native loopback frame and lifecycle scripts; Linux runtime plus Windows compile/native-CI parity | I10, I21 |
| 4 | Implement the finite-queue one-channel thin-slice server adapter for `text.insert` | `include/ssg/http_server.h`, `src/http_server.cpp`, `tests/test_core_websocket_slice.cpp`, `cmake/components/core-websocket-slice.cmake` | in-process/loopback-WebSocket parity, stale/malformed scripts, and side-channel audit | I1, I2, I11, I16 |
| 5 | Assemble every P0 feature command catalog, extend the common dispatch services, and aggregate/replay every typed snapshot/delta section for one client | `include/ssg/editor_session_assembly.h`, `src/editor_session_assembly.cpp`, `include/ssg/session_snapshot.h`, `src/session_snapshot.cpp`, session/registry/document seams, assembly tests and manifest | required catalog equals registry exactly; full transition snapshot equals replay; two-client capabilities and viewports remain isolated | I2, I3, I16 |

## Rationale (optional, skippable)

One protocol seam prevents each client from becoming a separate editor.
