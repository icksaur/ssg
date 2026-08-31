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
well-known surface backing. Prompt controls, notices, external modification,
tabs, panels, document, and status areas must all travel through the same
solver. Preserve platform-independent layout; terminal capability conversion
stays at rendering edges.

## Risks and Mitigations

- Reproducing the shell algorithm inside the solver would retain duplication:
  implement generic node constraints and delete feature branches as each moves.
- Hit geometry may drift from rendering: derive both from one immutable solved
  result.
- Transitional dual paths can diverge: permit them only within a step, with a
  named removal test before commit.

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
| `GridPresenter::project` and `EditorSession::projectForBridgedPresenterDeprecated` | semantic sections plus viewport dimensions and presenter state | Step 2 solves a `GridFrame` from a moved semantic snapshot and grid request; the presenter no longer calls an `EditorSession` projection bridge | `GridPresenter` no longer references `projectForBridgedPresenterDeprecated`; the deprecated `present` wrapper is its sole remaining caller until Plan 6 deletes both |
| `Renderer` | shell rectangles, viewport rows, style, prompt projection, and tree windows | Step 3 reads node rectangles and typed surface backing from one solved grid tree | renderer has no `ShellViewState`, `PromptViewState`, or `TreeWindow` branch |
| `HitTester` | shell rectangles, viewport hit targets, prompt controls, and tree windows | Step 3 derives hits from the same solved nodes consumed by `Renderer` | one solved-node parity oracle covers both render and hit results |
| Header, footer/status, tabs, panel/tree, document, prompt, picker, notice, external-modification, and search-result projection | feature-specific members of `ShellViewState`, `ViewportViewState`, `PromptViewState`, and `TreeWindow` | Step 4 migrates each named surface through generic placement and typed surface backing | each surface retains cell/hit parity and deletes its feature geometry branch |
| `buildShellTree`, shell accessibility/hit sidecars, and obsolete shell node kinds | independently assembled whole-screen geometry | Step 5 deletes them after every Step 4 row is complete | source inventory finds no parallel whole-screen layout path |
| Grid tests and `tests/legacy_grid_frame.h` | manually assembled legacy presentation values | Steps 1–4 move focused solver/render/hit tests to direct `GridFrame` construction; terminal parity remains the end-to-end oracle | Plan 6 removal does not require replacing a test-only presentation architecture |

`PresentationSnapshot::selectionNav` remains presenter-owned navigation state,
not a UI-tree input. Step 4's document migration consumes its projected result;
Plan 6 removes only its frozen legacy wire field. `PresentationSnapshot::style`
remains the grid medium's glyph configuration while semantic colors and node
roles originate in the UI tree.

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Define solved grid-tree values and generic constraint cases | `include/ssg/UiTree.h`, layout headers/sources, focused layout tests | hand cases: axis/size/inset/gap/scroll | UITREE-1, UITREE-2 |
| 2 | Solve validated UI frames into grid nodes | `src/WholeScreenAssembly.cpp`, layout/chrome lowering sources | property: every present renderable node has one solved result | UITREE-1, UITREE-4 |
| 3 | Render and hit-test from the solved tree | `src/Renderer.cpp`, `src/HitTester.cpp`, related headers/tests | parity: one solved node yields matching cells/hits | UITREE-2, UITREE-3 |
| 4 | Migrate every Plan 1 surface-inventory entry through generic placement | prompt, picker, notice, external modification, tab bar, tree providers/panel, document, search results, status/header/footer presentation sources | each inventory entry has cell/hit parity and no feature geometry branch | UITREE-1, UITREE-3 |
| 5 | After every Step 4 inventory entry passes, delete `buildShellTree`, feature geometry sidecars, and obsolete shell node kinds | `src/ShellState.cpp`, `include/ssg/ShellState.h`, callers/tests | inventory is complete and no parallel whole-screen layout path remains | UITREE-1 |

## Rationale

The UI tree already expresses the intended product composition. Executing it
in both clients removes a whole parallel architecture rather than wrapping it.
This plan begins only after Plan 3 is complete; its solved grid tree implements
the grid-projection type established there.
