# spec-chrome-stacks

Status: draft (spec only; not yet implemented)

## Goals

Generalize header/footer chrome from today's three ad-hoc placement mechanisms
(`fitRow`, `packEnd`, and manual input-line x-arithmetic) into ONE composable
row primitive — a `WidgetStack` built by `packLeft`/`packRight`/`center` — and
promote the footer (and the header's prompt) into a **region with modes**, so a
multi-row surface like find/replace is a footer MODE pushed over the default
status line rather than a separately-computed bottom-of-editor reservation.
After this change:

- One row is composed as `stack.packLeft(a).packRight(z).packLeft(b)...`,
  yielding `abc…xyz`: left items pack from the leading edge, right items from the
  trailing edge, an optional single `center` slot sits between them at an
  explicit or flexible width.
- Which items survive a crowded row is a STACK-level policy (`collapse` by rank);
  how a surviving item's content fits its own cell is a WIDGET-level overflow
  policy (`truncate` or `scrollTail`). The two are orthogonal, not competing.
- The footer is a variable-height region with a **mode stack**: the default mode
  is the 1-row status stack; find is a 2-row mode; replace is a 3-row mode.
  Opening find PUSHES a footer mode; closing POPS it. `computePromptLayout`'s
  bottom-reservation math folds into "a footer mode is N rows, each row a
  `WidgetStack` or a solved sub-tree."
- A prompt is a **focusable region mode**: exactly one prompt mode is active at a
  time, anchored to a region (header for the palette, footer for find/replace),
  and keyboard focus follows the topmost prompt mode. This makes today's
  implicit "keyboard always goes to the prompt" explicit in the layout model.

Non-goal (explicit): exposing stacks/modes to `init.lua` (a later spec; this one
only keeps the seam open); moving the palette out of the header; adding
`SemanticRole`s or `PromptKind`s; changing on-screen output in the porting
phases (the existing goldens are the oracle — byte-identical without
regeneration).

## Design

### WidgetStack: one row primitive

`WidgetStack` replaces `fitRow` + `packEnd` + `addFields` + the input-line
arithmetic with a single builder that lays out one row of the header or footer.
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

### Item, and the two overflow levels

A stack item is `{ id, WidgetContent, StackFit, Overflow }`:

- `StackFit` — the LEFT-group membership policy when it cannot fit every item:
  `Collapse{rank}` (drop the whole item, lowest rank first, survivors intact —
  today's header/footer status FIELDS) or `Keep` (never dropped — the input
  line, the primary content). The fit pass reproduces `fitRow`'s EXACT rule for
  the collapse set: stable-sort by rank, forward-scan appending while it fits,
  STOP at the first non-fit (NOT drop-until-fits — that picks a different
  retained subset). `StackFit` governs the LEFT group only; the RIGHT group's
  membership is the positional clamp-truncate/drop rule above (today's footer
  actions + hint), and the center slot is never dropped — its `Fixed` width
  clamps to the available gap, and only a second `center` call is fail-loud.
- `Overflow` — the WIDGET-level policy for a surviving item whose CONTENT
  exceeds its cell: `Truncate` (clip to the cell; today's footer actions/hint
  render, clipped by the renderer to the rect) or `ScrollTail{sigil, budget}`
  (pin a leading sigil, show the grapheme tail of the value; today's input line
  via `layoutTextInput`). A `Fixed`-content field that always fits uses neither.

Collapse decides WHICH items are present; Overflow decides how a present item's
text fills the cell it received. A narrow header collapses the branch field out
(whole), and independently scroll-tails the palette query within its own cell.

### Output: still the existing projections

A resolved `WidgetStack` produces the SAME `(id, kind, rect, role, content)`
`AccessibilityNode`s the procedural code emits today (one node per item, or the
per-role-homogeneous-span rule from spec-widget-chrome for a split-role item
like the input line + ghost). The `ui_layout`/`test_hit_test`/`test_render`
goldens verify each port span-for-span; no wire type changes in the porting
phases.

### Footer as a region with a mode stack

Today `buildShellTree` gives the footer an `exact(footerHeight)` (=1) region,
and find/replace are rendered by a SEPARATE `computePromptLayout` into a rect
carved from the bottom of the editor (`reservedPromptRows`, promptRowCount:
Find=2 Replace=3). This spec unifies them: the footer region becomes
**variable-height**, driven by the active footer mode's row count.

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

Single-source prompt rect (contract): today the prompt geometry is derived in
TWO places — the shell layout's `reservedPromptRows`/`view.prompt` reservation
(ShellState.cpp) and the prompt-status reservation passed to
`computePromptLayout` (runtime/snapshot.cpp). Phase D MUST make both consume the
SAME resolved footer-mode rect: the active `FooterMode`'s solved region rect is
the one source, and `computePromptLayout` is handed that rect rather than a
second independently-computed reservation. Otherwise neutrality can regress under
panel + distraction-free + notice-row combinations while a prompt-only test still
passes. This is the load-bearing part of the fold, and its oracle is an assertion
that the prompt rect the renderer sees equals the footer-mode rect across those
combinations (not just the bare-prompt case).

Multi-row footer modes are preserved deliberately: find/replace stay 2/3 rows
even though find alone could be one line, because a future richer footer surface
(find+replace together, results preview) wants the multi-row affordance.

### Prompt as a focusable region mode

`FocusTarget{Editor, Panel, Prompt}` and the focus stack already route keyboard
unambiguously to an active prompt. This spec makes the ANCHORING explicit: a
prompt is a region mode with `focusable = true` and a `promptKind`. The
invariant "at most one prompt mode is active across all regions" is enforced at
the mode-stack level (pushing a prompt mode while another is active is a spec
error), and `FocusTarget::Prompt` resolves to whichever region hosts the active
prompt mode. The palette is a HEADER-anchored prompt mode (kept where it is — its
results narrow to the top of the buffer just below the input); find/replace are
FOOTER-anchored prompt modes. No new `FocusTarget` value; the improvement is that
the layout model, not just a runtime enum, records where the focused prompt lives.

## Invariants

- Server owns layout/rendering; the client is a dumb renderer. Stacks and modes
  live server-side; only their projected `AccessibilityNode`/prompt-view
  artifacts cross the wire.
- `solveLayout` stays PURE and FAILS LOUDLY. The stack fit pass must guarantee
  fit before any solve; a second `center`, or a prompt-mode push over an active
  prompt, is a fail-loud spec error, not a clamp.
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
- The prompt geometry has a SINGLE source: the active footer-mode rect. The
  shell reservation and the prompt-status reservation both consume it; neither
  recomputes a prompt rect independently.
- Adding a `SemanticRole` or a `Style` glyph key requires its existing wiring
  sites; this spec adds none.

## Considerations

- **Behavior-preserving ports vs. the fold.** Phases B/C (port footer, then
  header, onto `WidgetStack`) are strictly behavior-preserving — goldens are the
  oracle, no regeneration. Phase D (footer modes + fold the prompt reservation
  in) is ALSO intended to be observably neutral, but it moves the prompt from an
  editor reservation to a footer region; the risk is a one-row off-by-one in the
  editor-shrink or the caret row. The prompt-status and render goldens pin it.
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
- **Focus regression** (keyboard to the wrong surface). Mitigation: the
  at-most-one-active-prompt invariant is a fail-loud assertion; existing
  focus-stack tests must stay green.
- **Scope creep into Lua.** Mitigation: the Lua binding is explicitly out of
  scope; phase F documents the seam only.

## Acceptance (Definition of Done)

- Observable: no visible change through phase D (goldens byte-identical without
  regeneration); find/replace look and behave exactly as today, now driven by
  footer modes. Any later visible change (e.g. a fixed-width center for the
  palette) needs signoff.
- Budgets: n/a (no perf-sensitive path; layout is already per-frame pure compute).
- Gates: `bash scripts/check.sh` green at every phase.
- Oracles:
  - `WidgetStack` resolution (left/right/center, collapse, overflow) — hand-
    computed unit tests in `test_widget`, incl. the `packRight` order and the
    stop-at-first-non-fit collapse edge and the second-center fail-loud.
  - Footer port — `ui_layout` + `test_hit_test` green WITHOUT regeneration.
  - Header port — `ui_layout` + `test_render` + `test_hit_test` green without
    regeneration; `inputLineCaret` unchanged.
  - Footer modes / prompt fold — `test_prompt_status` + `test_render` prompt
    cases + `PromptViewState` round-trip green without regeneration; the editor-
    shrink assertion (document top unmoved, height reduced by mode rows); the
    single-source assertion that the prompt rect the renderer sees equals the
    active footer-mode rect across panel / distraction-free / notice-row
    combinations.
  - Focus model — existing focus-stack/keymap-context tests green; a new test
    asserting at-most-one-active-prompt is fail-loud.

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| A | Add the `WidgetStack` primitive (`packLeft`/`packRight`/`center` + `StackItem{StackFit: Collapse/Keep, Overflow: Truncate/ScrollTail/None}`), resolving to relative placements over an extent; implement `Collapse` by delegating to the existing `fitRow` engine and `ScrollTail` to `layoutTextInput`. No callers yet. | `include/ssg/Widget.h`, `src/Widget.cpp`, `tests/test_widget.cpp` | `test_widget`: hand-computed left/right/center, packRight order, stop-at-first-non-fit collapse, right-group clamp-truncate/drop, Fixed-center clamps-to-gap when narrow, second-center fail-loud, truncate vs scrollTail | solver purity; fail-loud; collapse == fitRow |
| B | Port the footer status line (left status fields + right actions/hint group) to one `WidgetStack`, replacing the `addFields`+`packEnd` pair and the reverse-emit arithmetic in `computeShellLayout`. | `src/ShellState.cpp` | `ui_layout` + `test_hit_test` green WITHOUT regeneration | non-overlap; node emission order; collapse rule |
| C | Port the header (status fields + input line + ghost) to a `WidgetStack`, replacing `addFields` + the input-line x-advance; input line is a `Keep`+`ScrollTail` right/center item so fields never reflow as the query grows. | `src/ShellState.cpp` | `ui_layout` + `test_render` + `test_hit_test` green without regeneration; `inputLineCaret` unchanged | fields-independent-of-input-line; non-overlap |
| D | Introduce the footer region **mode stack**: a `FooterMode{rows, focusable, promptKind?}`, default status mode + find/replace prompt modes; derive footer region height from the active mode and re-host `computePromptLayout`'s solve tree in the footer region, folding out the editor `reservedPromptRows` reservation (bottom-shrink identical). Both the shell reservation and the prompt-status reservation MUST consume the one resolved footer-mode rect (single-source prompt rect). | `src/ShellState.cpp`, `src/PromptSurface.cpp`, `include/ssg/ShellState.h`, `src/runtime/snapshot.cpp` | `test_prompt_status` + `test_render` prompt cases + `PromptViewState` round-trip green without regeneration; editor-shrink assertion (top unmoved); **prompt-rect == footer-mode-rect assertion across panel/distraction-free/notice combinations** | prompt shrinks from bottom; solveLayout purity; single-source prompt rect |
| E | Make prompt focus explicit: a prompt is a `focusable` region mode with `promptKind`; enforce at-most-one-active-prompt at the mode-stack level; resolve `FocusTarget::Prompt` to the hosting region. Palette = header mode, find/replace = footer modes. No new `FocusTarget`. | `src/ShellState.cpp`, `include/ssg/focus.h` (doc), `src/runtime/*` | focus-stack + keymap-context tests green; new fail-loud at-most-one-prompt test | one active prompt; keyboard routing unchanged |
| F | Document the stack/mode/focus model + the `init.lua` composition seam it enables (design only; no binding). | `doc/spec-chrome-stacks.md`, `doc/config.md` | `test_config_doc` green (no new command/API claimed) | no wire/API change |

## Rationale (optional, skippable)

The widget-chrome work (spec-widget-chrome, done) funneled chrome through closed
primitive functions but left THREE row-placement rules and a prompt surface
computed outside the shell tree. This spec collapses the placement rules into one
`WidgetStack` and pulls the prompt into the same region/mode model as the rest of
the chrome, so the entire header+footer becomes "regions holding modes holding
rows of stacked widgets." That uniformity is the precondition for a future
`init.lua` that hands in a tree of stack/mode descriptors — the same data-
composition shape `theme.set`/`style.define` already use — without any of the
three special-case placement paths a Lua author would otherwise have to model.
Keeping every porting phase golden-verified makes the refactor provable rather
than inspected.
