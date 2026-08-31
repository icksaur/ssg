# plan-5-collapse-repeated-ui-identities

## Goals

Remove repeated representations of tree providers, transient controls,
actions, and focus so each semantic fact has one authoritative identity and one
normal update/input path.

## Design

Bind one tree surface to a typed provider identity and let `TreeModel` own the
active provider. Package schema generation, resolved node values, presence
basis, and focus path as one versioned UI-frame contract while allowing
internally efficient deltas. Represent prompt controls and actions through
normal typed nodes and command requests. Publish one typed focus path/context.
This plan authors the typed frame manually against the Plan 1 inventory; Plan 6
adopts that exact contract into its manifest/generator without redesigning it.

## Invariants

- IDENTITY-1: `TreeModel` is the sole authority for active tree-provider
  identity. State at `TreeModel`.
- IDENTITY-2: UI schema, node values, presence, and focus reconcile against one
  generation/basis and apply atomically. State at the UI-frame type.
- IDENTITY-3: User-visible actions use the normal typed command/input route with
  stale-generation rejection. State at action node/command request.
- IDENTITY-4: There is one authoritative focus representation. Hidden nodes
  cannot be the effectively focused endpoint; retained restoration targets
  earlier in the path may be hidden. State at focus-path authority.

## Considerations

Remove `PanelProvider`, repeated provider enums/surfaces/mappings, unused footer
prompt surface, redundant prompt projections, dedicated status-action message,
legacy focus projection, and special external-focus boolean only after
consumers migrate. One frame contract does not require resending the entire
schema for every value change.

## Risks and Mitigations

- Collapsing fields into an untyped property bag would weaken contracts: retain
  named typed node payloads and closed widget kinds.
- Focus migration can strand input: pin push/pop, hidden-node, prompt, picker,
  external modification, and reconnect cases first.
- Removing compatibility fields is wire-incompatible: version once and state
  the old-version removal condition.

## Acceptance (Definition of Done)

- Observable: tree/prompt/action/focus behavior remains equivalent in TUI and
  web; adding a tree provider requires no client layout/surface switch.
- Budgets: routine node-state changes do not resend immutable schema.
- Gates: `scripts/check.sh` and `scripts/check.sh push`.
- Oracles: provider lifecycle transitions; atomic UI-frame replay; stale action
  rejection; focus stack/path cases; protocol old-version refusal; a single
  node-value change has work and payload proportional to changed values rather
  than total schema nodes.

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Make one tree surface carry typed provider binding | `include/ssg/Widget.h`, `include/ssg/TreeModel.h`, whole-screen interaction/transition sources, clients | provider transition table | IDENTITY-1 |
| 2 | Introduce the atomic versioned UI-frame contract | UI schema/state/presence/focus headers, session snapshot and replay | round-trip and mismatched-basis rejection | IDENTITY-2, IDENTITY-4 |
| 3 | Move prompt controls and actions to normal node/command paths | whole-screen assembly, prompt/status sources, clients, protocol | stale action and prompt lifecycle cases | IDENTITY-3 |
| 4 | Replace legacy and special-case focus projections | `include/ssg/KeyboardFocus.h`, interaction authority, snapshots, clients | focus path/capture table | IDENTITY-2, IDENTITY-4 |
| 5 | Stage provider-enum removals after all clients use the bound tree surface | provider headers/codecs/clients/tests | provider inventory and Plan 1 compatibility policy | IDENTITY-1 |
| 6 | Stage footer-prompt and redundant prompt-projection removals after node consumers migrate | prompt/surface headers/codecs/clients/tests | prompt lifecycle parity and Plan 1 compatibility policy | IDENTITY-2 |
| 7 | Stage dedicated status-action message removal after command requests replace every sender | status/protocol/transport/client files | stale-action rejection and Plan 1 compatibility policy | IDENTITY-3 |
| 8 | Stage legacy focus and external-focus projection removals after all clients consume typed focus paths | focus/snapshot/protocol/client files | focus path parity and Plan 1 compatibility policy | IDENTITY-4 |

## Rationale

Repeated identities are expensive because every transition must synchronize
them. Removing them makes divergence unrepresentable. Physical wire removal of
the deprecated fields occurs in Plan 6 at `kSemanticUiWireVersion`.

## Step 1 design: one typed tree surface

Replace the provider-specific side-panel leaves with the static
`kTreeNodeId` leaf and `ViewSurface::Tree`. The canonical whole-screen schema
always uses that node when assembling the side panel; provider switching must
not replace a UI node, mutate layout vocabulary, or require a client layout
branch.

`TreeModel` is the sole authority for active-provider identity. Its published
tree state must carry an explicit `TreeProviderBinding` for the active provider
rather than asking consumers to infer identity from provider ordering. The
binding and the provider inventory are one semantic section and advance through
the existing snapshot/delta commit. The encoder continues to put the active
provider first for deterministic compatibility output during Plans 5 and 6, and
the decoder preserves valid encoded ordering. No consumer may use that ordering
to determine active identity after this step.

Remove `WholeScreenTruth::selectedProvider`. Whole-screen presence controls only
whether the generic tree leaf is present; it cannot encode which provider the
leaf displays. The solved panel, hit testing, renderer, terminal client, and web
client select tree content from the explicit active binding published by
`TreeModel`. `InteractionAuthority` produces presence-only truth, while
`TreeModel` alone produces active-binding semantic state; consumers read
`TreeViewState::activeBinding`, never presence, to resolve the active provider.

Provider-specific `ViewSurface` values and node identifiers remain decode-only
compatibility vocabulary during this increment. The encoder and schema builder
produce only `kTreeNodeId` with `ViewSurface::Tree`; the decoder accepts that
shape or the exact preceding panel child sequence `kFileTreeNodeId`,
`kGitStatusNodeId`, and `kSymbolsNodeId`, with their matching surfaces and no
additional panel child. The existing schema normalizer converts only that exact
preceding shape to the canonical generic leaf before support validation. Step 5
removes both the compatibility vocabulary and this normalizer after command and
client migrations are complete.

`ShowPanelProvider` and `SwitchPanelProvider` may keep `PanelProvider` as
boundary vocabulary until Step 5. `CommandTransition::prepare` converts once to
a `TreeProviderBinding`, validates that the exact id-and-kind binding exists,
and installs it through `TreeModel`. No later code path may carry the
compatibility enum, and it must not be retained in authoritative truth,
semantic output, solved output, or client state.

At every published, decoded, or replayed state, a non-empty
`TreeViewState::activeBinding` names exactly one provider whose id and kind both
match; empty tree state has no active binding and no providers. Snapshot decode
and delta replay validate the complete resulting `TreeViewState` before
publication and fail without exposing an invalid intermediate state. A delta
may change only the binding and compatibility ordering when the exact target
provider is already in the inventory; provider additions, removals, and
replacements remain valid only when the final binding-to-inventory relationship
holds.

Focused tests must prove that:

- the generic tree leaf keeps the same node identity and surface;
- the published active binding and provider inventory produce one valid
  semantic state after snapshot decode and delta replay;
- presence does not encode provider identity;
- unavailable, duplicate, missing, or kind-mismatched bindings are rejected
  before authoritative state changes;
- the terminal and web clients render the provider selected by the binding
  without provider-specific surfaces or layout selection, while node
  affordances remain data-driven;
- client contract tests contain no provider-specific surface selection in the
  render, focus, or hit paths; and
- the preceding schema shape normalizes to the same canonical generic leaf.

## Step 2 design: atomic UI frame

Introduce `UiFrame` as the only public semantic value that combines a
`UiSchema`, resolved `UiStateSection`, `UiPresenceSection`, and authoritative
focus path. `SessionSnapshotSections` stores one `UiFrame`, not independently
replaceable `ui`, `uiState`, and `uiPresence` members. Callers receive const
component accessors and can construct a frame only through validation; no
public mutation path may make its components disagree.

`UiFrameVersion` is a value type with `Generation generation` and
`PresenceBasis presenceBasis`. Snapshot and delta base/target seams encode it as
one object rather than as unrelated scalar fields. A valid frame satisfies all
of these conditions, checked in this order:

- schema validity and the whole-screen well-known-area contract hold;
- schema, resolved state, and presence name the same generation;
- state and presence each contain exactly one record for every schema node and
  no other record;
- the focus path is non-empty, every entry names a node id in the current
  schema, and its final effective node is effectively present.

A node is effectively present only when its own presence record and every
ancestor's presence record are present. Focus validation applies that rule to
the path's final effective node. Earlier entries are retained restoration
targets and may be hidden while a later capture is effective, as when a picker
hides the editor under its prompt capture. `UiFrame` therefore stores a
non-optional focus path; absence is not a valid published state. Removing a
focus entry and restoring the new endpoint's effective presence must happen in
the same atomic commit. A commit that exposes a hidden endpoint rejects without
publishing any part of the transition.

The runtime constructs the schema, node values, presence, and focus path as
local prospective values, validates one `UiFrame`, and publishes it only after
validation succeeds. Validation failure is explicit at decode/replay
boundaries; trusted in-process assembly fails loudly rather than publishing a
partial frame.

Introduce `UiFrameDelta` and `UiFrameDeltaCodec`. A delta carries
`UiFrameVersion base` and `UiFrameVersion target`; replay first requires
`base == current.version()` and reports a stale-version error separately from a
malformed body. Its body is a tagged union of:

- `UiFrameReplacement`, containing exactly one complete validated `UiFrame`;
  or
- `UiFrameChanges`, containing only incremental changes within one schema
  generation.

A schema-generation change requires `UiFrameReplacement`.
`UiFrameChanges` carries only:

- changed `UiNodeState` records, keyed by node id;
- changed `UiPresenceRecord` records, keyed by node id;
- the target presence basis; and
- an explicit changed flag plus optional replacement for the focus path.

Incremental record sets reject duplicate or unknown ids. Replay applies all
changes to the unchanged records from the base frame in a private candidate.
The incremental sets are subsets of the schema node set; the resulting frame
must still contain exactly one state and presence record per schema node before
publication. The tagged body makes replacement-plus-incremental construction
impossible. Replay rejects a stale base generation or basis, a regressing or
inconsistent target basis, and any final component or focus mismatch. A
node-value-only change may retain the presence basis. Any changed presence
record requires a strictly advancing presence basis; a basis may advance
without a record change to acknowledge an accepted no-op presence mutation.

The current encoder emits one `ui_frame` snapshot field and one
`ui_frame_delta` field. Their nested fields carry the typed components above;
routine deltas do not resend immutable schema or unchanged node records. The
preceding split `ui`, `ui_state`, and `ui_presence` fields become decode-only
compatibility vocabulary. A preceding snapshot must contain the schema, state,
and presence triplet together. The decoder extracts the preceding optional
focus path from `ui_state`; when absent, it derives one from the legacy
authoritative focus and the unique present focus host, rejecting ambiguity or a
missing host. It validates and converts the complete result into one `UiFrame`.

A preceding delta may contain any subset of the three split replacement fields,
because omitted fields meant unchanged. Decode represents that only as a third,
decode-only `LegacyUiFrameChanges` body. Replay applies the supplied full
component replacements to a private candidate and validates one resulting
frame; production delta derivation and the new encoder cannot construct this
body. The decoder rejects mixed new-and-preceding frame fields rather than
choosing one. Plan 6 removes the split field decoders,
`LegacyUiFrameChanges`, and their fixtures when it performs the
`kSemanticUiWireVersion` compatibility removal.

The web client retains one frame object and applies `ui_frame_delta`
transactionally through the same base/target and final-state rules. Rendering,
focus, and presence accessors read that frame; no client stores split mutable
copies. The terminal path reads the same `UiFrame` components from the semantic
snapshot.

Outside compatibility codecs, inventory tests permit no
`SessionSnapshotSections::ui`, `uiState`, or `uiPresence` members and no split
client state. `apps/ssg_main.cpp` and `apps/web/client.mjs` consume `UiFrame`
components through the frame contract.

Focused tests must prove that:

- independently mismatched schema, state, presence, or focus cannot construct
  a frame;
- stale generation and stale presence-basis deltas reject without mutation;
- schema replacement is atomic and cannot mix with incremental records;
- duplicate, unknown, hidden-effective-focus, and ancestor-hidden effective
  focus records reject, while a hidden retained base under a visible capture is
  accepted;
- snapshot and delta wire round trips preserve one frame;
- the exact preceding split triplet decodes to the same frame, while incomplete
  snapshot or mixed forms reject, and preceding subset deltas replay atomically;
  and
- changing one equivalent resolved node in both a small and a much larger
  schema emits one node record and no schema in either case, with encoded delta
  size differing only by the encoded node identity rather than total
  schema-node count.

## Step 3 design: node-native prompt and status actions

All current client activation of published UI controls converges on one ordinary
typed command, `ui.activate`. Its `UiNodeActivationArguments` carry only the
observed schema generation and `UiNodeId`; the enclosing `CommandRequest`
already carries the exact semantic base revision. The client never supplies the
target command, prompt kind, control index, status identity, or action payload.

Add `CommandEffect::Routing` and `CommandSpecBuilder::routes()` for commands
whose only accepted effect is to enqueue authoritative follow-up commands.
Routing commands require an exact base revision, may not require a view action,
and do not advance the semantic revision themselves. The command executor drains
their queued command before returning, rewriting the deferred command's base to
the then-current revision exactly as for existing aggregate dispatch. This
keeps one user activation from fabricating an intermediate revision while the
target command still receives the original principal's capability checks,
argument contract, and normal mutation semantics.

A routing handler either rejects before queuing anything or succeeds after
queuing exactly one target. The direct target may not itself be a routing
command; ordinary target commands may use the existing deferred-command chain.
The routing command publishes no independent success: the final target-chain
result is the public `CommandResult`, and any target rejection is returned as
the activation's rejection. A rejected routing handler clears its request
without draining, while the existing error and view-action drain rules continue
to clear any later queued work.

The `ui.activate` handler resolves a fresh `UiFrame` from the runtime and rejects
unless:

- the argument generation equals the frame generation;
- the node exists exactly once in the schema and state;
- the node and every schema ancestor are present;
- the node is a stateful `TextInput`, or is a `Checkbox` or `Field` whose leaf
  state carries a bound command target; and
- an ordinary command target takes no arguments.

Resolution is server-side from the current schema and leaf state. Ordinary
fields and checkboxes enqueue their current leaf command. A provider-backed,
stateful `TextInput` instead enqueues `prompt.focus_control` with the schema
widget's control id; its leaf command remains the typed value-update operation
and is never dispatched with an invented empty payload. A state-free input such
as the client-owned picker query, a count label, a container, and any leaf
without a command are inert. Unknown, hidden, stale, malformed, argument-taking,
or retired targets reject without queuing work.

The preceding `PublishedUiActionPointerInput` decoder delegates to this same
resolver during the compatibility window. `PromptControlPointerInput` and
`StatusActionPointerInput` retain their exact decoders and frozen fixtures but
are no longer constructed by current clients. Their compatibility handlers
remain isolated until the corresponding Step 6 and Step 7 removals.

### Prompt controls

`PromptSurface` remains the authority for the active request, values, toggles,
and active input. The existing prompt-control lowering remains the one internal
semantic conversion used to build the request-derived footer subtree and
resolve its `UiNodeState`; live find/replace state is overlaid at that resolver,
not through `PromptView`.

The current grid path traverses the `footer.prompt` subtree in `UiFrame`, joins
it to the solved node tree by `UiNodeId`, and derives captions, checked state,
active input, accessibility labels, and hit targets from those nodes. It does
not enumerate `PromptView::controls`. Text inputs hit `ui.activate` for focus,
checkboxes hit the node's command, and count labels are inert. Keyboard submit,
cancel, navigation, focus cycling, and text updates continue through their
existing authoritative keymap commands.

`PromptView`, `PromptControl`, `PromptViewSectionDelta`, legacy
`PromptViewState`, and their codecs remain compatibility projections in this
step, but no current terminal or web render/input path consumes them. Step 6
stops producing and then stages removal of that compatibility vocabulary. The
unused terminal prompt mirror variables are deleted now.

### Status actions

The built-in footer's `footer.status_actions` node becomes an ordinary
container whose children are actionable field nodes for the selected
`StatusQueue` entry. A composed footer still replaces the complete built-in
footer and therefore has no built-in status-action container or children.

The library derives a typed `StatusActionNode` projection from the selected
status entry. Each projection contains a collision-free opaque `UiNodeId` plus
the accessible label and payloadless target command. Identity uses the canonical text
`footer.status_action/<status-id>/<generation>/<action-id-hex>`, where the
numeric strong ids use canonical unsigned decimal and `action-id-hex` is
lowercase hexadecimal over the action id's UTF-8 bytes. The separators cannot
occur in the encoded components, so changing any component cannot collide;
clients never construct or parse the identity. The child order is authoritative
queue order. Label and target resolution ride `UiStateSection`; an unchanged
identity can therefore change state without changing schema. An identity-set or
order change replaces the subtree and advances the whole-screen schema
generation.

`InteractionAuthority` owns the current status-action projection alongside its
base composition. One rebuilding operation takes the base composition, retained
status projection, and current `PromptSurface`, applies the status-action
overlay when the built-in anchor exists, then applies the active footer-prompt
overlay, and produces one candidate `WholeScreenSchema` plus matching
`UiInteractionState` before swapping any owner.

Installing a composed footer retains the status projection in the authority but
publishes none of its nodes; restoring the built-in footer republishes those
same identities if the selected status is unchanged. A status refresh while any
prompt is open rebuilds through the complete pipeline, preserves the prompt and
its capture, and overlays the prompt against the new candidate schema. It never
closes a picker or footer prompt as a side effect.

Runtime command draining reconciles the selected status projection under the
operation lock after each command. The only other current writers,
`publishStatusValue` and `enqueueStatus`, perform the same runtime-thread
reconciliation immediately after `StatusQueue::enqueue`, before a snapshot can
observe the queue. Worker threads may not write `StatusQueue` directly; they
must post through the existing serialized runtime publication seam.

The status target command is resolved from the current node state and must be a
registered payloadless command. `StatusAction` has no payload field, so an
argument-taking target is unsupported and activation rejects explicitly.
Executing a status action queues that target command; it does not merely validate
the old `StatusActionInvocation`.

Current chrome lowering, rendering, accessibility, hit testing, and both clients
consume these ordinary nodes. Synthetic `SolvedChromeItem::statusInvocation`,
current `AccessibilityNode::statusInvocation`, dedicated prompt/status hit
regions, and client-side status inventories leave the current path. Exact old
schema and accessibility decoding remains in the legacy adapter until Step 7;
the sidecar members may remain physically present but unused until that removal.

Focused tests must prove that:

- current revision, generation, effective presence, node identity, leaf
  actionability, and payloadless target validation all gate `ui.activate`
  without partial mutation;
- a routing command advances no revision of its own, while its deferred target
  executes once under the same principal and publishes the target's normal
  result;
- prompt open, default active input, pointer focus, focus cycling, value update,
  toggle activation, inert count, close, shape-changing reopen, and stale
  pre-close activation are observable through `UiFrame` nodes. A pre-close
  request rejects first at the ordinary exact base-revision check; it introduces
  no prompt-specific stale outcome. Every valid prompt request has at least one
  input by `PromptSurface` construction, and a malformed control-free request
  rejects before publication. Non-control prompt regions have no activation and
  are inert to pointer hits;
- selected status action nodes track enqueue, replacement, next, previous, and
  dismissal, changing any status-identity component changes the collision-free
  node id, and activation executes the current target once;
- terminal and web presentation, accessibility, and hits use the same node ids,
  labels, commands, active state, and checked state;
- every published target command remains keyboard reachable through an
  applicable keymap binding or the command palette;
- current clients emit an ordinary `CommandRequest` for node activation and
  contain no construction of the three preceding feature-specific input
  variants; and
- frozen preceding client-input, prompt-view, status-view, schema, and
  accessibility fixtures continue to decode through compatibility paths.

## Step 4 design: one typed focus path

### Goals

Current terminal and browser behavior derives keyboard routing, active-surface
presentation, DOM focus, and local prediction eligibility from the `UiFrame`
focus path. `SessionSnapshotSections::focus` and `externalFocusHeld` cease to be
independent current state. Preceding peers remain wire-compatible until Plan 6.

### Design

`UiNode` gains an optional typed `FocusTarget` declaration. The whole-screen
assembly assigns it only to nodes that can hold keyboard focus: the editor,
panel, header prompt input, footer prompt, and external-modification surface.
The declaration publishes keymap context, not geometry or client policy.
`FocusTarget` remains the closed keymap-context vocabulary. This is deliberately
not a second `FocusHostKind`: a host declares the context in which the existing
keymap resolves input. Every `FocusTarget` value is a valid declaration, and
whole-screen schema tests pin the semantic assignments of the built-in hosts.

The authoritative focus value remains the ordered stack of base and captured
`UiNodeId` values in `UiFrame`. It is not a tree-ancestor path: consecutive
entries need not be parent and child. Its effective context is derived by
resolving the final node through the matching schema. The complete stack is
retained for restoration; clients do not infer context from well-known ids and
do not combine a path with a second effective-focus flag.

`FocusCapture` retains only node identity. `UiInteractionState` admits a base or
capture only when the addressed node belongs to its schema and declares the
required focus role. It derives `effectiveFocus()` from the current endpoint.
`WholeScreenTruth::externalFocusHeld` remains internal operational truth while
the pure whole-screen rebuild uses it to restore the external capture across
unrelated schema and presence rebuilds. It is not published as current client
state. Removing that internal rebuilding input would require replacing the
truth-driven interaction transition seam and is outside this projection step.

Implementation is staged:

**Current-path migration.** Schema encoding carries optional focus context.
`UiFrame` requires each focus-path entry to be a declared focus host and the
effective endpoint to be effectively present; hidden restoration entries remain
valid. Terminal routing and rendering, legacy shell adaptation, browser keymap
routing, picker opening, local text prediction, and DOM focus all resolve the
frame endpoint and its declared context. Picker prediction remains a
client-local overlay that appends the published header-input host and reads its
published context. The browser removes its root-focus fallback for a current
frame.

**Compatibility isolation.** `SessionSnapshotSections` drops `focus` and
`externalFocusHeld`; current `SessionDelta` behavior no longer replays them
beside a frame delta. The protocol codec continues to emit preceding root
`focus` and `external_focus_held` fields, derived from the frame at the transport
edge. The legacy focus value is the last path context other than
`ExternalModification`; the external flag is true only when the effective
endpoint context is `ExternalModification`.

Preceding snapshots that lack a frame focus path synthesize one from their root
focus fields, schema, and effective presence using this closed algorithm:

- `Editor` selects the built-in editor host;
- `Panel` selects the built-in panel host;
- `Prompt` selects the only effectively present built-in header or footer prompt
  host and uses the editor host as its compatibility-only restoration base;
- a true `external_focus_held` appends the effectively present built-in external
  host to an `Editor` or `Panel` base; `Prompt` plus true external focus rejects
  because a held prompt would be the effective capture.

Every selected host must exist, declare the matching context, and satisfy the
required effective presence; otherwise decode rejects. The algorithm never
consults retained process state. The editor restoration base for a preceding
prompt snapshot is intentional: the scalar cannot encode whether editor or
panel was underneath, and only its effective prompt focus is a preceding wire
promise.
The compatibility-only editor restoration choice disappears with this decoder
in Plan 6; until then, closing the prompt is settled by a later authoritative
snapshot or delta rather than by client-side restoration.

A preceding focus-only delta is not synthesized into an independently versioned
`UiFrameDelta`. `SessionSnapshotCodec::replay` first replays the supplied frame
delta against its real base. It derives the base compatibility pair from the
base frame, applies any root `focus` and `external_focus_held` replacements, and
runs the same closed algorithm against the replay candidate. If that frame delta
explicitly supplied a path, the synthesized and supplied effective
compatibility pairs must agree or the entire session delta rejects. Otherwise
replay constructs the final frame from the candidate schema, state records,
presence, version, and synthesized path. No partially reconciled
`SessionSnapshotSections` escapes.

Absent root compatibility fields impose no comparison and cannot conflict with
an explicitly changed frame path; only a present root field that disagrees
rejects. A conforming compatibility emitter nevertheless includes each root
field whenever its derived value changes so preceding receivers observe focus.

Snapshot compatibility emission derives both root fields from the frame. Delta
compatibility emission derives the pair from the base and target frames: it
emits `focus` exactly when derived legacy focus changes, and
`external_focus_held` exactly when the endpoint's
`ExternalModification`-context status changes. Legacy focus is found by scanning
the stack from top to base for the first non-`ExternalModification` context.
When a message carries both a current frame path and compatibility focus fields,
they must describe the same derived pair or decoding rejects. The old root
`focus` vocabulary remains restricted to its preceding closed set and never
gains `ExternalModification`.

Plan 6 removes the compatibility fields and normalization. Step 4 does not
renumber wire vocabulary or retain a second current behavior path.

Steps 1 through 4 form increment 4A and are reviewed and committed together.
Steps 5 through 7 form increment 4B and receive a separate implementation review
and commit. The compatibility fixtures remain through 4B and are removed only
with their fields in Plan 6.

### Invariants

- **FOCUS-PATH-1** — `UiFrame` focus is non-empty, generation-matched, and every
  entry names a schema node with a focus-context declaration. This remains on
  `UiFrame::require` and its focused tests.
- **FOCUS-PATH-2** — only the final path entry must be effectively present;
  earlier restoration entries may be hidden. This remains on `UiFrame::require`.
- **FOCUS-PATH-3** — effective keymap context is derived only from the final
  authoritative node and its schema declaration. This is stated on the `UiNode`
  focus-context field and `UiFrame` context query.
- **FOCUS-PATH-4** — schema, presence, path, and derived context change as one
  frame; no current snapshot or client field may override them. This is stated
  on `UiFrame`.
- **FOCUS-PATH-5** — external focus never appears reactively, survives unrelated
  rebuilds while held, yields to a prompt capture, restores after prompt close,
  and clears when its node disappears. Existing authority tests retain this
  behavior.
- **FOCUS-COMPAT-1** — compatibility focus is derived at the codec edge; mixed
  current and preceding representations that disagree reject transactionally.
  This remains on the protocol codec boundary until Plan 6.

### Considerations

- `UiNode` is an aggregate used by authored, decoded, and test schemas. The new
  field is last and optional so existing aggregate construction remains source
  compatible; current whole-screen assembly explicitly authors every focus host.
- Existing three-argument `UiNode{id, size, content}` construction remains valid
  because the optional declaration is the final aggregate member.
- Frozen schemas predate node focus context. Exact preceding decode annotates
  only the known preceding focus hosts before frame validation. Current authored
  schemas do not receive inferred metadata.
- A restoration entry can be hidden, so clients must not require direct or
  effective presence for every path entry. They validate effective presence only
  for the endpoint using ancestor-aware presence.
- Browser-local picker prediction may append only the published header prompt
  input on the user keystroke that opens the picker. It does not invent a
  context or mutate retained authoritative state; later frame settlement either
  confirms or removes the overlay.
- The retiring browser fallback used root `focus` only when
  `resolveUiFocusPath` reported that the retained frame had no path. Current
  frames require a path, so absence becomes malformed current state rather than
  selecting a second focus authority.
- Rendering uses endpoint identity where location matters and endpoint context
  only where semantic routing matters. Context must not become a geometry proxy.
- Legacy shell and presentation codecs remain compatibility adapters. They may
  derive old focus values from a frame but never feed current runtime behavior.

### Risks and Mitigations

- **Risk:** an old focus-only delta leaves the retained frame path stale.
  **Mitigation:** replay the real frame delta first, resolve compatibility
  fields against the replay base and candidate, then construct one validated
  final frame; test the algorithm against frozen deltas.
- **Risk:** path and context metadata disagree across schema generations.
  **Mitigation:** frame construction rejects undeclared hosts and schema
  replacement carries path, presence, and context together.
- **Risk:** removing browser fallback breaks locally predicted picker focus.
  **Mitigation:** retain the existing prediction overlay, but resolve its host
  and context from the current schema and test open/settle/rollback.
- **Risk:** compatibility emission widens the old `FocusTarget` wire set.
  **Mitigation:** derive legacy focus by skipping external contexts and retain
  the old decoder's closed-set rejection.

### Acceptance (Definition of Done)

- **Observable:** editor, panel, prompt, picker, and external-modification keys
  route identically in terminal and browser; prompt-over-external restoration and
  browser local echo behave unchanged.
- **Budgets:** focus changes remain sparse frame-state/presence deltas and do not
  replace an unchanged schema.
- **Gates:** `scripts/check.sh` and `scripts/check.sh push` pass.
- **Oracles:** schema round trip preserves optional focus context; malformed
  paths reject undeclared hosts and hidden endpoints; restoration paths admit
  hidden bases; capture LIFO and external/prompt behavior retain their focused
  truth tables; terminal and browser derive the same context from a shared
  frame; browser picker prediction overlays without mutating the frame; current
  mixed representations reject on disagreement; a table-driven independent
  compatibility reference maps every legal legacy focus/external/presence
  combination and rejects ambiguous or absent-host combinations; frozen
  preceding snapshots and focus-only deltas match that reference.

### Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Publish typed focus-host context and validate paths | `include/ssg/UiTree.h`, `src/UiTree.cpp`, `src/WholeScreenAssembly.cpp`, `include/ssg/UiFrame.h`, `src/UiFrame.cpp`, schema/frame/protocol tests | round trip plus malformed-host and hidden-restoration cases | FOCUS-PATH-1, FOCUS-PATH-2, FOCUS-PATH-3 |
| 2 | Collapse capture context into schema-derived context | `include/ssg/KeyboardFocus.h`, `include/ssg/InteractionState.h`, `src/WholeScreenInteraction.cpp`, authority/focus tests | capture, prompt-over-external, hide-and-restore truth tables | FOCUS-PATH-3, FOCUS-PATH-5 |
| 3 | Migrate terminal and shell consumers | `apps/ssg_main.cpp`, `src/Renderer.cpp`, `src/EditorSession.cpp`, terminal/render/hit tests | one frame routes and marks editor, panel, prompt, and external contexts | FOCUS-PATH-3, FOCUS-PATH-4 |
| 4 | Migrate browser routing, focus, and prediction | `apps/web/client.mjs`, `apps/web/reconcile.mjs`, `tests/web/test_reconcile.mjs` | shared frame context, ancestor-effective endpoint, picker prediction, local-echo cases | FOCUS-PATH-2, FOCUS-PATH-3, FOCUS-PATH-4 |
| 5 | Remove current snapshot and delta duplicates | `include/ssg/session_snapshot.h`, `src/session_snapshot.cpp`, `src/runtime/snapshot.cpp`, snapshot/delta tests | focus changes replay solely through `UiFrameDelta` | FOCUS-PATH-4 |
| 6 | Isolate preceding wire compatibility | `src/Protocol.cpp`, protocol fixtures/tests | independent legacy-pair reference versus snapshot decode and base-to-candidate delta replay; mixed conflict rejection; frozen snapshot and focus-only delta replay | FOCUS-COMPAT-1 |
| 7 | Regenerate current artifacts and remove dead consumers | generated command/protocol artifacts, source-inventory tests | no current read of root focus fields; frozen preceding fixtures unchanged | FOCUS-PATH-4, FOCUS-COMPAT-1 |

### Rationale

Deriving context from well-known ids in every client would replace two snapshot
fields with duplicate maps and make each new focus host a coordinated client
change. Publishing context on the authoritative host keeps identity and semantic
routing together while leaving native layout and device focus in each client.

Removing `WholeScreenTruth::externalFocusHeld` in the same increment was
rejected. It is not a client projection: it records the user's focus choice so a
pure interaction rebuild can reconstruct the capture. Replacing it requires a
different transition model and is separable from removing snapshot and wire
duplication.

## Step 5 design: one provider identity vocabulary

### Goals

Panel commands, transitions, tree state, and current UI schemas use
`TreeProviderBinding` and `ViewSurface::Tree` directly. The duplicate
`PanelProvider` domain and provider-specific current view surfaces disappear
without changing panel behavior or breaking the frozen preceding schema during
the Plan 6 compatibility window.

### Design

`TreeProviderBinding` becomes the only value carried by `ShowPanelProvider`,
`SwitchPanelProvider`, provider cycling, transition preflight, and runtime
command dispatch. `CommandTransition` retains one built-in panel-provider
catalog expressed as bindings in the established Filesystem, Git, Symbols
cycle order. Lookup by `TreeProviderKind`, exact-binding validation, and cycle
movement all consume that catalog. This keeps the product inventory and
canonical id-and-kind correspondence in one library-owned place without
introducing a replacement enum. A request whose binding is absent from the
catalog, including a known id paired with the wrong kind, rejects during
preflight before provider lookup or mutation. `treeProviderLabel` remains the
label conversion for the binding's kind.

`PanelProvider`, `panelProviderLabel`, and `panelProviderTreeBinding` are
deleted. Transition preflight receives the final binding and continues to
validate both id and kind before preparing any replacement. The named
show-files and show-git commands resolve their built-in bindings at the runtime
command boundary; next/previous commands resolve the adjacent binding from the
same catalog. No client learns the catalog or maps a provider to layout.

Whether a missing provider may be created empty remains a `TreeProviderKind`
rule owned by `TreeModel`, exposed through one named query used by both
`TreeModel::activateOrCreate` and transition preflight. Filesystem returns false;
Git and Symbols return true. A missing non-creatable binding rejects preflight;
creatable bindings receive the existing revision-stamped empty replacement.
The catalog does not duplicate this policy as another flag.

The current `ViewSurface` vocabulary removes `FileTree`, `GitStatus`, and
`Symbols`. Each retained enumerator keeps exactly its pre-Step-5 underlying wire
value through an explicit enum assignment; the frozen current-schema fixture
and the independent hand-authored ordinal table are the value oracles. The gaps formerly occupied by retired
surfaces remain reserved and are not compacted. `kAllViewSurfaces` and
`ClientUiProfile` enumerate only TabBar, FindResults, FooterPrompt, Notice,
ExternalModification, Document, and Tree densely. Name lookup, support
validation, and backing-section lookup reject values outside that current set.

The frozen preceding schema still uses the retired surface ordinals. During the
compatibility window, `decodeUiSchema` recognizes those ordinals only in a
decode-local compatibility representation. The accepted panel sequence is
exactly `(kFileTreeNodeId, retired FileTree ordinal)`,
`(kGitStatusNodeId, retired GitStatus ordinal)`,
`(kSymbolsNodeId, retired Symbols ordinal)`, as constructed independently in
the UI-schema codec oracle, with no additional panel child. Only that
complete ordered sequence normalizes directly to the canonical
`kTreeNodeId`/`ViewSurface::Tree` leaf before a `UiSchema` is published. A
current `ViewSurface` value never represents a retired provider surface, and a
partial, reordered, mixed, or stray legacy shape rejects. Current encoding
never emits a retired ordinal. Plan 6 deletes this decode-local representation
when it activates the new semantic wire version.

The browser removes retired provider-surface constants from its current
`SURFACE` vocabulary and profile without compacting retained values; every
retained browser surface value remains equal to the corresponding C++ wire
value. It continues to receive only server-normalized current schemas; there is
no browser-side provider-surface compatibility path.

### Invariants

- **PROVIDER-IDENTITY-1** — tree-provider selection crosses command and
  transition seams only as an exact `TreeProviderBinding`; id and kind are
  validated together before mutation. State at `ShowPanelProvider`,
  `SwitchPanelProvider`, and transition preflight.
- **PROVIDER-IDENTITY-2** — the built-in panel-provider inventory and cycle
  order have one library-owned binding catalog in Filesystem, Git, Symbols
  order. State at the catalog accessor in `CommandTransition`.
- **PROVIDER-IDENTITY-3** — empty-provider creatability is one
  `TreeProviderKind` rule consumed by tree activation and transition preflight.
  State at the named query in `TreeModel`.
- **PROVIDER-SURFACE-1** — current schema, profile, render, and backing
  vocabulary has only generic `ViewSurface::Tree`; provider identity comes from
  `TreeViewState::activeBinding`. State at `ViewSurface` and
  `TreeViewState`.
- **PROVIDER-COMPAT-1** — retired provider-surface ordinals are accepted only
  by the exact preceding-schema decoder and cannot escape as current typed
  values. State at `decodeUiSchema` until Plan 6.

### Considerations

- Removing enum members must not renumber retained wire surfaces. Explicit
  `ViewSurface` values preserve all retained ordinals; dense profile indexing
  must not cast those sparse wire values directly to array positions.
- A binding whose id exists under another kind is malformed and rejects; it is
  never treated as a request to replace that provider. Catalog membership is
  checked by transition preflight before inspecting present providers.
- The filesystem provider remains non-creatable when absent. Git and symbols
  retain lazy empty creation with revisions allocated by the existing
  transition preflight.
- Cycling from a binding outside the built-in catalog is a hard corruption
  error, not an inferred position or fallback.
- `ShellNodeKind::PanelProvider` names a frozen legacy presentation node, not
  provider identity. It remains until Plan 6 removes the presentation envelope.
  The forbidden-symbol oracle explicitly excludes this qualified symbol.
- Provider-specific node-id constants remain only where the exact preceding
  schema decoder identifies its frozen shape; they are not current schema
  vocabulary.

### Risks and Mitigations

- **Risk:** enum deletion silently changes wire values. **Mitigation:** retain
  explicit ordinals and compare encoded current surfaces with frozen fixtures.
- **Risk:** canonicalizing each retired ordinal independently admits malformed
  legacy schemas. **Mitigation:** collect decode-local retired meanings and
  normalize only after the complete exact panel sequence matches.
- **Risk:** replacing `PanelProvider` with loose ids loses kind validation.
  **Mitigation:** carry `TreeProviderBinding` through every command transition
  and retain mismatch rejection before mutation.
- **Risk:** browser current vocabulary continues advertising dead widgets.
  **Mitigation:** pin the exported surface key set and profile against the
  current C++ inventory.

### Acceptance (Definition of Done)

- **Observable:** files, git, and symbols panel commands, reselect-to-hide,
  next/previous cycling, focus preservation, and lazy provider creation behave
  unchanged in terminal and web clients.
- **Budgets:** provider switches remain ordinary tree-binding/state changes and
  do not replace the UI schema or add protocol fields.
- **Gates:** `scripts/check.sh` and `scripts/check.sh push`.
- **Oracles:** binding-based transition truth tables cover show, switch,
  unavailable filesystem, kind mismatch, lazy creation, reselect, and cycle;
  an independent current-surface name inventory excludes retired surfaces and
  frozen current-schema bytes and a hand-authored ordinal table pin retained
  wire values; the independently constructed preceding schema decodes to one generic
  tree leaf while partial, reordered, mixed, and stray retired forms reject;
  browser inventory/profile parity contains no retired provider surfaces; source
  inventory contains no unqualified `PanelProvider`,
  `panelProviderLabel`, `panelProviderTreeBinding`, or provider-specific
  `ViewSurface` member; `ShellNodeKind::PanelProvider` is an explicit
  compatibility exception.

### Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Add current-provider and surface-inventory oracles | `tests/test_command_transition.cpp`, `tests/test_ui_view_surface.cpp`, `tests/test_ui_tree_protocol.cpp`, `tests/web/test_reconcile.mjs` | independent binding transition table, current surface names, frozen exact/malformed schemas, browser key set | PROVIDER-IDENTITY-1, PROVIDER-IDENTITY-2, PROVIDER-SURFACE-1, PROVIDER-COMPAT-1 |
| 2 | Replace panel enum boundaries with exact bindings and one catalog | `include/ssg/WholeScreenInteraction.h`, `include/ssg/CommandTransition.h`, `include/ssg/TreeModel.h`, `src/CommandTransition.cpp`, `src/TreeModel.cpp`, `src/runtime/presentation.cpp`, interaction/transition/session tests | binding transition table, cycle cases, and shared creatability truth table | PROVIDER-IDENTITY-1, PROVIDER-IDENTITY-2, PROVIDER-IDENTITY-3 |
| 3 | Remove provider-specific current surfaces while preserving exact decode compatibility | `include/ssg/Widget.h`, `include/ssg/UiProfile.h`, `src/Widget.cpp`, `src/UiTree.cpp`, `src/UiTreeProtocol.cpp`, `src/ViewSurfaceBacking.cpp`, surface/schema/profile tests | current name/ordinal inventory plus frozen schema decode/rejection | PROVIDER-SURFACE-1, PROVIDER-COMPAT-1 |
| 4 | Remove dead browser vocabulary and enforce source inventory | `apps/web/reconcile.mjs`, browser and source-inventory tests | C++/browser current surface parity and forbidden-symbol scan | PROVIDER-IDENTITY-1, PROVIDER-SURFACE-1 |

### Rationale

Keeping provider selection as an enum at the command boundary would preserve
the same duplicate identity under a smaller scope: every new provider would
still require an enum case plus a binding map. Carrying the binding itself makes
the transition API reuse the value already owned and published by `TreeModel`.

Renumbering `ViewSurface` or dropping old schema decode now would create an
unrelated wire break before Plan 6. Decode-local retired meanings preserve that
single compatibility obligation without allowing retired provider surfaces
back into current product types or clients. Compacting retained surface values
now is rejected for the same reason: Plan 6, not this cleanup, owns any wire
break.
