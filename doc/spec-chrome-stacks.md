# spec-chrome-stacks

Status: done (phases A–F shipped)

## Goals

Generalize header/footer chrome from today's three ad-hoc placement mechanisms
(`fitRow`, `packEnd`, and manual input-line x-arithmetic) into a small composable
vocabulary — a `WidgetStack` row primitive (`packLeft`/`packRight`/`center`) for
the status-field and footer rows, and a `layoutInputLine` widget seam for the
picker input line — and make prompt anchoring/focus explicit, single-sourcing the
find/replace footer reservation so its geometry is derived once. After this
change (as shipped, phases A–F):

- One row is composed as `stack.packLeft(a).packRight(z).packLeft(b)...`,
  yielding `abc…xyz`: left items pack from the leading edge, right items from the
  trailing edge, an optional single `center` slot sits between them at an
  explicit or flexible width.
- Which items survive a crowded row is a STACK-level policy (left-group
  `collapse` by rank; right-group clamp-truncate); how a surviving item's content
  fits its own cell is a WIDGET-level overflow policy (`Truncate` or
  `ScrollTail`). The two are orthogonal, not competing.
- The find/replace prompt reservation is SINGLE-SOURCED: one full-width bottom
  strip (`view.prompt`) that the shell layout, the a11y node/hit region, and the
  rendered controls all consume, shrinking the editor from the bottom. (The
  original plan to make this a variable-height footer MODE STACK — a `FooterMode`
  value type with push/pop and status-line suppression — was found speculative
  and DEFERRED; see §The model as shipped. Find still takes 2 rows, replace 3,
  as before.)
- Prompt anchoring/focus is made EXPLICIT via `promptFocusRegion(kind)` (header
  for the palette, footer for find/replace) rather than a new runtime mode stack:
  there is a single `PromptSurface`, so at most one prompt is active and keyboard
  focus follows it unambiguously. This makes today's implicit "keyboard always
  goes to the prompt" explicit in the layout model without new machinery.

Non-goal (explicit): exposing stacks/regions to `init.lua` (a later spec; this
one only keeps the seam open); moving the palette out of the header; adding
`SemanticRole`s or `PromptKind`s; changing on-screen output in the porting
phases (the existing goldens are the oracle — byte-identical without
regeneration).

## Design

### WidgetStack: one row primitive

`WidgetStack` replaces `fitRow` + `packEnd` + `addFields` (the status-field and
footer-action placement) with a single builder that lays out one row of the
header or footer. The picker input line is NOT folded into a stack item — it
keeps its own `layoutInputLine` widget seam (reserve/grow width + ghost), placed
alongside the header field stack — so "one row primitive" means the STATUS-FIELD
and footer rows, with the input line a sibling widget on the header row.
It is a pure value type (no renderer/runtime dependency), consistent with the
existing `Widget.h` free-function style; the builder methods mutate and return
`*this` so calls chain.

Three groups per row:

- **left** — `packLeft(item)` appends to a left group that packs from the row's
  leading edge rightward, in call order.
- **right** — `packRight(item)` appends to a right group that packs from the
  trailing edge leftward. Call order is left-to-right in the FINAL layout (so
  `packRight(x).packRight(y).packRight(z)` renders `xyz` flush right), matching
  the footer's current reverse-emit behavior.
- **center** — `center(item, width)` sets the SINGLE center slot. `width` is
  `Fixed(n)` or `Flex` (absorbs the gap between left and right). At most one
  center per stack; a second `center` call is a spec error (fail loud, mirrors
  `solveLayout`'s fail-loud contract). Center never stacks.

Right-group width allocation (reproduces `packEnd` EXACTLY — the collapse rule
does NOT cover this): the right group fills from the trailing edge leftward,
each item taking `min(remaining, desired)` cells. The rightmost items keep their
full desired width; when the running total exceeds the extent the LEFTMOST right
item that still has room is **clamp-truncated** to the cells remaining, and any
right item with zero room left is **dropped**. This is positional
clamp-truncation, distinct from the left group's rank collapse, and is the
behavior Step B must preserve span-for-span. `packRight` items therefore do NOT
take a `Collapse` rank; their membership is decided by remaining room, not rank.

Resolution order (must match today's behavior): the right group claims its
desired cells first, the left group fills the remainder, the center slot takes
what is left between them (Fixed clamped to available; Flex = the whole gap).
This reproduces the footer (right group budgeted first, left fills the rest) and
the header (left fields budgeted, input line in the remainder) with one rule.
A `Fixed` center clamps to the available gap when the row is too narrow to
honor it in full (graceful, like the left collapse and right truncate); it is
NOT fail-loud. The only fail-loud center case is a SECOND `center` call, which
is a structural authoring error, not runtime narrowness.

### Item, and the two membership/overflow levels

A stack item as shipped is `StackItem { id, content, desired, rank, keep,
overflow, sigil }` — plain fields (there is no separate `StackFit` type; the
membership policy is expressed by `rank` + `keep` for the left group, and by
position for the right group):

- Left-group MEMBERSHIP when the group cannot fit every item: items with
  `keep = false` collapse by `rank` (drop the whole item, lowest rank first,
  survivors intact — today's header/footer status FIELDS); `keep = true` items
  are never dropped (the primary content). The fit pass reproduces `fitRow`'s
  EXACT rule: stable-sort by rank, forward-scan appending while it fits, STOP at
  the first non-fit (NOT drop-until-fits — that picks a different retained
  subset). The RIGHT group's membership is the positional clamp-truncate/drop
  rule above (today's footer actions + hint); the center slot is never dropped —
  its `Fixed` width clamps to the available gap, and only a second `center` call
  is fail-loud.
- `overflow` — the WIDGET-level policy for a surviving item whose CONTENT exceeds
  its cell: `Overflow::Truncate` (clip to the cell; today's footer actions/hint
  render, clipped by the renderer to the rect) or `Overflow::ScrollTail` (pin the
  item's `sigil`, show the grapheme tail of `content` via `layoutTextInput`).
  `Overflow::None` is a field that always fits.

Collapse decides WHICH items are present; Overflow decides how a present item's
text fills the cell it received. A narrow header collapses the branch field out
(whole), and independently scroll-tails the palette query within its own cell.

### The input line stays on its seam until the prompt-mode phase

Phase C ports the header STATUS FIELDS onto a `WidgetStack` (left collapse group
over `width - inputLineReservation`, the fixed floor that keeps fields from
reflowing as the query grows), and deletes `addFields` (the header was its last
caller after phase B). The picker INPUT LINE + ghost are deliberately NOT modeled
as stack items in phase C: the input line needs two different widths (its fixed
`inputLineReservation` budgets the field floor, but its actual node spans ALL the
remaining space, text-limited via `layoutTextInput`) and a non-uniform gap (1
cell field→input, 0 input→ghost) — neither of which the row primitive expresses.
That is real primitive design (reserve-vs-grow width, per-item gaps, ghost
handling), and the input line is the palette PROMPT. Phase E completes it by
bundling the query + ghost into a `layoutInputLine` widget seam (it does NOT
become a stack item — that would need the reserve/grow-width + per-item-gap
primitive extensions, deferred until a Lua binding needs them). Until phase E the
input line keeps the plain `layoutTextInput` seam it already uses.

### Output: still the existing projections

A resolved `WidgetStack` produces the SAME `(id, kind, rect, role, content)`
`AccessibilityNode`s the procedural code emits today (one node per item, or the
per-role-homogeneous-span rule from spec-widget-chrome for a split-role item
like the input line + ghost). The `ui_layout`/`test_hit_test`/`test_render`
goldens verify each port span-for-span; no wire type changes in the porting
phases.

### Footer prompt reservation: single-sourced (NOT a mode stack — shipped)

The original design promoted the footer to a variable-height REGION WITH MODES (a
`FooterMode { rows, focusable, promptKind? }` value type, a push/pop mode stack,
the footer node becoming `exact(activeFooterRows)`, and the footer status line
suppressed while a prompt owns the region). **That restructure was NOT built** —
it was found speculative (its only near-term effect, suppressing the
already-overpainted footer status nodes, changes goldens for no user-visible
gain) and DEFERRED until a second footer mode or the `init.lua` binding needs it.

What shipped instead (phase D) is the load-bearing part of the fold — a
SINGLE-SOURCE prompt rect. The footer node stays `exact(footerHeight=1)`, and
find/replace still reserve rows over the bottom via `computePromptLayout`; but
`view.prompt` is now the full-width bottom strip that the shell layout, the a11y
node/hit region, AND the rendered controls all consume (`promptStatusView`
receives it rather than recomputing). The editor shrinks from the bottom by that
rect's height. The original mode-stack prose is retained below as the DESIGN
RATIONALE for the deferred increment, not as shipped behavior:

Design rationale for a future `FooterMode` (deferred):

- A `FooterMode` is `{ rows: [RowSpec], focusable: bool, promptKind? }` where a
  `RowSpec` is either a `WidgetStack` (the status line) or a solved sub-tree (the
  find/replace control grid — reuse `computePromptLayout`'s existing
  `solveLayout` tree unchanged, just hosted in the footer region rather than a
  bottom reservation).
- The region carries a **mode stack**: default status mode at the bottom; a
  prompt open PUSHES its mode; close POPS. The footer region height = the active
  (top-of-stack) mode's row count; the shell tree's footer node becomes
  `exact(activeFooterRows)`, and the editor shrinks from the bottom exactly as
  the prompt reservation does today (never moving the document top).
- Because the height derivation and the bottom-shrink are identical to today's
  reservation math, the observable layout is unchanged — find still takes 2 rows
  over the footer, replace 3 — but it is now one mechanism (footer modes) rather
  than two (footer region + editor reservation).

Single-source prompt rect (contract — the load-bearing part of phase D, DONE):
today the prompt geometry was derived in TWO places — the shell layout's
`reservedPromptRows`/`view.prompt` reservation (ShellState.cpp) and the
prompt-status reservation recomputed from viewport dimensions in
`promptStatusView` (runtime/snapshot.cpp). These DIVERGED under a panel: the
shell reserved an EDITOR-width rect (`{editor.x, …, editor.width}`) while the
controls actually rendered FULL width (`{0, …, columns}`), so the a11y node / hit
region and the rendered controls disagreed on the bottom rows. Phase D makes both
consume ONE rect: `view.prompt` becomes the FULL-WIDTH bottom strip (`{0,
promptTop, columns, reservedPromptRows}` — the footer-region width it sits over),
and `promptStatusView` CONSUMES that rect (passed in from the shell view) rather
than recomputing. Under no panel the two were already equal, so rendered output
is byte-identical; under a panel the reservation reconciles toward the full-width
value the controls already used, so no pixel changes — only the a11y prompt-
reservation rect (and its hit region) widen to match what renders. The fallback
(compute from dimensions) remains for the palette (zero prompt rows; its input
lives in the header) and the no-active-prompt default, where the shell reserves
nothing. Oracle: a runtime test asserts `shell.prompt == promptStatus.prompt->rect`
AND full-width (x=0, width=columns) across panel-off and panel-on.

Realization note (scope): phase D ships the single-source contract above, which
IS the "fold" — the prompt is now the footer-anchored prompt's ONE full-width
rect, and the editor still shrinks from the bottom by that rect's height
(functionally identical to "the footer region grew"). The heavier `FooterMode {
rows, focusable, promptKind? }` value type + a variable-height footer node +
suppressing the footer status line while a prompt is open were then evaluated in
phase E and DROPPED as speculative — not built. Rationale: suppressing the
currently-emitted (and merely over-painted) footer status nodes would change the
goldens for no user-visible gain, and the `FooterMode` type earns its keep only
when a SECOND footer mode or the `init.lua` binding needs it. It remains the
natural next increment, documented but deferred.

Multi-row footer modes are preserved deliberately: find/replace stay 2/3 rows
even though find alone could be one line, because a future richer footer surface
(find+replace together, results preview) wants the multi-row affordance.

### Prompt as a focusable region mode

`FocusTarget{Editor, Panel, Prompt}` and the focus stack already route keyboard
unambiguously to an active prompt. This spec makes the ANCHORING explicit —
WITHOUT inventing a runtime mode-stack, because the focus model is already
unambiguous by construction and a mode-stack would restate what the type system
guarantees:

- There is exactly ONE `PromptSurface` (a single `std::optional<PromptRequest>`),
  so at most one prompt is ever active. "At-most-one" needs no runtime
  enforcement — a fail-loud assertion would guard an invariant `std::optional`
  already provides, and a mode-stack that could hold several prompts would exist
  only to be forbidden. So this spec does NOT add a mode-stack or that assertion.
- `reconcilePromptFocus` already keeps `FocusTarget::Prompt` coupled to
  `prompt.active()`. The improvement is to NAME the anchoring: `promptFocusRegion(
  PromptKind) -> {Header, Footer}` is the single expression of where a prompt of
  a given kind lives — Header for the palette (its query IS the header input
  line; results narrow to the top of the buffer just below it), Footer for
  find/replace/settings/etc. (a footer reservation). The ANCHORING decision
  "does the header host a prompt input line" (`inputLineActive` in
  `runtime/snapshot.cpp`) routes through it. Picker-MACHINERY sites (the
  `palette.execute` guard, the open-picker cleanup) stay gated on the picker
  identity (`kind == Palette`), not the region — region is reserved for anchoring
  so a future non-palette header prompt gets an input line without dragging
  palette-picker plumbing with it.

The heavier `FooterMode` value type, a variable-height footer node, and
suppressing the footer status line while a prompt is open are DEFERRED: they are
speculative until a SECOND footer mode or the `init.lua` binding actually needs
them, and suppressing the currently-emitted footer status nodes would change the
goldens for no user-visible gain. Phase F documents the model + the seam where a
future Lua binding would motivate that structure for real.

### Bundling the picker input line

The picker input line's query + completion ghost become one widget computation
`layoutInputLine(sigil, query, ghost, available)` (extending the `TextInput`
seam): the query via `layoutTextInput` (caret reservation + tail scroll), then
the ghost in the cells the query left, clamped to the ghost's display width. This
moves the reserve/grow + ghost geometry off ShellState-inline into the widget
layer (the caret column and node rects/roles stay with the caller). It completes
the input-line generalization deferred from phase C without needing the prompt to
be a full stack item.

### The model as shipped + the `init.lua` composition seam

What phases A–E leave in place, and the seam a future `init.lua` binding would
use (design note; NO binding is built, and it is out of scope here):

- **One row primitive.** The header STATUS-FIELD row and the whole footer row are
  `WidgetStack`s (`packLeft`/`packRight`/`center`) resolved over an extent,
  replacing the `fitRow` + `packEnd` + `addFields` placement. The picker input
  line remains a sibling widget (`layoutInputLine`) on the header row, not a stack
  item. Membership is `rank` + `keep` (left) / positional clamp-truncate (right);
  content fit is per-item `overflow` (`Truncate` / `ScrollTail`). `solveLayout`
  places the top-level regions; the stack places a row's contents.
- **Regions and anchoring.** The shell is regions (header / body / footer) holding
  content. A prompt is anchored to a region by `promptFocusRegion(kind)` — Header
  (the palette, whose query is the header input line) or Footer (find/replace,
  whose reservation is single-sourced full-width and shrinks the editor from the
  bottom). There is one `PromptSurface`, so at most one prompt is ever active and
  `FocusTarget::Prompt` resolves unambiguously to the hosting region.
- **Closed vocabulary, data-SHAPED internally.** The widget kinds are a fixed enum
  (`Container`/`Label`/`Field`/`Checkbox`/`TextInput`/`Spacer`); a status/footer
  row's placement is no longer bespoke C++ arithmetic but a list of
  `StackItem{id, content, desired, rank, keep, overflow, sigil}` plus a `center`.
  The C++ callers still BUILD those items (there is no runtime data-driven row
  composition yet) — but because the model is already this shape, exposing it to a
  Lua author is a matter of accepting the same descriptors, not redesigning the
  layout.

The `init.lua` seam this opens (design only): a future config call would accept a
tree of stack/region descriptors — the same data-composition shape
`theme.set`/`style.define` already use — the server would validate it against the
fixed `WidgetKind` set and the fit/solve rules (failing loud on an unknown kind
or an unsolvable row, exactly as `resolve`/`solveLayout` already do), and NO
native code would load. The three gates a binding must respect are already
invariants below: server-owned layout, `solveLayout`/stack purity + fail-loud,
and non-overlapping projected nodes. Nothing on the wire or in the renderer has
to change to KEEP this seam open — it is a property of having funneled every row
through the closed-primitive `WidgetStack` instead of ad-hoc placement. The
`FooterMode` value type (a named, data-described footer mode) is the natural next
increment WHEN a second footer mode or the Lua binding needs it; this spec
deliberately does not add it speculatively.

## Invariants

- Server owns layout/rendering; the client is a dumb renderer. Stacks and prompt
  reservations (and any future mode-stack) live server-side; only their projected
  `AccessibilityNode`/prompt-view artifacts cross the wire.
- `solveLayout` stays PURE and FAILS LOUDLY. The stack fit pass must guarantee
  fit before any solve; a second `center` is a fail-loud spec error, not a clamp.
- At most one prompt is active — guaranteed by the single `PromptSurface` (one
  optional request), not by a runtime check; `FocusTarget::Prompt` is coupled to
  `prompt.active()` by `reconcilePromptFocus`, and `promptFocusRegion(kind)` is
  the single source of which region hosts it.
- Projected nodes MUST NOT overlap (hit-testing takes the FIRST node containing a
  cell). The right-group-budgeted-first resolution preserves the current reason
  the input line reserves its width up front.
- Status field positions must not depend on the input line's contents
  (spec-input-line.md): the left group is laid out against a width that already
  has the right group's (and any center-Fixed) budget subtracted.
- The collapse fit rule is EXACTLY today's `fitRow` (stable-sort by rank, forward
  scan, stop at first non-fit) — not drop-until-fits.
- Opening a prompt shrinks the editor from the BOTTOM only (document top never
  moves).
- The prompt geometry has a SINGLE source: the full-width `view.prompt`
  reservation rect. The shell reservation and the prompt-status reservation both
  consume it; neither recomputes a prompt rect independently.
- Adding a `SemanticRole` or a `Style` glyph key requires its existing wiring
  sites; this spec adds none.

## Considerations

- **Behavior-preserving ports vs. the single-source fold.** Phases B/C (port
  footer, then header, onto `WidgetStack`) are strictly behavior-preserving —
  goldens are the oracle, no regeneration. Phase D (single-source the prompt
  reservation rect) is render-neutral: it makes `view.prompt` full-width and has
  `promptStatusView` consume it rather than recompute, so the two reservations
  can no longer diverge under a panel; the only non-render change is the a11y
  reservation rect widening under a panel to match what already renders. The
  prompt-status, render, and single-source oracles pin it.
- **Two input surfaces stay distinct.** The palette (header) and find/replace
  (footer) are different anchors; the unification is the FOCUS model and the
  stack primitive, not merging them into one surface.
- **Center width for fuzzy-find.** The palette query must not shift as branch/
  status fields collapse; a `center(query, Fixed(n))` (or the input line as a
  budgeted right/left item, as today) keeps it put. The spec keeps the current
  header input-line placement in phase C and only introduces `center` where a
  fixed-width middle is actually wanted.
- **packRight call-order semantics.** `packRight(x).packRight(y).packRight(z)`
  must render `xyz` flush-right (not `zyx`); the builder appends and the
  resolver reverses internally, matching the footer's current reverse-emit.
- **Node emission order.** Hit-test order and golden node sequence depend on
  emission order; each port must emit nodes in the same order the procedural
  code did (footer: actions reverse, then hint; header: fields, then input line,
  then ghost).

## Risks and Mitigations

- **Silent retained-subset drift** if `collapse` is implemented as
  drop-until-fits. Mitigation: reuse the existing `fitRow` implementation as the
  collapse engine and keep its unit tests (incl. the stop-at-first-non-fit edge).
- **Prompt fold off-by-one** (editor height / caret row). Mitigation: phase D
  keeps `computePromptLayout`'s solve tree verbatim and re-hosts it; the
  prompt-status + render goldens and `inputLineCaret` tests stay green without
  regeneration.
- **Focus regression** (keyboard to the wrong surface). Mitigation: at most one
  prompt is active by construction (a single `PromptSurface` optional), and
  `reconcilePromptFocus` couples `FocusTarget::Prompt` to `prompt.active()` — no
  fail-loud assertion is added because there is no way to violate it; existing
  focus-stack tests plus the region oracle stay green.
- **Scope creep into Lua.** Mitigation: the Lua binding is explicitly out of
  scope; phase F documents the seam only.

## Acceptance (Definition of Done)

- Observable: no visible change in any porting phase (goldens byte-identical
  without regeneration); find/replace look and behave exactly as today, their
  reservation now single-sourced full-width rather than derived twice. The one
  intended non-render change (phase D) is the a11y prompt-reservation rect
  widening under a panel to match what already renders. Any later visible change
  (e.g. a fixed-width center for the palette) needs signoff.
- Budgets: n/a (no perf-sensitive path; layout is already per-frame pure compute).
- Gates: `bash scripts/check.sh` green at every phase.
- Oracles:
  - `WidgetStack` resolution (left/right/center, collapse, overflow) — hand-
    computed unit tests in `test_widget`, incl. the `packRight` order and the
    stop-at-first-non-fit collapse edge and the second-center fail-loud.
  - Footer port — `ui_layout` + `test_hit_test` green WITHOUT regeneration.
  - Header port — `ui_layout` + `test_render` + `test_hit_test` green without
    regeneration; `inputLineCaret` unchanged.
  - Footer / prompt single-source — `test_prompt_status` + `test_render` prompt
    cases + `PromptViewState` round-trip green without regeneration; the editor-
    shrink assertion (document top unmoved, height reduced by the prompt rows);
    the single-source assertion that the shell reservation, the rendered controls,
    and the a11y node all consume the one full-width `view.prompt` across panel /
    distraction-free / notice-row combinations.
  - Focus model — existing focus-stack/keymap-context tests green; a new test
    asserting the palette resolves to the header and find/replace to the footer,
    with focus coupled to the single active prompt (at-most-one is guaranteed by
    the single `PromptSurface`, so there is nothing to fail-loud on).

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| A | Add the `WidgetStack` primitive (`packLeft`/`packRight`/`center` + `StackItem{id, content, desired, rank, keep, overflow, sigil}`), resolving to relative placements over an extent; the left group's rank collapse delegates to the existing `fitRow` engine and `ScrollTail` overflow to `layoutTextInput`. No callers yet. | `include/ssg/Widget.h`, `src/Widget.cpp`, `tests/test_widget.cpp` | `test_widget`: hand-computed left/right/center, packRight order, stop-at-first-non-fit collapse, right-group clamp-truncate/drop, Fixed-center clamps-to-gap when narrow, second-center fail-loud, truncate vs scrollTail | solver purity; fail-loud; collapse == fitRow |
| B | Port the footer status line (left status fields + right actions/hint group) to one `WidgetStack`, replacing the `addFields`+`packEnd` pair and the reverse-emit arithmetic in `computeShellLayout`; `WidgetStack`'s right group now delegates to `packEnd` (one source of the right-fill rule). DONE, green. | `src/ShellState.cpp`, `src/Widget.cpp` | `ui_layout` + `test_hit_test` green WITHOUT regeneration | non-overlap; node emission order; collapse rule |
| C | Port the header STATUS FIELDS to a `WidgetStack` left collapse group over `width - inputLineReservation` (the fixed floor), replacing `addFields` (deleted; also removes its now-dead `layoutRow` helper). The input line + ghost KEEP the `layoutTextInput` seam — phase E bundles them into a `layoutInputLine` widget seam (not a stack item). DONE, green. | `src/ShellState.cpp`, `src/Widget.cpp`, `include/ssg/Widget.h`, `tests/test_widget.cpp` | `ui_layout` + `test_render` + `test_hit_test` green without regeneration; `inputLineCaret` unchanged | fields-independent-of-input-line; non-overlap |
| D | Single-source the prompt rect: make `view.prompt` the FULL-WIDTH bottom strip and have `promptStatusView` CONSUME it (passed from the shell view) instead of recomputing from dimensions, so the shell reservation, the a11y node/hit region, and the rendered controls are one rect. DONE, green. (The `FooterMode` type + variable-height footer node + status-line suppression were evaluated in phase E and dropped as speculative — deferred to a future Lua-motivated spec.) | `src/ShellState.cpp`, `src/runtime/snapshot.cpp`, `src/runtime/editor_runtime_internal.h`, `tests/test_ui_layout.cpp`, `tests/runtime/test_runtime_editing.cpp` | runtime oracle: `shell.prompt == promptStatus.prompt->rect` AND full-width across panel-off/on; `test_render` prompt cases + `test_prompt_status` + `PromptViewState` round-trip green without regeneration; editor content rect unchanged (top unmoved, x/width unchanged) | single-source prompt rect; prompt shrinks from bottom; solveLayout purity |
| E | Make prompt focus explicit WITHOUT a runtime mode-stack (the single `PromptSurface` already guarantees at-most-one-active): add `promptFocusRegion(PromptKind) -> {Header, Footer}` as the one source of prompt anchoring and route the header-input-line ANCHORING decision (`inputLineActive`) through it (picker-machinery sites stay gated on `kind == Palette`); bundle the picker query + ghost into a `layoutInputLine` widget seam (completes the phase-C input-line deferral). `FooterMode`/variable footer/status suppression dropped as speculative (deferred to a future Lua-motivated spec). DONE, green. | `include/ssg/PromptSurface.h`, `include/ssg/Widget.h`, `src/Widget.cpp`, `src/ShellState.cpp`, `src/runtime/snapshot.cpp`, `tests/test_widget.cpp`, `tests/runtime/test_runtime_editing.cpp` | runtime oracle: focus↔active-prompt coupling + palette resolves to Header (input line present, no footer reservation) / find to Footer (reservation, no input line); `layoutInputLine` hand-computed cases; all goldens green without regeneration | one active prompt (by construction); keyboard routing unchanged; fields-independent-of-input-line |
| F | Document the stack/region/focus model + the `init.lua` composition seam it enables (design only; no binding), in the spec Design note and the forward-looking note in `doc/config.md`. DONE. | `doc/spec-chrome-stacks.md`, `doc/config.md` | `test_config_doc` green (no new command/API claimed) | no wire/API change |

## Rationale (optional, skippable)

The widget-chrome work (spec-widget-chrome, done) funneled chrome through closed
primitive functions but left THREE row-placement rules and a prompt reservation
derived in two places. This spec collapses the placement rules into one
`WidgetStack` and single-sources the prompt reservation, so the header+footer
becomes "regions holding rows of stacked widgets" (with a data-described footer
MODE STACK as a deferred design seam, not shipped). That uniformity is the
precondition for a future `init.lua` that hands in a tree of stack/region
descriptors — the same data-composition shape `theme.set`/`style.define` already
use — without any of the three special-case placement paths a Lua author would
otherwise have to model.
Keeping every porting phase golden-verified makes the refactor provable rather
than inspected.
