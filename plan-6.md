# plan-6-generate-and-reduce-the-protocol

## Goals

Declare semantic wire fields and enum meanings once, generate consistent C++
and JavaScript handling, and retain custom delta concepts only where measured
latency or payload needs justify them.

## Design

Create an authoritative typed wire manifest that generates or drives codecs,
field names, enum ordinals, validation, and JavaScript transactional replay.
Use ordinary replacement/change operations for small semantic sections.
Preserve specialized incremental document and large-tree deltas only when the
existing protocol benchmark shows an asymptotic improvement or generic replay
exceeds `kMaximumGenericDeltaPayloadRatio` or
`kMaximumGenericReplayLatencyRatio`; otherwise remove them. The ratio values
live in benchmark code, not this spec. `ProtocolValue` remains a private codec
representation, not a product property bag.

## Invariants

- PROTOCOL-1: Each wire field and enum meaning has one declaration consumed by
  every language binding. State at the wire manifest.
- PROTOCOL-2: Decoding validates types, bounds, required fields, and unknown
  compatibility behavior before state mutation. State at generated decoders.
- PROTOCOL-3: Replay is atomic and rejects stale or malformed bases. State at
  generated replay operations.
- PROTOCOL-4: Transport framing, authorization, replay queues, and backpressure
  do not reinterpret product behavior. State at transport route APIs.
- PROTOCOL-5: A specialized delta exists only with a measured payload or
  latency requirement and a focused oracle. State at each specialized delta.

## Considerations

Generation uses a Node script because Node is already the browser-test tool; it
adds no production runtime dependency. Generated artifacts should be
deterministic and checked during the build rather than hand-edited. Manifest
declaration order controls section output; alphabetic ordering controls
cross-section indexes. Avoid a universal dynamic property bag. Keep protocol
versioning and additive compatibility explicit.

## Risks and Mitigations

- A generator can hide complexity: keep the manifest typed, readable, and
  smaller than the duplicated implementations it replaces.
- Generated C++ can degrade diagnostics: generate named types/functions and
  retain public domain types.
- Replacing all deltas uniformly can regress large-document performance:
  measure before deleting specialized text/tree operations.

## Acceptance (Definition of Done)

- Observable: protocol behavior remains compatible at the selected new wire
  version; clients cannot silently retain stale fields.
- Budgets: generated/replacement deltas meet existing input and large-state
  latency/payload limits.
- Gates: `scripts/check.sh` and `scripts/check.sh push`.
- Oracles: deterministic-generation check; full field/enum cross-language
  fixtures; malformed/stale atomic replay; existing and extended protocol
  benchmark results compared against the named ratio budgets.

## Plan 3 bridge-removal inventory

Plan 4 first replaces the bridge's layout consumers with the solved grid tree.
When that handoff is complete, Step 6 removes all entries below in one wire
version change; none is a retained protocol concept.

| Compatibility entry | Current location | Step 6 action and oracle |
|---|---|---|
| `LegacyPresentationSnapshot` and the legacy-taking `GridFrame` constructor | `include/ssg/session_snapshot.h`, `include/ssg/GridPresenter.h` | delete both; `GridFrame` is constructed from semantic state plus solved grid output |
| `EditorSession::present`, `EditorSession::projectForBridgedPresenterDeprecated`, and presentation-bearing `SessionSnapshotCodec::assemble` | `include/ssg/EditorSession.h`, `src/EditorSession.cpp`, snapshot codec | delete the public wrapper and private bridge; semantic callers use `snapshot`, grid callers use `GridPresenter` |
| `encodeLegacyPresentationSnapshot`, `decodeLegacyPresentationSnapshot`, and presentation value codecs | `include/ssg/Protocol.h`, `src/Protocol.cpp` | delete at `kSemanticUiWireVersion`; prior-version messages reject through normal version negotiation |
| Frozen snapshot field `presentation` | session snapshot encoder/decoder and `session_snapshot.hex` | remove from the new-version manifest and replace the legacy fixture with a current-version semantic fixture |
| Frozen delta fields `style`, `shell`, `viewport`, `selection_nav`, `prompt_projection`, and `tree_windows` | `SessionDelta`, delta encoder/decoder, JavaScript replay compatibility, and `session_delta.hex` | remove fields, types used only by them, and client replay branches; cross-language fixtures prove exact current-version shape |
| Semantic fixed-default fixtures | `session_semantic_base.hex`, `session_semantic_target.hex`, `session_semantic_delta.hex` | regenerate only for the activated version and retain semantic replay equality |
| Legacy test builders and wrapper callers | `tests/legacy_grid_frame.h`, direct `EditorSession::present` tests, protocol legacy round trips | migrate remaining grid tests during Plan 4, then delete compatibility-only fixtures and helpers in this step |
| Stale protocol documentation and field manifests | `protocol/schema/README.md`, generated manifest inputs | generate the current-version field inventory and make regenerate-and-diff detect any handwritten mirror |

The deletion condition is executable: after `kSemanticUiWireVersion` is
activated, source inventory must find none of the compatibility symbols or
field names above outside prior-version rejection fixtures.

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Define the typed semantic wire manifest and deterministic generator | protocol schema/generator files, CMake component, generator tests | regenerate-and-diff is empty | PROTOCOL-1 |
| 2 | Generate field names, ordinals, validation, and language bindings | `include/ssg/Protocol.h`, `src/Protocol.cpp`, generated C++/JS integration, web reconciliation | cross-language fixture equality | PROTOCOL-1, PROTOCOL-2 |
| 3 | Generate transactional replacement/change replay for ordinary sections | session snapshot/delta sources and web replay | stale/malformed/property tests | PROTOCOL-2, PROTOCOL-3 |
| 4 | Extend the existing protocol/performance benchmark with generic-versus-specialized payload and replay cases | protocol benchmark sources, performance corpus/manifests, CMake performance component | benchmark produces both named ratios for each custom delta | PROTOCOL-5 |
| 5 | Remove custom delta/message concepts that fail the named retention decision | delta types/codecs, transport handlers, web replay | payload/latency ratio decision and semantic replay equality | PROTOCOL-5 |
| 6 | Activate `kSemanticUiWireVersion` and remove Plan 3/5 deprecated fields after the Plan 1 migration condition passes | protocol manifest/codecs/fixtures/clients | prior-version negotiation rejection and current-version round trip | PROTOCOL-1, PROTOCOL-2, PROTOCOL-3 |
| 7 | Split the oversized codec implementation by major manifest section and delete handwritten mirrors | document, tabs, tree, palette, UI and other section codec sources; web files; component manifests | inventory and full protocol fixtures | PROTOCOL-1, PROTOCOL-4 |

## Rationale

The goal is fewer concepts, not merely generated copies of the current
proliferation. Generation first prevents drift; measurement then permits safe
deletion.
