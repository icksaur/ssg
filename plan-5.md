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
- IDENTITY-4: There is one authoritative focus representation and hidden nodes
  cannot retain effective focus. State at focus-path authority.

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
