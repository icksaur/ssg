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

## Step 1 design: manifest foundation and first ownership cut

### Scope

Step 1 introduces the typed manifest and deterministic generation boundary
without changing accepted wire bytes, replay behavior, or the active wire
version. It moves the protocol message-kind inventory and the semantic
snapshot/delta root-field inventory out of handwritten C++, JavaScript tests,
and fixture metadata. Deep section shapes, generated validators, and the
remaining cross-language enum vocabularies move in Step 2 after this generation
boundary is executable. Step 2's first enum targets are the vocabularies both
languages currently interpret independently, including focus targets, view
surfaces, widget/size kinds, and semantic input kinds.

The authoritative declaration lives at
`protocol/schema/semantic_wire.mjs`. It exports data only and has no filesystem,
process, CMake, or application dependency. The declaration contains:

- every current or retired protocol message kind, with one symbolic identity,
  wire name, explicit ordinal, and lifecycle;
- every semantic section identity, with its ordered snapshot field, ordered
  delta fields, lifecycle, and replay policy;
- explicit lifecycle values for current and compatibility-only semantic
  sections and for current, compatibility-only, and retired message kinds;
- explicit replay-policy values for replacement, changed replacement,
  specialized delta, and compatibility validation.

The section identity is not a generic runtime property key. It is generator
input that names a closed declaration consumed at build/test time. Snapshot and
delta fields remain separate named members because one semantic section may
have more than one delta field. Declaration order is the snapshot order and
each section owns the order of its delta fields. Retired message kinds remain declared permanently as ordinal reservations,
because their numeric slots must never be reused. Retired semantic sections are
different: a section has no numeric slot to reserve, so the version-activation
step removes it from the active manifest in the same change that adds the
prior-version rejection fixture. Compatibility-only sections remain declared
until that coordinated removal.

Every root field, including scalar availability/focus compatibility facts, is a
semantic section for manifest purposes. A section may own a single snapshot
field and a single delta field without requiring a domain aggregate type.
`watcher_available` is therefore a current single-field section and
`external_focus_held` is a compatibility-only single-field section; there is no
parallel "miscellaneous root fields" inventory.

### Validation and generation

`protocol/schema/generate_semantic_wire.mjs` imports the declaration, validates
it completely, renders all outputs in memory, and then either checks or writes
them. Validation rejects:

- duplicate symbolic identities, wire names, ordinals, snapshot fields, or
  delta fields;
- an ordinal that is not an explicitly declared nonnegative safe integer;
- an unknown lifecycle or replay policy;
- a retired semantic section, because retired fields belong to a prior-version
  rejection fixture rather than the active semantic inventory;
- a current section with no snapshot field or delta field;
- a compatibility-only section without compatibility replay policy, or a
  current section with compatibility replay policy;
- invalid JavaScript/C++ identifiers and non-snake-case wire names.

Every message ordinal must remain owned by exactly one declared current,
compatibility-only, or retired message. The generator cannot infer deleted
history, so an independent hand-authored reservation oracle pins all retired
message identities and ordinals; deleting or reassigning a reservation fails
that oracle even if newly generated files agree with the edited manifest.

Check mode writes no repository file and reports every stale or missing output.
Write mode validates and renders the complete set before opening an output,
writes temporary siblings, and replaces outputs only after all temporary writes
succeed. Reported replacement failures trigger best-effort restoration from
temporary backups and fail loudly. A process or machine failure during the
multi-file replacement is not claimed to be atomic; the next check reports the
partial set. This behavior is the same on Linux and Windows and does not depend
on rename-over-existing semantics. Output text uses fixed newlines, quoting,
headers, and declaration order and includes a generated-file marker naming the
source declaration.

The checked-in generated consumers are:

- a generated C++ detail header under
  `include/ssg/detail/generated/` containing the enum-declaration macro,
  constexpr typed message-kind facts, and ordered snapshot/delta field facts.
  Its `detail` path and namespace keep it outside the embedding contract while
  allowing the public `ProtocolMessageKind` declaration to consume the
  generated enumerators;
- a browser module under `apps/web/generated/` containing frozen message-kind
  and semantic-field facts.

The generated C++ facts drive `ProtocolMessageKind`,
`semanticSessionWireFieldNames`, and
`semanticSessionDeltaWireFieldNames`; no second arrays remain in
`Protocol.cpp`. The generated browser facts replace the test-local field
manifest. `tests/fixtures/protocol/session_semantic_fields.json` is deleted.
Step 1 does not generate codecs or expose generated headers as an embedding
contract.

### Build and repository integration

The protocol component registers a generator check test when repository tests
are enabled. It invokes the same checked-in Node script and fails on a dirty,
missing, or extra generated output. Normal library and consumer builds compile
the checked-in C++ output and do not run Node. The existing browser test imports
the checked-in generated module, so the normal and push gates exercise both
languages. A documented write command is the only supported regeneration path.

The generator itself is covered by a Node test that uses a temporary directory
and hand-authored invalid declarations. The test proves deterministic repeated
output, check-mode mismatch reporting, duplicate/invalid declaration
rejection, sparse retired message preservation, and no output writes before
complete validation/rendering. It does
not compare the generator to its own output as the sole oracle: C++ and browser
tests independently assert the known current/compatibility inventory and
canonical wire fixtures continue to round-trip byte-for-byte.

### Ownership and compatibility

The manifest owns names, ordering, lifecycle, replay-policy classification, and
message ordinals only. Domain C++ types continue to own product meaning.
`Protocol.cpp` continues to own typed conversion during Step 1, and
`reconcile.mjs` continues to own transactional replay. Neither consumer may
branch on lifecycle metadata at runtime. Step 2 will generate validation and
binding code from the same declaration rather than teaching the Step 1
generator an untyped escape hatch.

No protocol fixture is regenerated merely because ownership moved. Existing
canonical message bytes are the compatibility oracle. Any changed byte is a
Step 1 failure.

### Invariants

- **MANIFEST-FOUNDATION-1** — one typed declaration owns current and reserved
  message kinds plus semantic root-field names, ordering, lifecycle, and replay
  classification. State at `semantic_wire.mjs`.
- **MANIFEST-FOUNDATION-2** — checked-in C++ and browser facts are deterministic
  products of that declaration and cannot drift in a passing repository gate.
  State at the generator check.
- **MANIFEST-FOUNDATION-3** — Step 1 changes declaration ownership only; wire
  bytes, typed conversion, validation, and replay remain behaviorally
  identical. State at existing canonical fixtures and replay tests.
- **MANIFEST-FOUNDATION-4** — production and embedding builds do not require
  Node or execute a source-tree writer. State at the protocol CMake component.

### Acceptance

- Generator write followed by check is clean, and repeated writes are
  byte-identical.
- Invalid declarations fail before any output changes.
- C++ and browser consumers enumerate the same independently asserted semantic
  fields and message kinds from generated facts.
- Source inventory finds no handwritten semantic root-field arrays or
  test-local JSON field manifest. It also finds no lifecycle-metadata read
  outside `include/ssg/detail/generated/`, `apps/web/generated/`, and
  generator/tests.
- Every existing current protocol fixture remains byte-identical and all
  snapshot/delta replay tests pass.
- The removed JSON field manifest is the only protocol fixture changed by Step
  1; canonical hex fixtures are not regenerated.
- `scripts/check.sh` and `scripts/check.sh push` pass.
