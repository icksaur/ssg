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

Responsive siblings use one additional `SizeKind::Responsive` shape with
minimum, preferred, growth, and optional fields. Only two factory-produced forms
are valid:

- `Size::minimumFlex(minimum)` produces required state with preferred equal to
  minimum and the same positive unit growth used by legacy Flex.
- `Size::optionalPreferred(preferred, minimum)` produces optional state with
  zero growth and requires a positive preferred value no smaller than minimum.

Responsive state with optional growth, preferred below minimum, non-positive
required growth, or any other field combination not produced by those factories
is rejected at construction and decode. A container with a Responsive direct
child may not have an Auto direct child because responsive feasibility must be
computable from published constraints without client-specific intrinsic child
measurement; schema validation rejects that combination without restricting
Auto descendants inside nested children. Existing Exact, Flex, and Auto
meanings and encodings remain unchanged. The body authors panel as optional preferred between
`StyleDimensions::panelMinimumWidth` and
`StyleDimensions::panelTargetWidth`, and content as required flexible from
`StyleDimensions::editorMinimumWidth`. The generic solver first removes optional
responsive children, last declared first, until every surviving child's floor
and every gap between survivors fit. Exact contributes its extent, legacy Flex
contributes no floor, Auto contributes its resolved intrinsic extent, and
Responsive contributes its minimum. The solver then allocates each survivor its
floor, distributes available space toward optional preferred targets
proportionally to each target's `preferred - minimum` range without exceeding
the target, and distributes the remainder among legacy Flex and required
minimum-flex children by growth. Integer remainder goes to the final eligible
child, preserving legacy Flex behavior. A dropped child and its subtree have no
solved nodes; if required floors cannot fit after every optional child is
dropped, solving fails.

The browser interprets the same responsive shape without surface identities.
On schema, presence, or container-size changes, generic retained-tree code
chooses the optional children that fit using only published constraints and the
measured container main-axis content extent, never child layout measurements.
Dropped children receive `display: none`. Surviving children use the existing
medium conversion for minimum and preferred extents: minimum becomes the
main-axis CSS minimum, preferred becomes flex basis, growth becomes flex grow,
and optional shrink is weighted by its `preferred - minimum` range so CSS
reaches the same floors. Existing size-kind CSS mappings remain unchanged. This
work is independent of document content and does not run on caret, selection,
or text-input updates.

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
- UITREE-5: Until a surface atomically switches to solved-tree consumption, its
  dormant solved rectangle equals the legacy shell rectangle still consumed for
  that surface. State at each transitional parity oracle; delete this temporary
  invariant with the last legacy placement reader.
- UITREE-6: Responsive removal and sizing derive only from published `Size`
  constraints and available parent extent; neither grid nor browser code names
  the panel or another product surface when resolving them. State at `Size` and
  each responsive interpreter entry point.

## Considerations

Support axis, size, inset, gap, scroll ownership, semantic style, presence, and
well-known surface backing. Prompt, picker, notice, external modification, tab
bar, tree providers/panel, document, search results, status, header, and footer
must all travel through the same solver. Preserve platform-independent layout;
terminal capability conversion stays at rendering edges.

Notice and external-modification surfaces are editor-owned chrome: the
authoritative tree places them after the tab bar and before the replaceable
document-editor/picker branches, inside the content column. They therefore
remain visible across a picker swap, reduce document or results rows without
moving the panel, and never span it. This matches the established terminal
placement and gives native clients the same responsive composition without a
client-specific exception.

Responsive allocation is an ordered operation over a container's main axis:

1. Resolve Auto children to their intrinsic main-axis extents and collect all
   children still admitted by semantic presence.
2. Compute feasibility from the parent's post-inset main-axis extent, every
   surviving child's floor, and gaps only between surviving adjacent children.
3. While infeasible, remove the last declared optional Responsive child and
   recompute floors and gaps. Both gaps adjacent to a removed child disappear,
   and one declared gap separates the survivors that become adjacent. Fail when
   infeasible and no optional child remains.
4. Allocate each survivor its floor. Give the available amount up to the sum of
   optional preferred ranges proportionally to those ranges, with integer
   remainder assigned to the final still-under-target child. If every optional
   child reaches its target, no preferred remainder remains and surplus passes
   to the growth phase.
5. Divide remaining space by growth among legacy Flex and required
   minimum-flex children; assign integer remainder to the final growing child.
   Leave surplus unused when no child grows.

Cross-axis stretching is unchanged. Existing Exact children therefore retain
their fixed extent, existing Flex children retain equal growth, and Auto
children retain intrinsic sizing.

`SizeKind::Responsive` has a distinct enum value within
`kProtocolWireVersion`; message framing does not change. Exact, Flex, and Auto
continue to encode exactly the existing `kind` and `extent` fields.
Responsive encodes `kind`, `extent` as preferred, `minimum`, `growth`, and
`optional` as separate fields, in that object-field order. Exact, Flex, and Auto
omit the final three fields rather than encoding ignored defaults. The
Responsive decoder requires every field and accepts only the two factory forms
above. Any C++ or browser decoder that does not
understand Responsive rejects the schema entirely rather than interpreting it
as Flex or Auto; the web interpreter likewise rejects every unknown size kind.

Panel interior projection follows the existing atomic-surface pattern.
`SolvedPanelSurface` derives the provider label row, visible tree rows, selected
state, and scrollbar gutter from the solved panel node, semantic
`TreeViewState`, presenter-owned first-visible state, and style dimensions.
`GridPresenter` recomputes the tree window against the solved panel height;
legacy `TreeWindow` values and shell panel rectangles are not geometry inputs.
When responsive solving drops the panel, the frame carries no panel window,
panel scrolling is unavailable, and presenter-owned `treeFirstVisible` is
preserved. When the panel reappears, projection clamps that retained offset to
the new solved height and performs the existing selection reveal. Renderer and
HitTester consume the same solved surface. The provider label is derived from
the active semantic `TreeProviderKind` through the existing authoritative panel
provider mapping, not copied from `ShellLayoutRequest`.

Document grid geometry follows the same pattern. `SolvedDocumentSurface`
derives the line-number band, document content, and scrollbar gutter from the
solved `document.viewport` node, the effective line-number setting, the
published logical-line inventory, and grid style dimensions. Its content
excludes both gutters. Line numbers are omitted when reserving them would leave
less than the editor minimum width, matching the legacy shell rule. Picker
presence removes the editor branch, so absence of the solved document viewport
is the only condition for absence of the document surface. Renderer, hit
testing, scrollbar interaction, caret placement, and presenter navigation all
consume this surface. While the deprecated bridge remains, a parity oracle
compares its pane geometry with the solved surface; separate corruption oracles
prove migrated consumers ignore every legacy pane rectangle.
Presenter-owned split topology subdivides the solved document viewport through
one shared pane-frame solver. `SolvedDocumentSurface` carries those pane
interiors and the active pane identity; visual navigation uses the active
interior while the existing single rendered pane uses the first. Split topology
does not become UI-schema or wire geometry.

The schema move does not reposition the established shell layout: in the same
commit, a parity oracle compares both notice and external-modification solved
rectangles with the legacy rectangles the shell already projects. Notice then
switches atomically to its solved rectangle; external modification retains its
equal legacy reader until its immediately following surface migration.

The frozen legacy delta remains decodable until Plan 6 removes its compatibility
wire path. Its preceding canonical arrangement is root `[header, notice,
external modification, body, footer prompt, footer]`, body `[panel, content]`,
content `[editor, find-results viewport]`, and editor `[tab bar, document
viewport]`. The current arrangement is root `[header, body, footer prompt,
footer]`, body `[panel, content]`, content `[tab bar, notice, external
modification, editor, find-results viewport]`, and editor `[document viewport]`.
UI-schema decode normalizes only a schema whose node-id order and container
relationships exactly match that preceding arrangement, preserving node
payloads while moving the named nodes, before current well-known-area
validation. The frozen preceding schema's exact default panel and flex content
sizes are part of that exact match and normalize to the corresponding
`Size::optionalPreferred` and `Size::minimumFlex` values from default
`StyleDimensions`; no other legacy size tuple is guessed. Every other
structural or size mismatch remains rejected without normalization, and newly
encoded schemas always use the current arrangement.

## Risks and Mitigations

- Reproducing the shell algorithm inside the solver would retain duplication:
  implement generic node constraints and delete feature branches as each moves.
- Hit geometry may drift from rendering: derive both from one immutable solved
  result.
- Transitional dual paths can diverge: a surface migration atomically switches
  render and hit consumers to solved-tree placement and deletes their old
  placement reads in the same commit. No committed frame has two authoritative
  placement sources for one node.
- Tightening the canonical topology can strand the frozen legacy delta: keep one
  exact decode-only normalization with a checked-in legacy-fixture oracle, and
  delete it with the compatibility wire path in Plan 6.
- Widening that normalization could silently admit obsolete topology beyond its
  compatibility window: require the complete preceding arrangement, not a
  partial or approximate structural match.
- A responsive kind could silently degrade in an older interpreter: use a
  distinct kind, reject unknown kinds at decode/interpretation, and preserve
  legacy size encodings exactly.
- Responsive checks could become a cursor-path DOM cost: run generic optional
  child selection only when schema, presence, or measured container extent
  changes, leaving retained content updates untouched.
- Panel scrolling could remain coupled to the legacy panel height: derive the
  presenter-owned window from the solved panel surface and prove corrupted
  legacy panel geometry cannot affect cells or hits.

## Acceptance (Definition of Done)

- Observable: terminal layout and interactions remain behaviorally equivalent;
  the panel grows, shrinks, and disappears at the same semantic thresholds in
  grid and browser clients; UI-tree changes affect both clients without
  surface-specific layout edits.
- Budgets: solving scales with UI-node count and visible surface projection, not
  document size; browser responsive resolution does not run on caret, selection,
  or text-input updates.
- Gates: `scripts/check.sh` and `scripts/check.sh push`.
- Oracles: independent responsive-allocation hand cases covering wide,
  between-floor-and-preferred, below-floor removal, multiple optional children,
  gaps, and unsatisfied required floors; legacy Exact/Flex/Auto byte fixtures;
  responsive wire round trip and malformed-combination rejection; pure browser
  responsive-selection cases using the same inputs but an independently stated
  expected result; panel cell/hit parity with deliberately corrupted legacy
  panel geometry and tree-window metrics; current terminal cell/hit parity;
  generated-schema constraint properties; library and client unsupported-widget
  refusal; each migrated feature deletes its shell branch; while any legacy
  surface reader remains, its dormant solved rectangle equals that reader's
  rectangle.

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
| 4 | Add the generic responsive size shape, grid allocation, strict wire support, and identity-free browser interpretation before migrating a surface that depends on it | `include/ssg/LayoutConstraints.h`, `src/Layout.cpp`, `src/UiTreeProtocol.cpp`, `src/WholeScreenAssembly.cpp`, `apps/web/reconcile.mjs`, `apps/web/client.mjs`, focused C++/web/protocol tests | independent allocation hand cases; old size fixture bytes unchanged; responsive round trip and malformed rejection; browser optional-child selection matches stated hand results and is not invoked by retained content updates | UITREE-1, UITREE-4, UITREE-6 |
| 5 | Migrate prompt, picker, notice, external modification, tab bar, tree providers/panel, document, search results, status, and remaining header/footer backing through generic placement, one atomic surface commit at a time; normalize only a decoded UI schema whose complete node-id order and container structure equal the preceding canonical arrangement | surface presentation sources, `src/UiTreeProtocol.cpp`, protocol and focused render/hit tests | each named surface has cell/hit parity and its commit deletes that surface's feature geometry reads; panel width/removal matches responsive hand cases and ignores corrupted legacy geometry; the frozen legacy delta still decodes and replays to current canonical semantic state while every other malformed topology remains rejected; when Plan 6 deletes the shim, its compatibility-fixture inventory removes the frozen fixture or replaces it with a current semantic fixture | UITREE-1, UITREE-3, UITREE-6 |
| 6 | After every Step 5 inventory entry passes, construct frames directly from semantic state and the grid request; then delete `buildShellTree`, feature geometry sidecars, obsolete shell node kinds, and the presenter's bridge call while retaining the wrapper-only bridge for Plan 6 | `src/GridPresenter.cpp`, `src/ShellState.cpp`, `include/ssg/ShellState.h`, callers/tests | inventory is complete, no parallel whole-screen layout path remains, and `GridPresenter` no longer references `projectForBridgedPresenterDeprecated` | UITREE-1 |

## Rationale

The UI tree already expresses the intended product composition. Executing it
in both clients removes a whole parallel architecture rather than wrapping it.
Keeping responsive behavior as a shell or panel special case would preserve the
parallel policy path. A distinct responsive size shape is preferred over
changing Flex or Auto because legacy values retain their meanings and bytes,
older consumers reject the new requirement rather than rendering a plausible
but wrong layout, and the same small vocabulary serves future optional
sidebars. Reverse declaration order is the drop tie-breaker so schema order is
the only additional fact and no separate priority inventory is introduced.
Plan 3 is complete. Its grid-projection type is the ownership seam this plan's
solved grid tree implements.
