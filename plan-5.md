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
