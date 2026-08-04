# spec-widget-chrome

Status: draft (spec only; not yet implemented)

## Goals

Re-architect SSG's header, footer, and prompt from ad-hoc procedural placement
into a single composition of **widgets**. A widget is a self-contained UI element
that (a) is positioned by RELATIVE layout inside its container (never absolute
x/y arithmetic), (b) owns the configurable GLYPHS it draws, and (c) tags every
cell it emits with a SEMANTIC COLOR ROLE. After this change:

- Header and footer are widget containers whose children (path/branch fields,
  status actions, help hint, picker input line) are widgets laid out relative to
  the header/footer rect.
- Checkboxes/toggles are a reusable `Checkbox` widget rather than glyphs the
  renderer stitches in at paint time.
- The prompt is a widget container; its text field is a reusable `TextInput`
  widget that can later be instantiated elsewhere.
- The design admits a future in which `init.lua` composes chrome from these
  built-in widget primitives, WITHOUT that exposure being built now.

Non-goal (explicit): exposing widget composition to `init.lua`; adding new
`SemanticRole`s or new `PromptKind`s; changing the on-screen result (Phase 1-2
are behavior-preserving — the existing goldens are the oracle).

## Design

### The widget as the single source of three projections

Today three concerns are split across two files and re-decided per element:
geometry (procedural x arithmetic in `ShellState.cpp` `addFields` and the inline
header/footer code, and in `PromptSurface.cpp` `computePromptLayout`), color role
(chosen by the caller of `addNode`, or by the renderer), and glyphs (stitched in
by `Renderer.cpp` `paintPrompt`/`paintShellLeaves` from `Style`). A **Widget**
unifies them: one tree is the single source for all three projections —

- geometry: the widget participates in relative box layout and receives a solved
  `Rect`;
- render: given its `Rect` + the `Style` + the caret/value data, the widget emits
  **paint spans** — positioned text runs each tagged with a `SemanticRole`, plus
  the configurable glyphs it chose (checkbox mark, label separator, sigil,
  truncation ellipsis);
- accessibility: the widget emits its `AccessibilityNode`(s) (`id`, `kind`,
  `rect`, `commandId`) for hit-testing.

The renderer stops choosing roles and glyphs for chrome; it becomes a blitter of
widget-emitted spans. `addNode`'s per-call role argument and the glyph stitching
in `paintPrompt` move INTO the widgets.

### Widget primitives (a fixed, small set)

Widgets are a closed set of C++ primitives (a tagged struct + free
`measure`/`layout`/`paint`/`project` functions switching on kind — matching the
codebase's pure-function, no-virtual style; no per-widget class hierarchy). The
set covers all current chrome:

- `Container` — arranges children on an `Axis` (Row/Column) with an alignment
  (Start/End) and per-child collapse policy (see fit pass). Header, footer, and
  the prompt row(s) are Containers.
- `Label` — static text in a role (panel provider label, hint, prompt input
  label).
- `Field` — a `Label`-like value with an id + optional command (status fields:
  path, branch, status, follow); collapsible by rank.
- `Checkbox` — a boolean with checked/unchecked glyphs from `Style.toggle` and a
  caption; the find toggles (case/word/regex).
- `TextInput` — an editable/one-line text region with a sigil/label prefix, a
  scrolling value tail, and a caret cell (the picker input line and the prompt
  inputs). This is the reusable widget the user wants extractable.
- `Spacer` — flexible gap (the reserved input-line slot; the packing gap between
  header fields and the input line).

Rationale for a FIXED set with data-composition: `init.lua` (eventually) COMPOSES
trees of these primitives; it does not define new primitive kinds in C++. So the
enum is closed and the extensibility is in the tree shape and the data, which is
what a future Lua binding would hand in.

### Relative layout: reuse the box solver + a fit pass

The existing box-tree solver (`include/ssg/Layout.h`, `solveLayout`) already does
relative Row/Column/Flex/Exact/Inset and already lays out the top-level regions
(header/footer/body/panel in `buildShellTree`). The widget layer REUSES it as the
geometry mechanism (chosen over a new layout engine: it is pure, tested, and
already relative). The gap the solver does not cover is the current procedural
behavior that fields/actions COLLAPSE when the row is crowded and pack from the
RIGHT — the solver instead fails loudly when Exact children do not fit. So the
widget layer adds a **fit pass** ahead of the solver, per Container:

- each child reports a desired size (its content width) and whether it is
  collapsible plus a collapse rank (the current `StatusField::collapseRank`);
- the fit pass reproduces today's `addFields` rule EXACTLY, which is NOT
  "drop-until-fits": it is a **stable stable-sort by collapse rank, then a single
  forward scan that appends each field while it fits and STOPS at the first field
  that does not fit** (later, higher-rank fields are not reconsidered even if a
  smaller one would have fit). This specific rule must be preserved verbatim — a
  naive "drop the lowest-rank until the set fits" picks a DIFFERENT retained
  subset for some width/value combinations and would silently drift. The footer
  hint "yields before status actions" is the same rule with the hint as the
  lowest-priority participant;
- alignment Start packs left (header fields), End packs right (footer actions and
  hint) — replacing the reverse-iteration right-edge arithmetic;
- the retained, aligned children become `Exact` solver nodes; the solver assigns
  their rects. Everything relative; no `view.footer->right()` arithmetic in the
  widget code.

The solver's fail-loudly contract is preserved: the fit pass guarantees the set
fits BEFORE the solver runs, so a `nullopt` from the solver remains a real bug,
not a crowded row.

### Prompt as a widget container

`computePromptLayout` is replaced by building a prompt Container of `TextInput`
(inputs), `Checkbox` (toggles), and `Label`/`Field` (match count) widgets, laid
out by the same fit-pass + solver in the reserved prompt rows. `PromptViewState`
/ `PromptControlView` remain the wire projection the client renders (so the
protocol is unchanged), but they are now PRODUCED BY the widget tree rather than
hand-rolled. The find toggles' checkbox glyphs come from the `Checkbox` widget,
not `Renderer.cpp` `paintPrompt`.

### What crosses the wire

Phase 1-2 keep the existing snapshot projections (`AccessibilityNode` for chrome,
`PromptViewState.controls` for the prompt) as the client-facing contract, so the
protocol and the client renderer change minimally and the existing goldens stay
valid. The widget tree lives SERVER-SIDE in `computeShellLayout` /
`computePromptLayout`; its `paint`/`project` outputs populate the same snapshot
fields as today. (A later, separate spec may replace the per-node role/glyph
carriage with an explicit paint-span list; this spec deliberately does not, to
keep the refactor behavior-preserving and reviewable.)

### Ownership contract: one node per role-homogeneous span (resolves the split-role case)

`AccessibilityNode` carries a SINGLE `SemanticRole` and a single content string,
and that stays true. A widget whose output spans MORE THAN ONE role therefore
`project`s ONE node per role-homogeneous span — which is exactly what the code
does today: the picker input line already emits a `input_line.query` node
(`SemanticRole::Prompt`) AND a separate `input_line.ghost` node
(`SemanticRole::LineNumber`). So `TextInput.project` returns a small list of
nodes (sigil+value+caret span in `Prompt`; ghost tail in `LineNumber`), not one
node with mixed roles. The per-phase ownership rule is thus unambiguous and needs
no wire change:

- Geometry ownership (all phases): the widget's `layout` computes the sub-rects
  of its spans from its solved `Rect`; no caller does x-arithmetic.
- Role + glyph ownership (introduced per surface as its Plan step lands): the
  widget's `project`/`paint` chooses the role for each span and the glyph it
  draws. Until a surface is ported, its existing renderer/`addNode` role+glyph
  path stays; a surface is either FULLY widget-owned or FULLY procedural at any
  given commit — never split mid-surface. The renderer keeps its current
  role/glyph code ONLY for not-yet-ported surfaces and loses it per surface as
  each Plan step ports that surface.

Because a widget emits the SAME set of `(id, kind, rect, role, content)` nodes
the procedural code emits today, the `ui_layout` golden and `test_hit_test`
verify the port span-for-span, and no renderer role/glyph logic is duplicated:
ownership moves wholesale per surface, never straddling.

## Invariants

- Server owns layout and rendering; the client is a dumb renderer. Widgets live
  server-side; only their projected artifacts cross the wire.
- `solveLayout` stays PURE geometry and FAILS LOUDLY (returns `nullopt`) rather
  than clamping. The fit pass must guarantee fit before solving.
- Hit-testing takes the FIRST a11y node containing a cell, so projected nodes
  MUST NOT overlap (today's reason the input line reserves field width up front).
  The widget layout must preserve non-overlap.
- Status field positions must not depend on the input line's contents
  (doc/spec-input-line.md): the fields' Container is laid out against a width that
  already has the input-line reservation subtracted.
- Adding a `SemanticRole` requires its five wiring sites (Theme.h enum + count +
  kAllSemanticRoles, Theme.cpp name, DefaultTheme.cpp color, doc/config.md, and a
  renderer consumer). This spec adds NONE — widgets consume existing roles.
- A new configurable glyph key must be registered + validated in `Style.cpp` and
  documented; widgets consume existing `Style` glyph keys, adding none in Phase
  1-2 unless a gap is found.
- The command/protocol codecs round-trip every snapshot type they carry
  (`everySettingKeyRoundTrips`-style tests); any changed projected struct keeps
  its round-trip.

## Considerations

- The single biggest risk is the fit pass reproducing today's collapse/right-pack
  behavior EXACTLY: `addFields`'s stable-sort by collapse rank, the "+2" padding
  per field, the single-space separators, the footer hint yielding before status
  actions, and the input-line reservation. These are behavioral facts with no
  standard name; the port must preserve each, and the `ui_layout` golden +
  `test_hit_test` are the oracle that it did.
- The input line is half field-row, half text widget today (sigil + scrolling
  tail + a held-back caret column). Modeling it as `TextInput` must keep the
  one-column caret reservation and the `visibleQueryTail` scrolling, and keep the
  ghost-completion span.
- The prompt's multi-row shape (replace = query row + replacement row + options
  row; find = query + options) must fall out of the Container tree, matching
  `promptRowCount`.
- Distraction-free mode collapses chrome to just the document; the widget layer
  must honor it (no header/footer Containers built).

## Risks and Mitigations

- Risk: a subtle geometry drift (off-by-one in padding/separator) ships silently.
  Mitigation: Phase 1 is behavior-preserving; regenerate NOTHING first — run the
  existing `ui_layout` golden and `test_hit_test`/`test_render` BEFORE touching
  goldens; any diff is a port bug until proven an intended improvement.
- Risk: scope creep into protocol/paint-span redesign. Mitigation: explicitly out
  of scope; the wire projections are unchanged.
- Risk: the fit pass and solver double-compute or disagree. Mitigation: the fit
  pass only SELECTS and orders children; the solver alone assigns rects.

## Acceptance (Definition of Done)

- Observable: header/footer/prompt render pixel-identically to before across the
  covered cases (default, crowded header that collapses branch, footer with
  status actions + hint, all prompt kinds, distraction-free, picker input line
  with ghost). Because output is unchanged, signoff is "no visible diff", shown
  via the `ui_layout` golden staying byte-identical without regeneration.
- Budgets: no layout-time regression beyond noise (chrome layout is per-frame;
  the fit pass is O(children)); n/a otherwise.
- Gates: `bash scripts/check.sh` green (build + all tests).
- Oracles: see Plan — each phase pins its behavior with an existing golden/hit
  test used as a REGRESSION oracle (must stay green without regeneration), plus
  new unit tests for the widget measure/fit/layout primitives (reference-computed
  hand cases).

## Plan

Phased so each row is independently shippable and behavior-preserving; the
existing goldens are the primary oracle (they must stay green WITHOUT
regeneration, proving no behavior changed). **Per-surface ownership is atomic**
(the ownership contract above): at every commit a surface is either fully
widget-owned or fully procedural, never split. Interim state between steps:

- After step 2: header/footer FIELD/action/hint/input-line GEOMETRY is
  widget-solved and their a11y nodes are widget-projected, but the renderer still
  paints them from those nodes exactly as before (glyphs like the input-line
  sigil still emitted as node content, as today). The prompt is untouched
  (procedural).
- After step 3: the input-line sigil/tail and the (prompt) checkbox GLYPHS are
  widget-`paint`-owned; the prompt LAYOUT is still `computePromptLayout` until
  step 4. Step 3 therefore ports glyph ownership only for the header input line
  (whose layout step 2 already moved) and introduces the `Checkbox` widget used
  by step 4 — no prompt surface is half-ported.
- After step 4: the prompt is fully widget-owned (layout + glyphs + projection).

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Define the Widget primitives + `measure`/`fit`/`layout`/`paint`/`project` free functions (Container, Label, Field, Checkbox, TextInput, Spacer). Pure, no runtime deps. | new `include/ssg/Widget.h`, `src/Widget.cpp` | new unit test: hand-computed measure/fit cases incl. the **stop-at-first-non-fit edge** (a low-rank wide field blocks a later narrow field that would have fit) — reference impl vs output | solver purity; fail-loud |
| 2 | Port header/footer field + action + hint + input-line placement to a Container widget tree; derive `AccessibilityNode`s from its `project` (one node per role-homogeneous span), replacing `addFields` + inline arithmetic. Renderer still paints from the nodes. | `src/ShellState.cpp` | `ui_layout` golden + `test_hit_test` stay green WITHOUT regeneration | non-overlap; fields-independent-of-input-line |
| 3 | Move the toggle/checkbox glyph and the input-line sigil/tail into the `Checkbox`/`TextInput` widgets' `paint`; introduce `Checkbox`. | `src/Renderer.cpp`, `src/Widget.cpp` | `test_render` stays green without regeneration | glyph config in Style; role wiring |
| 4 | Port the prompt to a Container of `TextInput`/`Checkbox`/`Label` widgets, replacing `computePromptLayout`; keep `PromptViewState` as the produced projection. | `src/PromptSurface.cpp` | `test_render` (prompt cases) + prompt-status tests green without regeneration; `PromptViewState` round-trip unchanged | protocol round-trip; promptRowCount shape |
| 5 | Extract `TextInput` as the reusable widget seam (documented entry point for a future non-prompt text field); no new caller yet. | `include/ssg/Widget.h`, doc | unit test instantiating `TextInput` standalone (caret reservation, tail scroll) | - |
| 6 | Document the widget model + the `init.lua`-composition seam it enables (design note only; no binding). | `doc/spec-widget-chrome.md`, `doc/config.md` | doc tests green | - |

## Rationale (optional, skippable)

The top-level shell already proved the box-solver approach for regions; this
extends the same relative-layout discipline DOWN into the row contents, which are
the last procedural-absolute holdout. Framing widgets as a fixed primitive set
composed by data (not an open C++ class hierarchy) is what makes a future
`init.lua` UI binding a matter of handing in a tree of primitive descriptors,
rather than loading native code — the same shape as `theme.set`/`style.define`
today. Keeping the wire projections unchanged makes the whole refactor provable
by the goldens rather than by inspection.
