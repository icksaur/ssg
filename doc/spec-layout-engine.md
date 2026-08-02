# spec-layout-engine

Status: DRAFT (spec through V1 only — a behavior-preserving rearchitecture). V2
(rendering borders/separators/tags from the tree, converging the two manifests)
is deferred to a follow-up spec after discussion. Do NOT implement until reviewed.

## Goals

Replace the flat, hand-arithmetic shell layout (`computeShellLayout`,
`src/ShellState.cpp:389-641`) with a **composable box-tree data structure** and a
solver that computes every region's rectangle from the terminal dimensions:

    LayoutNode tree  ->  solveLayout  ->  SolvedBoxes

The observable outcome for V1 is **nothing** — the produced `ShellViewState`
(named rects, panes, `accessibilityNodes`) is byte-for-byte identical to today's
across every viewport size and chrome state, proven by golden tests. The *value*
is structural: after V1, adding or resizing a UI region is an **additive change to
a declared tree**, not a rewrite of imperative split arithmetic threaded through
one 250-line function.

Primary need (Carl): the **data structure**, kept deliberately minimal, so future
layout changes are additive. A single-location C++ authoring form (a builder / DSL
for the tree) is desirable but not required in V1.

The strategic goal this enables: easy iteration on non-text UI elements —
boxes/frames, separators, tags. The design does NOT add a bespoke type for those.
Each is **made from the existing pieces**: a node (geometry), an inset (reserved
space), and a single child; the renderer draws the reserved ring/region keyed on
the node's `id`. No "adornment" concept exists.

Non-goals (V1): no new UI element, no visible change, no consumer migration to
query-by-id (consumers keep reading the same `ShellViewState` fields), no
weighted/nested pane authoring beyond today's 50/50 binary splits, and no
border/separator/tag RENDERING (that is V2, built on inset + render-by-id).

## Design

### Two concerns, deliberately separated

- **Region geometry** — WHERE each box sits (rows×cols, position). This becomes a
  declarative nested box tree solved against the terminal `Rect`. The new part.
- **Content packing within a region** — the horizontal packing of variable-width
  items inside a solved box: status-field collapse-rank, footer-action
  right-alignment, tab widths + active-tab auto-scroll window, input-line
  reservation. These stay as **content packers** that fill a *given* box rect. They
  move verbatim (behavior-preserving) and are handed a solved rect instead of a
  hand-built one.

Consumers (Renderer, HitTester) already partly iterate the flat
`accessibilityNodes` manifest for chrome and reach into named fields for content
regions. V1 keeps both — the engine PRODUCES the identical `ShellViewState`, so no
consumer changes.

### The data structure (the deliverable)

Minimal by design. A node has an id, a size along its parent's axis, an axis for
its own children, an optional inset, and children.

```cpp
enum class Axis { Row, Column };   // Row: children share WIDTH (left→right);
                                    // Column: children share HEIGHT (top→bottom)

enum class SizeKind { Exact, Flex };
struct Size {
    SizeKind      kind  = SizeKind::Flex;
    std::uint16_t exact = 0;        // main-axis cells, when kind == Exact
};                                  // (equivalently: std::optional<uint16_t>,
                                    //  nullopt == Flex — same information)

struct Inset { int left = 0, right = 0, top = 0, bottom = 0; };  // cells reserved
                                    // inside the frame before children are placed

struct LayoutNode {
    std::string   id;               // "header","panel","tabbar","document",...
    std::optional<ShellNodeKind> kind;   // the a11y/field role, OR nullopt for a
                                    // STRUCTURAL container (root/body/content) that
                                    // exists only to group children and is NEVER
                                    // projected into accessibilityNodes.
    Size          size;             // this node's size within its PARENT's axis
    Axis          axis = Axis::Column;   // how THIS node arranges its own children
    Inset         inset;            // frame → child-area reservation (0 in V1 shell)
    std::vector<LayoutNode> children;
};
```

Structural nodes (`root`, `body`, `content`) carry `kind == nullopt`: they shape
geometry but do not correspond to any of today's `ShellNodeKind` values and MUST
NOT appear in the projected `accessibilityNodes` (emitting them would break the
byte-for-byte output). Only nodes with a real `kind`, plus the overlays and
content-packer items, are projected — and only in the pinned order (see below).

That is the whole vocabulary: Exact|Flex, an axis, an inset, children, and a
kind-or-structural marker. No min, no max, no weight, no `present`, no adornment.
Everything those did is expressed by HOW THE TREE IS BUILT (see "Policy lives in
tree construction").

Solved output — a flat list plus an id accessor. One `Rect` per box (its full
frame); the inset is consumed during solving and is recoverable from the child
rects, so it is not stored:

```cpp
struct SolvedBox {
    std::string   id;
    std::optional<ShellNodeKind> kind;   // nullopt for a structural container
    Rect          rect;             // the node's full solved rectangle
};

struct SolvedLayout {
    std::vector<SolvedBox> boxes;   // one per placed node, in emission order
    const SolvedBox* find(std::string_view id) const;   // named-field accessor
};

// Failure is a first-class result: when a container's Exact children cannot fit
// its available cells, the layout does not fit and the app shows a "too small"
// message. No clamping, no garbage layout.
std::optional<SolvedLayout> solveLayout(const LayoutNode& root, Rect bounds);
```

### The solver (minimal, pure mechanism)

A node's children are placed within its frame minus its `inset` (when inset is 0,
the whole frame). For a container laying children along `axis` within a main-axis
extent `M`:

1. `S = sum of the Exact children's sizes`.
2. If `S > M` → **failure** (the layout does not fit; return `nullopt`).
3. `remainder = M − S`, split **equally** among the Flex children; cells that do
   not divide evenly go to the **last** flex child (matches today's
   `rect.width − firstWidth` remainder rule). A container may legally have zero
   flex children (all Exact); then `remainder` is unused.
4. Cross-axis: each child fills the container's cross extent.
5. Recurse into each child within its placed rect.

That is the entire solver. Equal-split with remainder-to-last also reproduces the
pane 50/50 rule, should panes ever migrate onto it (V2).

### Policy lives in tree construction, not the solver

min/max/conditional-presence do not vanish; they move OUT of the solver (a pure
mechanism) INTO the function that builds the tree each frame — which is the same
place today's arithmetic already lives. Concretely:

- **Conditional chrome** (header/footer/panel/prompt appear conditionally): the
  builder simply does not add the node when it is absent. No `present` flag.
- **Panel min-width rule** (`min(panelTarget, viewport − editorMin)`, absent below
  `editorMin + panelMin`): the builder computes the exact panel width with the
  SAME expression as today (ShellState.cpp:502-509) and declares an
  `Exact(panelWidth)` node beside a `Flex` editor — or omits the panel node below the
  threshold. The solver never needs to know the rule.
- **Minimum viewport 20×4**: the shell checks this up front and returns the
  existing `ViewportTooSmall` error before solving (as today), so a Flex region can
  never collapse to a degenerate size in practice.

This keeps the solver trivially testable and puts every sizing DECISION in one
readable construction function, mechanism cleanly separated from policy.

### The declared tree (V1, reproducing today exactly)

```
Column root                         (header / body / footer stacked)
├─ header    Exact(headerHeight)              added iff !distractionFree
├─ body      Flex                   (Row: panel | content)
│  ├─ panel  Exact(panelWidth)                added iff requested & fits;
│  │                                          panelWidth computed by the builder.
│  │                                          Panel node/provider = full frame; the
│  │                                          scrollbar gutter is derived in the
│  │                                          projection (today's carve), not an inset.
│  └─ content Flex                 (Column: tabbar | document)
│     ├─ tabbar   Exact(tabBarHeight)
│     └─ document Flex             ← pane subtree rooted at this solved rect
└─ footer    Exact(footerHeight)              added iff !distractionFree
```

Chrome heights, gutter width, panel widths, minimums all come from `Style`
(data-driven, unchanged). Distraction-free mode = the builder adds only the
`document` node filling the viewport (no header/footer/tabbar/panel).

### Borders / separators / tags — from existing pieces (V2 renders them)

No new type. Each is a node + inset + render-by-id:

- **Separator** — an `Exact(1)` node between two siblings on the container's axis. Its
  `rect` (found by id) is a 1-cell line the renderer paints. Geometry from the
  tree; appearance is the renderer drawing a line glyph into that rect.
- **Box / frame** — a node with `inset = {1,1,1,1}` and a single child. Children sit
  inside the inset; the **border ring** is the parent's `rect` minus the child's
  `rect` (both found by id). The renderer, told to decorate that id, paints border
  glyphs in the ring. This is exactly "adornment made from insets + a single
  child," so no `Adornment` field is needed on the node.
- **Tag / badge** — an `Exact`-width leaf (typically produced by a content packer,
  like a status field); the renderer fills/brackets its `rect` by id.

In V1 every shell node uses `inset = 0` and no border is drawn, so V1 is visually
identical. Inset is honored by the solver and unit-tested with non-zero values so
the mechanism is proven, even though the V1 shell exercises it only at 0. (The
panel scrollbar gutter is NOT modelled as an inset in V1 — it is derived in the
projection exactly as today; see Projection — because a panel inset would shrink
the panel/provider a11y rects and break the byte-for-byte oracle.)

### Overlays (the 3 non-flow elements)

The prompt (over footer), palette (over document), and input line (over header)
are NOT flow boxes. They are an explicit, ordered overlay list applied AFTER the
flow solve, positioned from solved base rects:

- **prompt** — the bottom `reservedPromptRows` of the screen; additionally the
  `document` flex height is reduced by the prompt reservation measured from the
  screen bottom, reproducing today's "shrink the editor from the bottom, cover the
  footer" behavior (the pane top stays fixed — a pinned invariant). **Ordering: the
  pane solver must receive the POST-prompt document rect** (height reduced to
  `promptTop − document.y`) BEFORE `canLayout`/`layoutPanes` run, or a prompt could
  push a multi-pane split into the single-pane fallback at the wrong threshold.
  **Distraction-free: the prompt is NOT laid out when `distractionFree` is true**,
  even if `reservedPromptRows > 0` — matching today's code, which skips the whole
  chrome+prompt block and gives the viewport to the document. `reservedPromptRows`
  > 3 keeps the existing `InvalidPromptRows` error.
- **palette** — NOTE the seam: `computeShellLayout` does NOT know palette state
  today; the palette projection is added AFTER, in `src/runtime/snapshot.cpp`, from
  the solved `view.panes.front().content`/`scrollbar` rects. V1 does not move that
  boundary. So the palette is in the oracle only when the golden fixture captures
  the final `ShellViewState` at the snapshot layer (recommended — see
  Considerations), not `computeShellLayout` in isolation.
- **input line** — occupies the solved `header` rect to the right of the packed
  fields, after the field width has been reserved (unchanged content-packer rule).

Overlays are `{id, kind, rect}` regions inserted into the solved manifest **at the
exact position today's code emits them** (see the pinned order below), NOT merely
appended — appending would change `accessibilityNodes` bytes and hit precedence.

### accessibilityNodes emission order is FIXED, not tree order (pinned)

The manifest order is load-bearing: HitTester returns the FIRST node containing a
cell, so order encodes hit precedence (e.g. the input line must precede the header
fields it overlays). Today's order is hand-emitted and is NOT a naive flatten of
the tree. The projection MUST reproduce this exact sequence:

1. header (container), footer (container);
2. header fields (left→right), then the input-line query + ghost overlay;
3. footer actions (right→left), then footer fields;
4. panel, panel-provider, panel scrollbar;
5. tabbar (container), then tabs (first-visible → right);
6. prompt reservation overlay;
7. per pane: pane, pane scrollbar, and (when emptyState) the empty-state node.

The engine solves geometry in any order; the PROJECTION step emits nodes in
exactly this sequence. A projection-order golden test pins it independently of the
rect values.

### Panes (V1: reuse, rooted at the solved document box)

The pane binary-split tree (`PaneNode`, `layoutPanes`, `canLayout`) already IS a
recursive box layout with its own graceful fallback (a split that cannot fit
collapses to a single pane — NOT a hard failure). V1 keeps it verbatim, rooted at
the solved `document` rect instead of the hand-built `editor` rect. Folding
`PaneNode` into `LayoutNode` (splits as equal Flex children) is a clean V2
unification, deferred to keep V1's blast radius minimal and behavior identical.

### Projection back into ShellViewState (keeps consumers unchanged)

`computeShellLayout` becomes, in this exact order (preserving today's two
pre-layout error checks, ShellState.cpp:391-400):

1. **check minimum viewport** (< 20×4 → `ViewportTooSmall`, as today);
2. **check `reservedPromptRows <= 3`** (else `InvalidPromptRows`, as today);
3. build the declared tree from `Style` + `request`;
4. `solveLayout` (a container whose Exact children overflow → `nullopt` →
   `ViewportTooSmall`);
5. reduce the solved `document` height by the prompt reservation, then **check the
   prompt-induced failure**: if `reservedPromptRows >= document.height` (the prompt
   leaves no editor content row), return `ViewportTooSmall` — reproducing
   ShellState.cpp:594-599, which errors BEFORE pane layout. This check lives here,
   after the flow solve, because the prompt is an overlay the solver does not see;
6. apply overlays; root the pane solver at the post-prompt `document` rect;
7. **project** into today's `ShellViewState`:

- named `optional<Rect>` fields via `solved.find("header")->rect`,
  `find("panel")->rect`, etc. (absent when the builder omitted the node);
- `panes` from the pane subtree solved at the `document` rect;
- `accessibilityNodes` = the solved boxes THAT HAVE A REAL `kind` (structural
  containers with `kind == nullopt` are skipped) + overlays + the content packers'
  per-item nodes (fields, tabs, actions), inserted in the pinned order;
- `panelScrollbar` derived EXACTLY from the solved panel frame with today's
  condition and offset — present only when `panelWidth > gutterWidth &&
  panel.height > 1`, at `{panelWidth − gutterWidth, panel.y + 1, gutterWidth,
  panel.height − 1}` (ShellState.cpp:519-528); the panel node and provider rects
  are the FULL solved panel frame. V1 keeps shell insets at 0 and does NOT model
  the panel gutter as an inset (which would change the provider/a11y rects);
- `tabHits`, `palette` as today, from the corresponding solved rects.

One rect per region computed once (in the tree), then read by id — retiring the
"compute a named rect AND separately `addNode` it" duplication.

## Invariants

- **INV-layout-behavior-preserving (V1 gate):** for every case in the golden
  matrix, the produced layout output is byte-for-byte identical to the
  pre-refactor output. This is the primary oracle. The matrix must vary EVERY
  input that changes the output (see Acceptance), not just viewport geometry.
- **INV-layout-single-source (new):** region geometry is owned by the one declared
  `LayoutNode` tree + solver. No consumer, and no code outside the layout module,
  hand-computes a region rectangle. Content packers position ITEMS within a solved
  region; they do not compute region geometry.
- **INV-geometry-content-separation (new):** the box tree computes regions; content
  packers fill a given region rect; consumers read solved boxes by id/field.
- **INV-solve-fails-loudly (new):** when a container's Exact children exceed the
  available cells, `solveLayout` returns `nullopt` and the shell surfaces the
  existing too-small result — never a clamped, overlapping, or truncated layout.
- **INV-inset-honored (new):** children are placed within frame minus inset; the
  V1 shell uses inset 0 (so no visible change) but the solver honors non-zero inset
  and it is unit-tested, because inset is the V2 border mechanism.
- **Preserved from today (pinned by existing tests):** minimum viewport 20×4 →
  `ViewportTooSmall`, not a bad layout; prompt shrinks the editor from the BOTTOM
  and the pane top is identical with/without a prompt; no overlaps among
  header/footer/tabbar/panel/pane-content/scrollbars; header row 0, footer last
  row; input-line reservation deducted from fields BEFORE they pack; active tab
  always visible; scrollbar gutter reserved so content width is stable; all chrome
  dimensions come from `Style`, not literals.

## Considerations

- **Golden oracle captured from master FIRST**, or it proves nothing. Capture the
  FINAL `ShellViewState` at the snapshot layer (so the palette overlay, added in
  `snapshot.cpp`, is included) for the state matrix into a fixture on master, then
  refactor until the new engine reproduces it. A test written against the new code
  is a fake oracle.
- **Policy relocated to tree construction, not removed.** The panel width
  expression and the conditional-presence decisions move from inline layout
  arithmetic into the tree builder. This is the design's central trade: a pure,
  trivially-tested solver at the cost of the builder owning the sizing policy. The
  builder must stay the SINGLE place those decisions live (do not leak a second
  panel-width computation into a consumer) — INV-layout-single-source guards this.
- **Prompt/editor-height coupling is the subtlest rule** (today:
  `editor.height = promptTop − editor.y`, `promptTop = rows − reservedPromptRows`,
  covering the footer). The overlay pass must reproduce it exactly; include every
  `reservedPromptRows ∈ [0,3]` at several heights in the matrix.
- **`accessibilityNodes` ordering is load-bearing** (first-match hit precedence);
  the projection emits nodes in the pinned order, not merely the same set.
- **Content packers stay behavioral, not geometric.** `addFields`, tab packing,
  footer-action alignment, `visibleQueryTail` move unchanged; they receive a solved
  rect. Do NOT "improve" them in V1 — that would break the byte-for-byte oracle.
- **Conditional presence = omission.** A node the builder does not add is absent
  from the projection (its named field is `nullopt`), matching today's
  `optional<Rect>` semantics — not a present-but-zero rect.
- **Solver determinism.** Integer flex distribution must match today's
  remainder-to-last rule, or golden rects drift by a cell. Pin it with a solver
  unit test independent of the shell.

## Risks and Mitigations

- **Silent geometry drift** (a rect off by one). Mitigation: the golden fixture
  captured from master is the gate; a single differing cell fails the suite. Build
  the matrix broad.
- **Policy-in-builder becomes a dumping ground.** Mitigation: keep the builder a
  small, readable function; the solver stays pure; INV-layout-single-source forbids
  a second geometry computation elsewhere. If the builder grows unwieldy, that is a
  signal to add a solver capability in a FUTURE spec — not to scatter arithmetic.
- **Scope creep into V2** (borders/tags rendering, consumer migration, pane
  unification, weighted splits). Mitigation: V1 is explicitly the invisible
  refactor; the minimal data structure admits V2 additively; no V2 behavior ships.
- **Behavioral packers accidentally changed.** Mitigation: move them verbatim; diff
  to prove no logic change; the golden suite catches any drift.

## Acceptance (Definition of Done)

- Observable: none (invisible refactor). No visual signoff needed BECAUSE the
  golden oracle proves identical output; any golden diff is a V1 bug.
- Budgets: layout cost per frame no worse than today; no new per-frame allocation
  on the hot path beyond the solved-box vector the manifest already builds.
- Gates: `bash scripts/check.sh` green (all suites, 0 warnings), tree-sitter ON and
  OFF.
- Oracles:
  - **Golden projection** (primary): a captured-from-master fixture, reproduced
    exactly. Capture the FINAL `ShellViewState` at the snapshot layer (palette in
    scope). The matrix must vary: viewport size (small/large/odd, the 20×4 minimum,
    one below); panel on/off and the width thresholds (below); `reservedPromptRows`
    ∈ {0,1,2,3} at several heights including fallback-inducing ones; pane topology
    (single, H, V, nested); tab count 0/1/many with active/dirty/multibyte labels
    and the narrow-width auto-scroll window; focus (Editor/Panel/Prompt);
    empty-state on/off; distraction-free on/off × prompt rows > 0; input-line
    active with query + ghost (and a scrolling long query); header/footer fields +
    footer actions present and collapsing; panel-provider label; palette open vs
    closed; a non-default `Style`.
  - **Panel width boundary cases** (guard against one-cell drift), default
    dimensions (target 24 / min 12 / editorMin 20): columns 31 → no panel; 32 →
    panel 12 / editor 20; 43 → panel 23 / editor 20; 44 → panel 24 / editor 20;
    45 → panel 24 / editor 21.
  - **Error-path cases**: viewport below 20×4 → `ViewportTooSmall`;
    `reservedPromptRows > 3` → `InvalidPromptRows`; a height where the prompt
    reservation leaves no editor content row (`reservedPromptRows >=
    document.height`) → `ViewportTooSmall`. These reproduce the two pre-layout
    checks and the post-flow prompt check exactly.
  - **Solver unit test** (independent of the shell): Exact/Flex equal-split with
    even/odd remainder-to-last; a zero-flex container; **non-zero inset on all four
    edges** (children placed in frame − inset); and **failure** when Exact children
    exceed the extent (`solveLayout` returns `nullopt`).
  - **Projection-order test**: the `accessibilityNodes` sequence matches the pinned
    legacy emission order, independent of rect values.
  - **Existing test_ui_layout** invariants (non-overlap, minimum viewport, prompt
    pane-top stability, chrome-from-Style) continue to pass unchanged.

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Capture the golden fixture from CURRENT code across the FULL input matrix, snapshotting the FINAL `ShellViewState` at the snapshot layer; include the panel width thresholds, prompt rows 0–3 at fallback heights, multi-pane topologies, distraction-free×prompt | tests/test_ui_layout.cpp (+ golden data) | the fixture compiles and matches current output (incl. accessibilityNodes order) | INV-layout-behavior-preserving |
| 2 | Add `Axis`/`Size`/`Inset`/`LayoutNode`/`SolvedBox`/`SolvedLayout` + `solveLayout` (returns `optional`) with a standalone solver unit test | include/ssg/Layout.h (new), src/Layout.cpp (new), tests/test_layout_solver.cpp (new) | solver reference: exact/flex equal-split, remainder-to-last, non-zero inset, failure-on-overflow | INV-layout-single-source, INV-solve-fails-loudly, INV-inset-honored |
| 3 | Write the tree builder (Style + request → LayoutNode, with panel width + conditional presence as construction policy; structural containers carry `kind = nullopt`) + overlay pass; rewrite `computeShellLayout` as: check min viewport → check `reservedPromptRows <= 3` → build → solve (nullopt → too-small) → reduce document by prompt & check `reservedPromptRows >= document.height` → overlays → root panes at document → project (skip structural nodes; derive `panelScrollbar` by today's carve) into the unchanged `ShellViewState`; move content packers to fill solved rects verbatim | src/ShellState.cpp, include/ssg/ShellState.h | the golden projection + error-path fixtures from step 1 reproduce byte-for-byte | INV-layout-behavior-preserving, INV-geometry-content-separation |
| 4 | Delete the retired hand-arithmetic + the compute-then-addNode duplication; confirm consumers (Renderer, HitTester) unchanged | src/ShellState.cpp | full gate green; test_ui_layout unchanged and passing | INV-layout-single-source |

## Beyond V1 (a separate V2 spec)

V1 is the geometry substrate. The stated goal — easy iteration on boxes,
separators, tags — is V2, additive on V1's pieces (no new node type):

1. **Render borders/separators/tags by id** from the solved tree: a decorator step
   that, given a box id, paints a line into a separator's rect, or border glyphs
   into the ring between a box's rect and its child's rect, or a fill/bracket into
   a tag's rect. Insets already reserve the space; only rendering is new.
2. **Migrate the renderer/hit-tester to walk the solved tree** where a node's
   appearance is fully described by its geometry + kind, retiring bespoke per-kind
   paint functions. The renderer already walks `accessibilityNodes` keyed on
   `node.kind` (Renderer.cpp:362) — V2 extends that.
3. **Converge the two manifests** (named rects + the flat node list) into one
   visual-element tree consumed by solver, renderer, and hit-tester, with the named
   `ShellViewState` fields already thin `find(id)` accessors from V1.

This is why V1 keeps `inset` (borders need it) while dropping every other extra
concept: borders/separators/tags fall out of node + inset + child + render-by-id,
so no `Adornment` type is warranted.

## Rationale (skippable)

The compositional idea already lives in the codebase for editor splits
(`PaneNode`/`layoutPanes` is a recursive box layout) and a flat node manifest
already exists (`accessibilityNodes`); what is missing is applying both to the
CHROME, today a flat sequence of hand-computed `Rect{...}` assignments with a
compute-then-re-register duplication per element. Introducing the tree as the
single source of region geometry — with a deliberately tiny vocabulary
(Exact|Flex + inset), a pure solver that fails loudly rather than clamping, and all
sizing policy in one builder — collapses the per-element blast radius while V1
ships zero visible change under a golden gate. Borders/separators/tags are left to
V2 precisely because they need no new data: node + inset + child + render-by-id is
enough, so the node model stays minimal and every richer element is an addition,
not a re-cut of the type.
