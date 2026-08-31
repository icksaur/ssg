# plan-4-execute-the-ui-tree-for-grid-presentation

## Goals

Use the authoritative UI tree as the only source of whole-screen placement and
visibility for both web and terminal presentation. Adding or moving a feature
must not require feature-specific terminal layout code.

## Design

Build a grid solver/interpreter over validated `UiSchema`, resolved node state,
presence, and typed surface backing. It produces solved rectangles keyed by
node identity. Surface renderers and hit derivation consume those solved nodes.
Delete the independently assembled shell tree and feature-specific layout
sidecars.

## Invariants

- UITREE-1: Whole-screen node placement, ordering, visibility, and focusability
  originate from one validated UI tree. State at the UI frame/grid solver API.
- UITREE-2: Grid cells and rectangles remain presentation outputs, never editor
  state or protocol inputs. State at the grid projection type.
- UITREE-3: Render and hit testing consume the same solved tree and cannot
  disagree about node placement. State at solved grid tree.
- UITREE-4: Unknown required widgets fail visibly rather than disappearing.
  Library profile validation rejects them before publication, and client
  `firstUnsupportedPrimitive`/`firstMalformedNodeStyle` checks provide a visible
  refusal if an incompatible frame still arrives. State at both boundaries.

## Considerations

Support axis, size, inset, gap, scroll ownership, semantic style, presence, and
well-known surface backing. Prompt, picker, notice, external modification, tab
bar, tree providers/panel, document, search results, status, header, and footer
must all travel through the same solver. Preserve platform-independent layout;
terminal capability conversion stays at rendering edges.

## Risks and Mitigations

- Reproducing the shell algorithm inside the solver would retain duplication:
  implement generic node constraints and delete feature branches as each moves.
- Hit geometry may drift from rendering: derive both from one immutable solved
  result.
- Transitional dual paths can diverge: a surface migration atomically switches
  render and hit consumers to solved-tree placement and deletes their old
  placement reads in the same commit. No committed frame has two authoritative
  placement sources for one node.

## Acceptance (Definition of Done)

- Observable: terminal layout and interactions remain behaviorally equivalent;
  UI-tree changes affect both clients without TUI layout edits.
- Budgets: solving scales with UI-node count and visible surface projection, not
  document size.
- Gates: `scripts/check.sh` and `scripts/check.sh push`.
- Oracles: generic constraint hand cases; current terminal cell/hit parity;
  generated-schema constraint properties; library and client unsupported-widget
  refusal; each migrated feature deletes its shell branch.

## Plan 3 bridge handoff

Plan 3 leaves one deprecated grid-projection bridge. The table below assigns
every presentation consumer that must change before the bridge can be removed.
Plan 6 owns the compatibility API and wire deletions after these migrations
finish.

| Current consumer | Presentation input | Plan 4 destination | Removal oracle |
|---|---|---|---|
| `GridPresenter::project` and `EditorSession::projectForBridgedPresenterDeprecated` | semantic sections plus viewport dimensions and presenter state | Step 3 makes `GridFrame` own one solved tree; Step 4 moves each surface's projection inputs behind typed backing; Step 5 projects directly from semantic state and the grid request | `GridPresenter` no longer references `projectForBridgedPresenterDeprecated`; the deprecated `present` wrapper is its sole remaining caller until Plan 6 deletes both |
| `Renderer` | shell rectangles, viewport rows, style, prompt projection, and tree windows | Step 3 atomically migrates header/footer placement while establishing the frame's solved-tree path; each Step 4 surface atomically replaces its remaining feature placement reads with solved-tree placement and typed backing | one solved-node parity oracle covers both rendering and hits; each surface commit deletes its named legacy placement reads, and removal of the last read is a Step 5 precondition |
| `HitTester` | shell rectangles, viewport hit targets, prompt controls, and tree windows | Step 3 atomically migrates header/footer placement to the same solved tree as `Renderer`; each Step 4 surface atomically replaces its remaining feature placement reads with solved-tree placement and typed backing | one solved-node parity oracle covers both rendering and hits; each surface commit deletes its named independent geometry reads, and removal of the last read is a Step 5 precondition |
| Header, footer/status, tabs, panel/tree, document, prompt, picker, notice, external-modification, and search-result projection | feature-specific members of `ShellViewState`, `ViewportViewState`, `PromptViewState`, and `TreeWindow` | Step 3 migrates header/footer placement; Step 4 migrates their remaining backing with status and migrates every other named surface through generic placement and typed surface backing | each surface retains cell/hit parity and deletes its feature geometry branch |
| `buildShellTree`, shell accessibility/hit sidecars, obsolete shell node kinds, and the presenter's deprecated projection call | independently assembled whole-screen geometry | After every Step 4 row is complete, Step 5 first constructs frames directly from semantic state and the grid request, then deletes the obsolete layout and sidecars | source inventory finds no parallel whole-screen layout path; `GridPresenter::project` no longer calls `projectForBridgedPresenterDeprecated`, which remains reachable only from `EditorSession::present` until Plan 6 |
| Grid tests and `tests/legacy_grid_frame.h` | manually assembled legacy presentation values | Steps 1–4 move focused solver/render/hit tests to direct `GridFrame` construction; terminal parity remains the end-to-end oracle | Plan 6 removal does not require replacing a test-only presentation architecture |

`PresentationSnapshot::selectionNav` remains presenter-owned navigation state,
not a UI-tree input. Step 4's document migration consumes its projected result;
Plan 6 removes only its frozen legacy wire field when
`kSemanticUiWireVersion` is activated. `PresentationSnapshot::style` remains
the grid medium's glyph configuration while semantic colors and node roles
originate in the UI tree.

Step 4's complete surface inventory is prompt, picker, notice, external
modification, tab bar, tree providers/panel, document, search results, status,
header, and footer. Header/footer placement begins in Step 3; their remaining
typed backing migrates with the status surface in Step 4.

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Define solved grid-tree values and generic constraint cases | `include/ssg/UiTree.h`, layout headers/sources, focused layout tests | hand cases: axis/size/inset/gap/scroll | UITREE-1, UITREE-2 |
| 2 | Solve validated UI frames into grid nodes | `src/WholeScreenAssembly.cpp`, layout/chrome lowering sources | property: every present renderable node has one solved result | UITREE-1, UITREE-4 |
| 3 | Thread Step 2's existing solver output into one tree owned by each `GridFrame` and atomically move header/footer render-hit placement to it | `include/ssg/GridPresenter.h`, `src/GridPresenter.cpp`, `src/Renderer.cpp`, `src/HitTester.cpp`, related tests | parity: one solved node yields matching header/footer cells and hits; no header/footer consumer reads legacy placement | UITREE-2, UITREE-3 |
| 4 | Migrate prompt, picker, notice, external modification, tab bar, tree providers/panel, document, search results, status, and remaining header/footer backing through generic placement, one atomic surface commit at a time | surface presentation sources plus focused render/hit tests | each named surface has cell/hit parity and its commit deletes that surface's feature geometry reads | UITREE-1, UITREE-3 |
| 5 | After every Step 4 inventory entry passes, construct frames directly from semantic state and the grid request; then delete `buildShellTree`, feature geometry sidecars, obsolete shell node kinds, and the presenter's bridge call while retaining the wrapper-only bridge for Plan 6 | `src/GridPresenter.cpp`, `src/ShellState.cpp`, `include/ssg/ShellState.h`, callers/tests | inventory is complete, no parallel whole-screen layout path remains, and `GridPresenter` no longer references `projectForBridgedPresenterDeprecated` | UITREE-1 |

## Rationale

The UI tree already expresses the intended product composition. Executing it
in both clients removes a whole parallel architecture rather than wrapping it.
Plan 3 is complete. Its grid-projection type is the ownership seam this plan's
solved grid tree implements.
