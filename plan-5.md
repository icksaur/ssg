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
