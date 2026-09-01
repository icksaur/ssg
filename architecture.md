# Architecture

SSG is a C++20 editor library with terminal and browser clients. The library
owns editor meaning and authoritative product state. Clients own presentation,
native layout, and device I/O. A client may predict presentation locally for
responsiveness, but it does not create a second editor behavior path.

The strongest contracts are the public types under `include/`, the target graph
in `CMakeLists.txt` and `cmake/ssg_layers.cmake`, and the focused tests under
`tests/`. This document is a map to those contracts rather than a replacement
for them.

## System model

An `EditorSession` is the serialized aggregate boundary for a running editor.
A host attaches a client with a host-chosen principal and view, then the client
sends either a named `ClientCommand` or a typed `ClientInput`. The session
validates identity, capability, and revision; applies the authoritative
transition; advances its revision when semantic state changes; and publishes a
per-client `SessionSnapshot` or `SessionDelta`.

The normal flow is:

1. The host supplies client identity, view identity, and capabilities.
2. `EditorSession` publishes an initial semantic snapshot for that attachment.
3. Commands and semantic input converge on the same core command and transition
   machinery.
4. The core publishes revisioned state; the protocol and transport encode and
   order it without deciding editor behavior.
5. A client presents that state using either the grid service or its native UI.
   Local predictions are discarded or settled against later authoritative
   revisions.

`EditorSession` serializes each state operation across command execution,
reconciliation, revision publication, and result construction. Command handlers
that need another command use `deferDispatch`; they do not recursively re-enter
the session.

## Build layers

Each library component has its own public include root. The following table uses
"depends on" in the ordinary direction: the target in the first column may
consume the targets in the last column.

| Target | Responsibility | Public root | SSG dependencies |
|---|---|---|---|
| `ssg_platform` | Operating-system and filesystem adapters | `include/platform` | none |
| `ssg_core` | Editor behavior and authoritative semantic state | `include/core` | `ssg_platform` |
| `ssg_protocol` | Socket-free encoding and decoding | `include/protocol` | `ssg_core` |
| `ssg_grid` | Optional cell-grid layout, rendering, and hit testing | `include/grid` | `ssg_core` |
| `ssg_http_server` | HTTP/WebSocket session adapter | `include/transport` | `ssg_protocol`, `ssg_core` |
| `ssg` | External compatibility facade | none; interface target | `ssg_core`, `ssg_protocol`, `ssg_grid` |

Protocol and grid are siblings: neither may depend on the other. Core cannot
depend on either presentation or transport. Transport sits above protocol and
core and is deliberately not part of the `ssg` facade. Executables are the
composition boundary and may link the components they host.

`cmake/ssg_layers.cmake` makes this graph executable. It records each target's
allowed direct links, public root, private roots, and public-header inventory.
Configuration fails for a forbidden target edge, an unexpected visible include
root, a missing public header, or duplicate logical ownership. Separate include
roots matter: target names alone would not stop a source file from including a
sibling layer's header.

`cmake/components/*.cmake` are feature manifests, not additional architectural
layers. They assign sources to the owning component and register focused tests.
Allowed and forbidden compile-visibility cases live under
`tests/fixtures/layers`; graph rejection fixtures live under
`tests/fixtures/layer_graph`.

## Major components and power spots

A power spot is the one place that owns an important state transition or policy.
New behavior should normally extend these owners instead of reproducing their
knowledge elsewhere.

| Owner | Single responsibility |
|---|---|
| `EditorSession` | Serialize client attachment, command/input ingress, worker publication, revisioning, and per-client snapshots. See `include/core/ssg/EditorSession.h` and `src/runtime/`. |
| `InteractionAuthority` | Own whole-screen semantic truth, schema generation, prompt lifecycle, focus/presence projection, and tree revision allocation. See `include/core/ssg/InteractionAuthority.h`. |
| `Document::apply` | Validate and atomically apply text transactions against document revision, mode, UTF-8 boundaries, and edit ranges. |
| `Workspace` | Own file/document identity, open/save/close policy, baselines, archives, recovery, and observation of self-saves. |
| `CommandCatalog` and `CommandSpecBuilder` | Own the named command vocabulary, metadata, capability requirements, and handlers. |
| `CommandTransition` and `PreparedTransition` | Preflight and atomically install the closed set of changes that span prompt, panel, picker, focus, and tree-provider state. |
| `assembleWholeScreen` | Build the canonical, medium-independent whole-screen UI tree and apply the single chrome override/fallback policy. |
| `UiFrame` and `UiFrameDeltaCodec` | Keep schema, dynamic node state, presence, and focus in one validated publication and derive/replay its changes. |
| `SessionSnapshotCodec` | Derive and replay complete per-client semantic deltas without publishing a partial candidate. |
| `GridPresenter` | Project semantic state through the grid-specific layout service and resolve grid view actions against a presentation basis. |
| `ProtocolCodec` | Encode and decode the bounded wire value tree and typed message envelopes; it does not own sockets. |
| `HttpEditorRoute` | Bind WebSocket connections using host policy, order state and completion messages, enforce queue limits, and retain reconnect replay history. |
| Platform file and watcher seams | Hide Linux and Windows durability, filesystem-watch, and Git-watch implementations from core behavior. |

Domain models such as selection, history, search, tabs, trees, syntax, LSP, diff,
settings, theme, and status remain separate values and services inside core.
`SessionSnapshotSections` is their publication aggregate, not their mutation
owner.

## Authoritative UI model

The UI virtual machine is medium-independent. It describes what exists and how
nodes relate without publishing pixels, terminal cells, browser objects, or
resolved rectangles.

- `UiSchema` is an immutable tree for one `Generation`. Each `UiNode` has a
  strong `UiNodeId`, a size constraint, semantic style roles, and exactly one
  container or leaf payload. Structural change replaces the schema and advances
  its generation.
- `UiStateSection` contains generation-scoped dynamic leaf values and the
  authoritative focus path.
- `UiPresenceSection` contains the present state of nodes at a
  `PresenceBasis`.
- `UiFrame` validates and publishes those three parts together. Its delta is
  either a full replacement or generation-preserving state, presence, and focus
  changes.

`assembleWholeScreen` owns the canonical root/header/body/panel/content/footer
structure. `InteractionAuthority` rebuilds focus and presence from
`WholeScreenTruth`, so clients do not infer product state from geometry or
prompt kinds. Typed well-known areas let a native client hand selected subtrees
to its own toolkit; the tree remains the source of placement and visibility.

Concrete colors are not UI-tree properties. Nodes carry `SemanticRole`
identities, and `ThemeSnapshot` is the source of resolved colors.

## Commands and mutations

There are several typed mutation forms, each at a different seam:

- `ClientCommand` carries one command name, a base revision, and the payload
  declared by that command. The command catalog and argument codec registry
  reject unknown or mismatched commands.
- `ClientInput` is a closed variant for key/text input and semantic tab, tree,
  picker, external-action, notice-action, document-selection, scroll, focus,
  and navigation input. Pointer coordinates are resolved at the presentation
  edge before entering core.
- `EditTransaction` is the document mutation unit.
- `CommandTransition` is the closed cross-subsystem interaction mutation. Its
  preflight is fallible and non-mutating; the resulting `PreparedTransition`
  installs complete replacement state.

`MutationPatch` and `applyMutationPatch` define an atomic,
generation-and-basis-checked reference model for show/hide/toggle presence
patches, including `ApplicationId` reconciliation. At present this model is
exercised by core tests but is not carried by the wire schema and is not called
by the production session path. Current authoritative presence is rebuilt by
`InteractionAuthority` and published through `UiFrame`. Do not assume that
optimistic presence patches are a supported client protocol until that seam is
wired explicitly.

## Snapshot and wire protocol

`SessionSnapshot` is a move-only, immutable-per-publication aggregate containing
the attachment identity, topology, revision, and semantic sections. A
`SessionDelta` identifies its base and target revisions and carries sparse
section changes. Replay verifies attachment and revision identity before
producing a replacement snapshot.

The socket-free binary protocol is implemented by `ProtocolCodec`:

```text
[wire-version byte][message-kind byte][tagged ProtocolValue payload]
```

`ProtocolValue` supports null, Boolean, signed and unsigned integers, UTF-8
text, bytes, arrays, and objects. Decoding applies explicit message-size,
nesting, collection, text, and byte limits. Message kinds cover command
requests/results, client input/results, snapshots, and deltas. Retired ordinals
remain reserved so later message values do not move.

`protocol/schema/semantic_wire.mjs` is the data-only inventory for message
kinds, enums, wire types, semantic sections, lifecycle, and replay policy.
`protocol/schema/generate_semantic_wire.mjs` validates that inventory and
generates the C++ core manifest, C++ protocol schema and ordinary replay code,
and the browser manifest. Structured or asymptotically important sections keep
specialized replay implementations; ordinary replacement shapes are generated.

The HTTP/WebSocket adapter adds connection lifecycle and ordering. Its initial
text attach request is `SSG1 ATTACH` plus either the last applied revision or a
no-state marker. Subsequent editor messages are binary protocol envelopes. The
host's `HttpEditorConnectionPolicy`, not client input, supplies session,
principal, view, and capability authority. The route can replay a contiguous
bounded delta chain after reconnect; otherwise it sends a replacement snapshot.

## Presentation and clients

### Grid and terminal

The grid layer is optional presentation support. `GridPresenter::project`
accepts an `EditorSession`, client identity, dimensions, and palette report. It
uses a two-phase seam:

1. `EditorSession::capturePresentation` captures semantic state and style.
2. Grid code solves layout without holding the session lock.
3. `EditorSession::projectViewport` accepts the retained viewport state only if
   client, view, and semantic revision still match.

The presenter retries a bounded number of times when the semantic basis changes.
The resulting `GridFrame` contains the semantic publication, solved grid tree,
grid surfaces, and a `GridBasis`. `GridPresenter::apply` rejects actions from a
stale view, semantic revision, or presentation generation.

The terminal application in `apps/` owns raw terminal mode, byte input/output,
escape-sequence decoding, capability probing, signals, and process lifecycle.
`ssg_grid` owns cell layout, rendering, and hit testing. Terminal coordinates
become typed semantic input before they reach core.

### Browser

The browser consumes the same semantic snapshots and UI tree but uses DOM and
CSS layout instead of grid geometry. `apps/web/reconcile.mjs` is the DOM-free,
testable protocol/replay/reconciliation module. `apps/web/client.mjs` owns
WebSocket I/O, DOM retention, CSS lowering, browser events, focus, and resize
observation. `apps/web/fuzzy.mjs` implements the published fuzzy-matching
contract.

The web client intentionally performs the following work locally:

- It echoes pending text at the authoritative caret and later settles the FIFO
  against command/input results and published revisions.
- It predicts prompt text and temporary focus presentation while input is in
  flight.
- It filters and ranks the complete server-published palette candidate set
  using server-published matcher parameters.
- It chooses native DOM geometry, responsive flex sizing, optional-node
  survival, element retention, accessibility attributes, and concrete event
  handling.

These are responsiveness and presentation exceptions, not alternate editor
authority. Predictions are based on typed IDs, revisions, activation IDs, and
published data; reconnect or uncertainty clears them; authoritative state wins.
Keyboard input still reaches the authoritative keymap, and pointer-visible
actions have typed semantic input or command routes.

The browser necessarily duplicates some codec and replay mechanics in
JavaScript because it cannot link the C++ library. Generated vocabulary,
canonical fixtures, and tests against the shipped reconciliation module limit
drift. Product inventories, command behavior, candidate sets, matcher
parameters, semantic roles, and UI structure remain library-owned.

## Directory map

| Path | Contents |
|---|---|
| `include/core/ssg` | Public editor-domain and semantic UI contracts |
| `include/platform/ssg` | Public OS/filesystem adapter contracts |
| `include/protocol/ssg` | Public wire codec contracts and generated schema |
| `include/grid/ssg` | Public cell-grid presentation contracts |
| `include/transport/ssg` | Public HTTP/WebSocket adapter contract |
| `src/` | Implementations; broad core files plus grid implementations |
| `src/runtime/` | `EditorSession` domain and publication implementation slices |
| `src/protocol/` | Feature and aggregate protocol codecs |
| `src/platform/` | Linux and Windows adapter implementations |
| `apps/` | Terminal/process host and HTTP startup composition |
| `apps/web/` | Browser client, pure reconciliation, fuzzy matcher, and generated manifest |
| `protocol/schema/` | Authoritative semantic wire inventory and generator |
| `cmake/components/` | Feature source/test manifests |
| `data/` | Command, Unicode, and UI inventories |
| `tests/` | Focused owner tests, protocol fixtures, browser tests, and layer fixtures |
| `benchmarks/` | Performance and protocol-delta measurements |
| `scripts/` | Repository build and validation entry points |
| `vendor/` | Vendored implementation dependencies |

## Deliberate pressure points and exceptions

- `EditorSession` is deliberately an aggregate rather than a small
  single-purpose object. Atomic sequencing belongs there, while domain behavior
  is split among models and `src/runtime/` implementation slices.
- `InteractionAuthority` combines schema, prompt, focus, presence, and tree
  revision ownership because independent writers previously allowed those facts
  to diverge.
- `Protocol.cpp` remains a central envelope/registry power spot even though
  feature codecs are split under `src/protocol/`.
- Core depends on platform interfaces for durability and observation. The OS
  implementations remain behind platform seams; core must not acquire direct
  filesystem or watcher implementations.
- `EditorSession.cpp` still contains POSIX wake-descriptor plumbing. This is an
  implementation-level platform exception not enforced by the component include
  graph.
- The wire schema retains named compatibility fields and permanently retired
  ordinals. They preserve decoding and numbering; they are not a second active
  product-state model.
- The application links several components because process startup and device
  composition must happen above the library layers.
- Browser-native layout, local input echo, prompt prediction, and local fuzzy
  ranking are the intentional responsiveness exceptions described above.
