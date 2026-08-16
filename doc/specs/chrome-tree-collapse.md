# Collapsing built-in header/footer onto the UiNode tree

## Why

The 7A north star is one library-owned UiNode tree that every client renders in
its own idiom. When the library adds a chrome widget, no client does layout work
and web/TUI parity is automatic, because both interpret the *same* tree.

We are not there for the built-in header and footer. Two representations exist:

- The **tree** path: `assembleWholeScreen` builds a header/footer subtree (status
  `Field`s, a `footer.hint` `Field`, a `footer.status_actions` `StatusActions`
  node). This is published on the wire and is the only thing the web consumes.
- The **grid** path: `ShellState::computeShellLayout` renders the built-in
  header/footer from a *parallel* set of `ShellLayoutRequest` inputs
  (`headerFields`, `footerFields`, `footerHint`, `footerActions`) through a
  `WidgetStack`, emitting `ShellNode`s directly. This is what the TUI actually
  draws.

The two paths carry the same information from different sources. The concrete
symptom: the tree's `footer.hint` is a `Field` whose value comes from
`chromeResolverFor`, which only knows the catalog status fields — it has no entry
for `footer.hint`, so the field resolves empty and drops (the semantic drop). The
TUI never noticed because it draws the hint from `request.footerHint`, not from
the tree. The web has no second source, so the help hint is absent.

(The `footer.status_actions` node is a different case, and the spec must not
conflate them: `StatusActions` is an *intentionally opaque* widget whose data
rides the typed `promptStatus` section, not `uiState`. It does not "resolve" or
"drop" like a `Field`; it renders from `promptStatus` and is simply empty when no
status item carries actions. The web already renders it from `promptStatus`. So
the missing element the user saw is the hint; the status-actions concern here is
about making the *TUI* lowering render that same opaque node from the same typed
channel, not about resolver drops.)

The consequence the user hit: **web parity is impossible while built-in chrome
bypasses the tree.** Every widget the TUI draws must be a node in the tree, and
the built-in header/footer must be rendered by lowering that tree — the same way
a composed (`ssg.chrome`) region already is.

The good news the audit surfaced: the tree-lowering path
(`lowerUiChromeRegion` -> `lowerChromeGroups`) already uses the *same*
rank-based collapse engine as the grid path (`WidgetStack::resolve` / `fitRow`).
The collapse behavior is not a blocker; it is shared code. What is missing is
(1) a resolver that supplies the hint's label/command, (2) lowering support for
the `StatusActions` widget, and (3) making built-in chrome go through lowering at
all instead of the bespoke `WidgetStack` packing in `ShellState`.

## The end state

There is no built-in-chrome-specific rendering code. `ShellState` lowers the
header and footer subtrees of the assembled UiNode tree — identically whether
those subtrees are built-in or composed — over their rects, through one lowering
path. Every header/footer widget (status fields, help hint, status actions, and
eventually the prompt input line) is a tree node.

Per-frame data reaches the render through **two typed channels, by widget kind**,
not one uniform channel:

- A `Field`/`Label`/`Checkbox` resolves its value/label/command through the
  `ChromeProviderResolver`. The TUI lowering resolves it in **Grid mode** (the
  formatted values, e.g. `cwdPrefix`) to keep the TUI grid byte-identical; the
  wire `uiState` resolves it in **Semantic mode** (no grid formatting reaches
  native clients). Same tree structure, resolved twice — the unification is of
  structure, never of resolved values.
- A `StatusActions` node is opaque and carries no `uiState` leaf. Its data rides
  the typed `promptStatus`/status-queue projection: the web reads it from
  `promptStatus`, and the TUI lowering reads the same typed projection passed as a
  distinct input. It is never routed through the field resolver.

`ShellLayoutRequest` no longer carries `headerFields`, `footerFields`,
`footerHint`, or `footerActions`; the grid and the wire render the same nodes, so
their structure cannot diverge.

The grid-parity golden (`tests/fixtures/runtime/grid_parity.txt`,
`test_runtime_grid_parity`) is the guardrail: the TUI rendered grid must stay
byte-identical through the collapse (regenerated only if a deliberate, reviewed
change alters it).

## What has to move, and the decisions it forces

### 1. The assembled tree must reach `ShellState`

`shellView` (src/runtime/snapshot.cpp) builds the `ShellLayoutRequest` and calls
`computeShellLayout`. At that point the fully assembled tree — built-in or
composed, already merged by `assembleWholeScreen` — is available via
`interaction.interaction().schema()`. Today only a composed override reaches
`ShellState` (`request.composedUi`); the built-in subtree does not.

**Decision A (resolved): pass the whole `ValidatedSchema`.** `ShellState` selects
the canonical header/footer regions internally by well-known id. Passing raw
region subtrees would discard the validated topology and generation
correspondence and create another caller-maintained seam. The `ValidatedSchema`
is the single validated object both the TUI lowering and the wire `uiState`
derive from, so they cannot reference different trees.

### 2. `StatusActions` must lower from its typed backing channel

The `footer.status_actions` node is an opaque `StatusActions` widget. Its data
(the selected status item's actions, its status id, generation) rides the typed
`promptStatus` section. `lowerChromeGroups` handles `Field`/`Label`/`Checkbox`/
`Spacer` but not `StatusActions`; it must expand the node into its action items,
pack them right in reverse order (matching today's emission), and collapse them
with the shared engine.

**Decision B (resolved): keep the typed `promptStatus` backing channel and pass
it to TUI lowering as a distinct input.** Do **not** widen `ResolvedProvider`, do
**not** copy actions into `uiState`, and do **not** reinterpret actions as
provider/command clicks. `StatusActions` stays opaque and typed end to end: the
web reads it from `promptStatus`, and the TUI lowering reads the *same* typed
projection passed alongside (not through) the `ChromeProviderResolver`. This
keeps one typed source for the actions while leaving the `Field` resolver seam
untouched. (This corrects an earlier draft that proposed resolver-carried actions
— that would collapse the opaque widget into the field channel and is rejected.)

**Invocation representation (the typed sidecar).** The invocation identity a
status action dispatches — `StatusActionInvocation{statusId, actionId,
generation}` (include/ssg/StatusQueue.h) — has **no typed carrier today**: there
is no `HitRegion` for status actions, and `AccessibilityNode`/`RegionHit` carry
only `fieldId` + `commandId`. `commandId` must **not** be overloaded to smuggle
it (that routes through the plain-command dispatch and bypasses the generation
freshness gate — exactly the distinction the `FooterHint` comment preserves).

The exact lowering input is **`StatusViewState`** (include/ssg/StatusQueue.h),
obtained via `PromptStatusViewState::status` (what `promptStatusView()` returns) —
it carries the status id, generation, and the ordered actions. Do **not** pass
`StatusFooterProjection` (`footerProjection()`): it lacks the status id and
generation and would recreate a parallel authority. The collapse adds a typed
path:

- `AccessibilityNode` (the ShellNode) gains an optional
  `std::optional<StatusActionInvocation> statusInvocation`, populated only for
  `FooterAction` nodes during lowering from the typed status projection. It does
  not change any rendered cell, so the grid golden (which captures rendered grid
  text, not node structs) stays byte-identical. `AccessibilityNode` **is
  protocol-encoded** (src/Protocol.cpp `toValue`/`decodePresent`), so the field is
  added with **additive** encoding: encode when present, decode as optional/absent
  for backward compatibility. The `session_snapshot` protocol fixture is
  regenerated (accessibility nodes ride it), and a protocol round-trip test covers
  the new field.
- `HitRegion` gains `StatusAction`; `RegionHit` gains
  `std::optional<StatusActionInvocation> statusInvocation`. `HitTester` maps a
  `FooterAction` node to `HitRegion::StatusAction` carrying that invocation.
- Status-action clicks flow through the existing pointer seam
  `apps/pointer_routing.{h,cpp}` — `PointerTargets` gains the invocation and
  `route_pointer` dispatches `status.invoke_action` with it (mirroring how a
  `FooterField` hit routes through `targets.field_command_id`). No bespoke dispatch
  is added in `apps/ssg_main.cpp`. This is the same library command and generation
  gate the web's binary `StatusActionInvocation` already uses — both clients reach
  one behavior path.

**Decision B2 (resolved): TUI click dispatch is in Phase 1**, not render-only. The
typed carrier and the pointer route land together; leaving status actions
un-clickable in the TUI would keep the web ahead and is not the parity target.

### 3. The hint must resolve through the field channel

`hintField` is a tree `Field` with id `footer.hint`, provider-backed. It drops
because `chromeResolverFor` has no entry for it. The resolver must resolve
`footer.hint` to the live help label (from the `help.open` binding) and the
`help.open` command — the same value `request.footerHint` carries today. Unlike
the actions, the hint genuinely *is* a `Field`, so the field-resolver channel is
correct for it.

**Exact unbound behavior (the oracle target).** Mirroring today's
`request.footerHint` construction: when `help.open` is bound, `footer.hint`
resolves to the label `"<keys>  help"` (the formatted key sequence, two spaces,
`help`); when `help.open` is **unbound**, it resolves to the literal label
`"help"` (no key prefix) — **not** absent — with the `help.open` command still
attached in both cases. The command is always present so the hint stays clickable;
only the label's key prefix is conditional. The seam oracle asserts both exact
label forms and that the command is `help.open` in both.

### 4. Grid vs. semantic value parity

The TUI and the wire deliberately resolve *different values* over the *same tree
structure*. `chromeStatusFields(ChromeFieldMode::Grid)` applies presentation
formatting (e.g. `cwdPrefix`) that must **not** be published to native clients;
the wire uses `ChromeFieldMode::Semantic`. The collapse must preserve this: one
tree structure, resolved twice —

- TUI lowering resolves through a **Grid-mode** resolver (the formatted values,
  producing byte-identical TUI output including `cwdPrefix`).
- The wire `uiState` resolves through the existing **Semantic-mode** resolver (no
  grid formatting), exactly as it does today.

The spec's unification is of *structure and channel*, never of *resolved values*:
the byte-identical TUI grid comes from the Grid-mode resolver, and native clients
never receive grid-formatted strings. The hint and the status-actions projection
follow the same rule (the TUI lowering may format for the grid; the wire carries
the semantic form).

### 5. Node kinds, roles, and hit-testing must be preserved

**Decision C (resolved): preserve `FooterHint` and `FooterAction` node kinds and
their roles.** Renderer behavior depends on both, and `FooterHint` hit-testing
deliberately dispatches its command *directly* rather than through the
status-action freshness gate. Lowering must therefore stamp the correct
`ShellNodeKind` and `SemanticRole` per widget (hint -> `FooterHint`, each action
-> `FooterAction`, status field -> `FooterField`/`HeaderField`), so the golden and
hit-test order are unchanged and the enums stay. Lowering gains per-widget kind
selection rather than a single `nodeKind` argument for the whole region.

### 6. Scope of "everything in header/footer"

The one header element still outside the tree is the **prompt input line** (the
palette/finder query with caret, scrolling tail, ghost text), placed by
`ShellState` after the header fields. A `TextInput` widget kind already exists.

**Decision D (resolved): `TextInput` is Phase 2.** Its caret, ghost, scrolling,
focus, and local-prediction semantics are independently design-heavy and are not
required to remove the duplicated status-chrome path. Phase 1 collapses the
status fields, help hint, and status actions; Phase 2 makes the prompt input line
a `TextInput` tree node.

### 7. The notice row stays out of Phase 1

**Decision E (resolved): explicitly keep `ShellNotice`, the notice request state,
and notice lowering outside Phase 1** until the notice is represented in the
authoritative tree. Compiler-driven deletion is not sufficient protection: the
notice is a TUI-only chrome element with no current wire representation, and
unifying it is a separate concern. Do not touch it in Phase 1; do not delete its
structs.

## Deletion surface once Phase 1 lands

Driven by the compiler after the tree becomes the only path for status
fields/hint/actions:

- `ShellLayoutRequest`: `headerFields`, `footerFields`, `footerHint`,
  `footerActions`.
- Structs `ShellFooterHint`, `ShellLabel` — **only** once proven unused after the
  fields/hint/actions collapse. Do **not** delete `ShellNotice` /
  `ShellNoticeAction` (Decision E).
- `ShellState::computeShellLayout` built-in `WidgetStack` header block and footer
  block (the packLeft/packRight/resolve loops and their `ShellNode` emission),
  replaced by lowering the assembled regions.
- **Keep** `ShellNodeKind::FooterHint`/`FooterAction` (Decision C).
- In `WholeScreenAssembly`: **keep** `hintField` and `statusActionsWidget` — they
  are the tree nodes we are making authoritative, not dead code.

Keep `WidgetStack`/`fitRow` (the shared collapse engine), `StatusFieldCatalog*`,
`projectStatusFields`, `chromeRegion`, and `lowerUiChromeRegion`.

## Plan (ordered, one reviewable step each)

Each step keeps `scripts/check.sh push` green and the grid-parity golden
byte-identical unless the step explicitly regenerates it with review.

1. **Lowering stamps per-widget kind/role.**
   Production: `src/ChromeLowering.cpp` (`lowerChromeGroups`, `lowerUiChromeRegion`
   — replace the single `nodeKind` argument with per-widget kind/role selection:
   hint id `footer.hint` -> `FooterHint`/`SemanticRole::Footer`, status field ->
   `HeaderField`/`FooterField`), `include/ssg/ChromeLowering.h`.
   Test: `tests/test_chrome_lowering.cpp` — assert the emitted `ShellNodeKind` and
   `SemanticRole` per widget for a region holding a hint-like and a field-like
   widget. No behavior change yet (composed regions only).

2. **Lowering learns `StatusActions` + the typed invocation carrier.**
   Production: `include/ssg/ShellState.h` (add
   `std::optional<StatusActionInvocation> statusInvocation` to `AccessibilityNode`),
   `src/Protocol.cpp` (additive encode/decode of `statusInvocation` on
   `AccessibilityNode` — encode when present, decode as optional/absent),
   `include/ssg/HitTester.h` (`HitRegion::StatusAction`, `RegionHit.statusInvocation`),
   `src/HitTester.cpp` (map `FooterAction` -> `HitRegion::StatusAction` with the
   invocation), `src/ChromeLowering.cpp` (expand a `StatusActions` node into its
   action items from the typed projection input — the `StatusViewState` from
   `PromptStatusViewState::status`, carrying status id + generation + ordered
   actions; **not** `StatusFooterProjection` — pack right in reverse order,
   collapse via the shared engine, emit `FooterAction` carrying the invocation),
   `apps/pointer_routing.{h,cpp}` (`PointerTargets` gains the invocation;
   `route_pointer` dispatches `status.invoke_action` with it).
   Test: `tests/test_chrome_lowering.cpp` (a seam test lowering a footer with a
   `StatusActions` node at **wide and narrow** widths: action order, `FooterAction`
   kind/role, collapse priority, and the invocation identity round-trip preserved,
   not reinterpreted as `commandId`); `tests/test_hit_test.cpp` (a `FooterAction`
   node hit yields `HitRegion::StatusAction` with the exact `StatusActionInvocation`);
   `tests/test_ssg_app.cpp` (a status-action pointer click routes to
   `status.invoke_action` with the invocation); `tests/test_protocol.cpp` +
   `session_snapshot` fixture regen (the additive `statusInvocation` round-trips).

3. **The `footer.hint` resolves in both modes.**
   Production: `src/runtime/snapshot.cpp` (`chromeResolverFor` / the Grid-mode
   resolver and the Semantic-mode wire resolver both resolve `footer.hint` to the
   help label + `help.open` command from the live binding, per the exact
   bound/unbound forms above).
   Test: `tests/test_chrome_lowering.cpp` (or `tests/test_ui_layout.cpp`) — assert
   the hint resolves to `"<keys>  help"` when bound and `"help"` when unbound, with
   `help.open` in both, across both resolver modes.

4. **`ShellState` lowers the built-in regions.**
   Production: `include/ssg/ShellState.h` + `src/ShellState.cpp` (accept the whole
   `ValidatedSchema` + the Grid-mode resolver + the typed `StatusViewState`
   projection; select canonical header/footer regions by id; lower them for the
   built-in case exactly as the composed case already does; route the prompt input
   line off the lowered header's right edge as today — input line stays Phase 2),
   `src/runtime/snapshot.cpp` (pass those inputs into `computeShellLayout`).
   Test: `test_runtime_grid_parity` byte-identical across all four states;
   `tests/test_ui_layout.cpp` updated to the one path.

5. **Delete the parallel inputs.**
   Production: `include/ssg/ShellState.h` (remove `headerFields`, `footerFields`,
   `footerHint`, `footerActions` from `ShellLayoutRequest`; remove
   `ShellFooterHint`/`ShellLabel` if now unused — keep `ShellNotice`/
   `ShellNoticeAction`), `src/ShellState.cpp` (remove the built-in `WidgetStack`
   header/footer packing blocks), `src/runtime/snapshot.cpp` (remove the code that
   built those request fields). Let the compiler find every caller.
   Test: build is the deletion proof; `test_runtime_grid_parity` still
   byte-identical; full `check.sh push` green.

6. **Web parity confirmation (no web code change).**
   Manually verify the footer help hint (and status actions when present) render on
   the web purely from the published tree — the proof that the tree is the single
   source. No production change; visual signoff only.

## Guardrails and validation

- `test_runtime_grid_parity` byte-identical across its four states (editor-only,
  panel shown, palette open, find open) — the primary end-to-end oracle.
- The **new seam oracles** in plan steps 1–3 (per-widget kind/role, status-action
  lowering across wide/narrow widths preserving order + kind + invocation
  identity, hint resolution across resolver modes). The grid golden alone does not
  independently prove action backing, typed invocation, collapse priority, or
  narrow-width ordering, so these focused oracles are required, not optional.
- Existing `test_chrome_lowering`, `test_ui_layout`, `test_whole_screen_assembly`
  stay green or move to the one path.
- `bash scripts/check.sh push` green.

## Resolved decisions (summary)

- A: pass the whole `ValidatedSchema`; select regions internally.
- B: `StatusActions` stays opaque on the typed `promptStatus` channel, passed to
  TUI lowering as a distinct input; never resolver-widened or copied into
  `uiState`. Its invocation identity rides a typed sidecar
  (`AccessibilityNode.statusInvocation`, additively protocol-encoded) through
  `HitRegion::StatusAction` and the `pointer_routing` seam to `status.invoke_action`.
- B2: TUI status-action click dispatch is in Phase 1 (parity with the web).
- C: preserve `FooterHint`/`FooterAction` kinds and roles; lowering stamps them
  per widget.
- D: `TextInput` prompt input line is Phase 2.
- E: the notice row stays entirely outside Phase 1.
- Value parity: one tree structure resolved twice — Grid-mode for the TUI
  (formatted, byte-identical grid), Semantic-mode for the wire (no grid
  formatting reaches native clients).
