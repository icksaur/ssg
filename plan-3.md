# plan-3-separate-semantic-state-from-grid-presentation

## Goals

Make the general session and wire contracts semantic-only. Keep reusable
terminal presentation in an optional grid adapter without exposing cells,
rectangles, viewport dimensions, or shell geometry to native-layout clients.

## Design

Split `SessionSnapshot` into an authoritative semantic frame and a separately
owned grid projection. Each grid presenter is per-view and single-owner; it
consumes immutable revision-keyed semantic snapshots and never observes a
mid-frame mutation. HTTP publishes only semantic frames. The TUI composes
`EditorSession` with the grid presenter in-process. This plan retains a
deprecated presentation bridge until Plan 4 supplies the UI-tree solver and
Plan 6 activates `kSemanticUiWireVersion`; it does not create a permanent
second path.

## Invariants

- FRAME-1: The semantic frame is complete without client geometry. State at the
  semantic snapshot type.
- FRAME-2: Grid state cannot enter the general protocol contract. State at the
  protocol snapshot codec.
- FRAME-3: Each client owns native layout and device geometry; optional library
  presentation consumes semantic state but cannot mutate product state. State
  at the grid presenter API.
- FRAME-4: Per-view presentation caches are isolated by view identity and
  invalidated by their semantic basis. State at grid presentation state.

## Considerations

Move shell layout, viewport, style, prompt grid projection, selection reveal,
and tree windows together. Preserve terminal behavior and hit testing. Do not
create a second semantic snapshot assembled independently from the existing
authoritative sections.

## Risks and Mitigations

- Splitting ownership may duplicate snapshots: pass immutable semantic values
  or revision-keyed views into the presenter.
- Presentation currently causes some view-state updates: identify and replace
  hidden mutation with explicit typed presentation state.
- Wire removal follows the compatibility policy in Plan 1. This plan marks the
  bridge deprecated; Plan 6 removes it after all in-tree consumers migrate.

## Acceptance (Definition of Done)

- Observable: terminal output/hits remain equivalent; web consumes only the
  semantic side of the split while the deprecated wire bridge remains until
  Plan 6.
- Budgets: semantic snapshot cost does not acquire grid work; grid projection
  remains viewport-bounded where currently guaranteed.
- Gates: `scripts/check.sh` and `scripts/check.sh push`.
- Oracles: semantic snapshot equality with and without a grid presenter;
  terminal cell/hit parity; protocol fixtures reject removed geometry;
  per-view isolation cases.

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Introduce the semantic-frame and per-view grid-projection ownership seam | `include/ssg/session_snapshot.h`, `include/ssg/EditorSession.h`, new/existing grid presenter headers | invariant: dimensionless frame is complete | FRAME-1, FRAME-3 |
| 2 | Move grid-only state and caches out of `EditorSession::Impl` into single-owner presenters | `src/runtime/editor_session_internal.h`, `src/runtime/snapshot.cpp`, grid presentation sources | concurrent session changes appear only at snapshot revision boundaries; per-view isolation | FRAME-3, FRAME-4 |
| 3 | Rewire TUI presentation through the adapter | `apps/ssg_main.cpp`, terminal/render integration files | golden: terminal cells and hits | FRAME-3 |
| 4 | Mark the old presentation projection as a deprecated bridge owned outside semantic state | snapshot/protocol manifest, runtime and tests | semantic clients never read bridge fields | FRAME-1, FRAME-2 |
| 5 | Hand the bridge removal inventory to Plans 4 and 6 | `plan-4.md` surface inventory and Plan 1 wire manifest | inventory: every bridge consumer has an assigned migration/removal step | FRAME-2 |

## Rationale

Grid rendering is useful reusable presentation, but making it part of every
client's state contract reverses the intended dependency direction. The grid
projection introduced here is the same solved grid-tree value implemented by
Plan 4, not a legacy intermediate type.
