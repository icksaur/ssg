# spec-style

Companion to `doc/spec-color.md`. Color owns *which hue* a cell is; style owns
*what shape the UI is* -- the dimensions of its regions and the glyphs its
chrome is drawn with. The two are deliberately separate documents because they
fail differently: a color mistake makes something hard to read, a style mistake
makes it the wrong size or drawn with the wrong character.

Status: draft (spec review pending). Nothing here is implemented.

## Goals

One place that decides how wide the panel is, how tall the footer is, and which
character draws a scrollbar thumb -- so those decisions are stated once, are
consistent across every surface that has them, and can be changed without
hunting through the renderer.

After this work: a scrollbar thumb of any size renders through one rule; a tree
indicator, a dirty-tab marker and an input-line sigil come from one table; and
the panel's width, the gutter's width and the header's height are read from one
object rather than from constants scattered across two files.

## What style is, and is not

**Style is** element dimensions and chrome glyphs.

**Style is not** color. `doc/spec-color.md` owns palettes, `SemanticRole`s and
background washes, and remains the sole authority. A style object never names a
color; it names a glyph and a size, and the renderer resolves the color for that
glyph through the existing role system exactly as it does today.

**Style is not** layout policy. `computeShellLayout` decides that the panel is
dropped before the editor is squeezed, that fields are laid out before the input
line, and that a too-small viewport is rejected. Style supplies the *numbers*
that policy uses; it does not decide the policy. This boundary is what keeps
style a data object rather than a second layout engine.

## Current state (verified in code, not from docs)

**Chrome glyphs.** Ten, all originally hardcoded at their point of use. The
inventory was wrong repeatedly by recall -- first missing the ellipsis and the
live-diff prefix, then the prompt toggle markers, and finally the replacement
glyph and the prompt separator, which a whitespace-tolerant sweep for
`\xNN`-escaped and composed literals found only after Y2 shipped. It is now
rebuilt from that sweep rather than memory:

- `|` scrollbar track, `#` scrollbar thumb -- `src/Renderer.cpp:245`
- `" "` empty gutter when nothing scrolls -- `src/Renderer.cpp:238`
- `…` (U+2026) truncation marker -- `src/Renderer.cpp:158`
- `▾` / `▸` tree expanded/collapsed -- `src/Renderer.cpp:284`
- `[x] ` / `[ ] ` prompt toggle markers -- `src/Renderer.cpp:749`
- U+FFFD replacement glyph for control/invalid bytes -- `src/Renderer.cpp`,
  duplicated at FOUR sites (148, 486, 588, 634) before the sweep
- `": "` prompt label/value separator -- `src/Renderer.cpp:752`, duplicated at
  the cursor-width site (780), the same label/width drift the sigil had
- `" *"` dirty-tab marker -- `src/ShellState.cpp:553`
- `"D "` live-diff tab prefix -- `src/runtime/snapshot.cpp:16`
- `"> "` input-line sigil -- `src/ShellState.cpp:471`, and separately as the
  width constant `kInputLineSigilWidth = 2`

Deliberately NOT style, and left in place: the `"leader:"` hint prefix
(`src/runtime/snapshot.cpp`) is functional label text, not a decorative glyph;
`" "` blanking fills and accessibility labels and error strings are content, not
chrome. The line between the two is "would a re-theme want to change it" -- a
substitution glyph yes, a leader-mode announcement no.

Several are produced OUTSIDE the renderer -- in shell layout and in snapshot
composition -- so no single-file scan could ever have been complete. That, more
than any specific miss, is why the sweep (below) reads all three translation
units.

**Dimensions.** Named constants in one anonymous namespace
(`src/ShellState.cpp:12-29`): `kMinimumColumns` 20, `kMinimumRows` 4,
`kPanelTargetWidth` 24, `kPanelMinimumWidth` 12, `kEditorMinimumWidth` 20,
`kInputLineSeparator` 1, `kInputLineSigilWidth` 2, `kInputLineQueryBudget` 8,
`kInputLineReservation` 11.

Unnamed inline literals, which are most of what this work should capture:

- scrollbar gutter width `1`
- header, tab-bar and footer heights `1` each
- tree indent `2 * depth` spaces
- **tab horizontal padding `+ 2`** -- `src/ShellState.cpp:556`
- **footer action padding `+ 2`** -- `src/ShellState.cpp:503`

The two padding literals are the "header buttons, footer buttons" the request
named: there is no button *glyph*, but a footer action and a tab are each sized
as label-plus-padding, and that padding is the styling they have.

**No test pins any chrome glyph.** Searching the suite for the literals above as
rendered output finds nothing; the render fixtures encode cell geometry and
grapheme segmentation, not chrome characters. Changing a glyph today is silent.
That absence is the strongest argument for this work: the abstraction is worth
little without oracles, and the oracles are worth adding even before it.

## The scrollbar visibility defect (color, not style)

Investigated because it prompted this spec, and reported here so it is not
lost -- but it belongs to `doc/spec-color.md`, and this spec does not fix it.

`SemanticRole::ScrollbarTrack` resolves to palette index 2
(`src/DefaultTheme.cpp:82`). `SemanticRole::TreeBackground` resolves to palette
index **2 as well** (`src/DefaultTheme.cpp:67`). The panel's gutter therefore
paints `|` in exactly the colour of the surface behind it:

- Panel track on its background: **1.00:1** -- identical colour, invisible
- Panel thumb on its background: 1.97:1
- Editor track on its background: 1.57:1
- Editor thumb on its background: 3.08:1

WCAG's minimum for a non-text UI component is 3:1, so only the editor thumb
clears it and the panel track is not merely dim but literally undrawable. The
track is not missing and the panel does not use a different style; it is painted
in the background colour.

The fix is a role reassignment in the theme plus an invariant that a chrome role
must be distinguishable from every background it is drawn on -- both squarely
`spec-color.md`'s. `test_theme.cpp` already enforces that "co-visible role
pairs" sit on distinct indices; `ScrollbarTrack`/`TreeBackground` is evidently
not in that set and should be.

## Design

### One snapshot, one resolver

`StyleSnapshot` is a plain data object holding the glyphs and dimensions listed
below. `Style` is the resolver over it, mirroring how `ThemeSnapshot` carries
values and `semanticIndex()` reads them. Two types rather than one, for the same
reason the theme has two: the snapshot is copyable, comparable and wire-encodable
data, and the resolver holds the rules that turn it into what the renderer draws.

The renderer asks the resolver for a glyph and a colour role, then paints. It
never contains a literal.

### Scrollbar thumbs at size 1, 2 and N

The one piece of genuine rendering logic style owns, and the reason a bare glyph
table is not enough.

A thumb may be one row, two rows, or many. If a style wants distinguishable end
caps -- a rounded or half-block top and bottom -- then a one-row thumb has no
room for two caps and a two-row thumb is all caps with no body. So the rule
cannot be "top, then body, then bottom" alone.

`ScrollbarGlyphs` therefore carries four entries: `single`, `top`, `body`,
`bottom`. The resolver's rule, stated once and completely:

- size 0 -> nothing is a thumb; the whole gutter is the empty-gutter glyph.
  **Note the metrics do not report it this way.** `scrollbarMetricsImpl` returns
  `thumbSize == viewportRows` when the content fits -- a full-length thumb, not
  an absent one -- and marks the case with `maximumFirstRow == 0`
  (`src/Viewport.cpp:339-341`). An earlier draft of this spec asserted the
  opposite, and routing the renderer through Style on that assumption turned
  every fits-in-view gutter solid. `paintScrollGutter` therefore translates the
  metrics' convention into Style's at the boundary; Style keeps the clearer
  contract rather than inheriting the confusing one.
- size 1 -> `single`
- size 2 -> `top`, `bottom`
- size N>2 -> `top`, then N-2 x `body`, then `bottom`
- size > track height -> clamped to the track height, then the rule above.
  Cannot occur today (`thumbSize <= viewportRows` by construction) but the
  resolver clamps rather than trusting it, because a caller supplying its own
  metrics is exactly how that assumption would break.
- track height 1 with a thumb -> the size-1 case; a one-row track can only ever
  show `single`.

A style that wants a uniform thumb sets all four to the same glyph, which is
today's behaviour and stays the default. A style that wants caps gets correct
degradation at the small sizes for free. The track is one glyph, and the
"gutter" -- the reserved column when there is nothing to scroll -- is its own
entry rather than a hardcoded space, so an empty gutter can be styled (or made
deliberately blank, as now).

This rule is what makes the abstraction earn its place: the size-1 and size-2
cases are exactly what an implementer gets wrong, and stating them here means
they are decided once instead of per surface.

### Dimensions

`StyleSnapshot` carries the named constants above, plus the six currently
inline: scrollbar gutter width, header height, tab-bar height, footer height,
tree indent per depth, and the tab / footer-action horizontal padding. Naming
the inline ones is most of the value -- `kPanelTargetWidth` is already
discoverable, an inline `+ 2` inside a `std::min` is not.

The two padding literals are what "header buttons, footer buttons" amounts to
today: a tab and a footer action are each sized as
`label width + padding`, clamped to the space left. There is no button glyph to
abstract, so the padding is the whole of their style, and it should be one value
shared by both rather than the same `+ 2` written twice.

Dimensions stay `int` cells. There is no unit system, no scaling factor and no
percentage: this is a terminal, the unit is a cell, and inventing a unit type
would be the over-thinking the request warns against.

### Dynamic restyling: possible, via a proven path

The architecture already supports exactly this, and `ThemeSnapshot` is the
worked example:

- lives as a field on `EditorRuntime::Impl` (`theme`)
- is published as a snapshot section (`SessionSnapshot::sections().theme`)
- has a delta type (`ThemeSectionDelta`) so a change repaints
- is wire-encoded (`toValue`/`decodePresent` for `ThemeSnapshot`,
  `src/Protocol.cpp:4639`)
- is mutated at runtime by Lua commands (`theme.define`, `theme.background`)

`Renderer::render(SessionSnapshot const&)` takes the whole snapshot and reads
the theme from it, so a style section arriving the same way needs no renderer
signature change. A `StyleSnapshot` following that path is therefore dynamic by
construction, with no architectural obstacle found.

**But the first pass does not take that path.** Step 1 makes style a
compiled-in default with no snapshot section, no delta, no wire encoding and no
command -- because each of those is a real cost (a protocol cascade, a delta
derivation, a command-catalog cascade across six sites) and none of them is
needed to remove the hardcoded literals. Dynamism is step 3, wired only once the
abstraction has proven it holds the right things. The path is recorded here so
that step is mechanical rather than a redesign.

## Invariants

- **Y1** Chrome glyphs live only in the style tables, kept true by *routing
  tests* rather than a literal-blocklist guard. The routing tests restyle a
  `Renderer` (and a `ShellLayoutRequest`) and require the painted output to
  follow; a painter that reintroduces a hardcoded glyph makes restyling stop
  changing that cell, so the test fails -- for the right reason, at any glyph,
  without anyone enumerating the forbidden literals. A one-time sweep (the Y3
  step) established that the current painters hold no stray glyph; the routing
  tests keep it that way. This replaces an earlier plan for a grep-guard with an
  exemption list, which would have been a blocklist of the glyphs already known
  -- codifying exactly the recall failure that made the inventory wrong four
  times, and giving false confidence against the next new glyph.
- **Y2** No layout dimension literal outside `StyleSnapshot`, except where a
  value is structural rather than stylistic (see Considerations).
- **Y3** Style never names a colour, and the theme never names a glyph or a
  dimension. The two documents stay separable.
- **Y4** A scrollbar thumb of any size renders through the single size rule
  above; no surface implements its own.
- **Y5** Layout remains a pure function of its request and shell state. Style is
  an input to it, not a source of hidden state.

## Considerations

- **Which numbers are style and which are structural.** `kMinimumColumns` 20 and
  `kMinimumRows` 4 are the floor below which layout is *rejected*; that is a
  correctness boundary, not a taste decision, and a style that could raise or
  lower it would let a config make the editor unusable. Proposal: they stay
  outside style. `kEditorMinimumWidth` is the same shape. This distinction needs
  deciding at review time rather than assumed.
- **The sigil's glyph and its width are two facts today** (`"> "` at
  `ShellState.cpp:471`, `kInputLineSigilWidth = 2`). They must become one, or
  they will drift -- a two-cell sigil replaced by a one-cell glyph would
  misalign the whole input line. Width should be derived from the glyph, not
  stored beside it. Note the derivation is *display width*, not byte length: the
  tree indicators are already multi-byte, and a wide glyph would occupy two
  cells. Y2 makes this an executable check rather than a note, because a
  derivation stated only in prose is how the pair drifted in the first place.
- **Recall does not converge; a sweep does.** The inventory grew six -> seven ->
  eight -> ten, each correction from a different method (review, routing, a
  whitespace-tolerant literal sweep), and several glyphs are produced outside
  the renderer. This is the case against a literal-blocklist guard: it would
  have been seeded from whatever count was current and would silently pass the
  next new glyph. The completeness check is the one-time sweep; the standing net
  is the routing tests, which fail on any unrouted glyph without naming it.
- **Glyphs must be terminal-safe.** `▾`/`▸` already assume a UTF-8 terminal.
  Whether the default set should be ASCII-only, with box-drawing as an opt-in
  style, is a real question this spec does not answer. It matters more once
  styles are user-supplied.
- **Existing render fixtures** encode cell contents. Changing a default glyph
  would change them; keeping defaults byte-identical in step 1 avoids that
  entirely and keeps the refactor honest.
- **Header and footer heights are 1 and the layout assumes it** in places beyond
  the constant's use. Making them style-supplied is not the same as making them
  work at other values; step 1 should name them without promising they are free
  parameters.

## Risks and Mitigations

- A style object that quietly becomes a second layout engine. Mitigation: it
  holds data only; the resolver's sole rule is the thumb-size one, and anything
  else that wants to live there is a signal to stop.
- Guard tests that are too broad and fail on unrelated string literals.
  Mitigation: scan only the renderer and shell-layout translation units, and
  match the specific known glyphs rather than "any non-ASCII".
- Naming a dimension implies it is adjustable when the surrounding code assumes
  its current value. Mitigation: the spec says which are adjustable and which are
  merely named; the tests assert only what is true.

## Acceptance (Definition of Done)

- Observable: the editor renders byte-identically to today after step 1 -- this
  refactor's success is that nothing changes on screen. Steps 2-3 change
  appearance and need signoff.
- Budgets: no new per-frame allocation in the render path; style is read, not
  built, per frame.
- Gates: `bash scripts/check.sh` and `scripts/check.sh push` green, zero
  warnings.
- Oracles: per plan step below.

## Plan

| # | Step | Files | Oracle |
|---|------|-------|--------|
| Y0 | **Superseded** -- merged into Y1. Originally: pin today's glyphs with golden assertions before refactoring | -- | see Status |
| Y1 | **Delivered.** `Style` with the glyph tables and dimensions, defaults byte-identical to today. No callers yet | `include/ssg/Style.h`, `src/Style.cpp`, `tests/test_style.cpp`, `cmake/components/style.cmake` | configured-behavior unit tests: a style built with distinctive glyphs must render exactly those. Plus a size property (resolved thumb length == requested size, over all heights and offsets) and the derived sigil width. Every behavior perturbation-verified |
| Y2 | **Delivered.** Route the renderer, shell layout and snapshot composition through it | `src/Renderer.cpp`, `src/ShellState.cpp`, `src/runtime/snapshot.cpp`, `include/ssg/Renderer.h`, `include/ssg/ShellState.h`, `src/runtime/editor_runtime_internal.h` | the existing suite passes unchanged (the refactor is invisible), PLUS two routing proofs -- restyling a `Renderer` changes the painted chrome, and a `ShellLayoutRequest`'s style changes panel width, the minimum viewport and the sigil. Both perturbation-verified against a regression to literals |
| Y3 | **Delivered as a sweep, not a guard.** One-time sweep of the three chrome translation units for any glyph literal reaching the grid; route the stragglers into `Style`; then rely on the Y2 routing tests as the standing net | `src/Renderer.cpp`, `include/ssg/Style.h`, `tests/test_render.cpp`, `tests/test_style.cpp` | the sweep found two more glyphs (U+FFFD replacement x4, prompt `": "` separator x2), now routed; a new routing proof restyles `unrenderable` and requires the document to follow (perturbation-verified). No grep-guard: a blocklist would only catch the glyphs already known |
| Y4 | (Optional, after review) Publish style as a snapshot section with delta, wire codec and a `style.define` command, following the theme's path | `include/ssg/session_snapshot.h`, `src/Protocol.cpp`, runtime, command catalog | round-trip through the codec; a `style.define` at runtime repaints with the new glyph |

## Status

**Y1, Y2 and Y3 delivered.** `Style` exists, the renderer, shell layout and
snapshot composition all draw from it, and a one-time sweep confirmed the
painters hold no stray glyph. 89 tests green (91 push).

**The glyph inventory was wrong four times, which is the whole argument against
a grep-guard.** Six by first recall; seven at review (ellipsis, live-diff
prefix); eight while routing (the prompt toggles `[x] `/`[ ] `, missed because a
targeted grep pattern was malformed); and finally ten when a whitespace-tolerant
sweep for escaped and composed literals found the U+FFFD replacement glyph
(duplicated four times in `paintDocument`) and the prompt `": "` separator
(duplicated at its cursor-width site). A blocklist guard would have been seeded
from whichever count was current when it was written, and would have waved the
next new glyph straight through. The sweep -- read every `\xNN` and composed
display literal in all three units, classify each as chrome or content -- is
what actually reached the bottom, and it is a thing you do once, not a test you
run forever.

**What keeps it true instead:** the Y2 routing tests. They restyle and require
the screen to follow, so a painter that reintroduces a literal fails them at any
glyph without enumerating forbidden ones. The replacement glyph got its own such
proof; regressing all its uses to the literal fails it (perturbation-verified),
while regressing a single site does not -- a reminder that a routing test covers
the path its fixture traverses, and the four replacement sites are structurally
identical one-liners rather than four independent risks.

**A wrong assumption about scrollbar metrics broke 42 assertions.** The spec
claimed `thumbSize == 0` when content fits; it is actually `viewportRows`, with
`maximumFirstRow == 0` as the real signal. Routing on the wrong assumption made
every fits-in-view gutter solid, and the existing tests caught it immediately.
The fix reconciles the two conventions in `paintScrollGutter` rather than
teaching `Style` the confusing one. Corrected above.

**"Existing tests pass" was deliberately not accepted as proof of routing.**
Because the defaults reproduce the old appearance exactly, an implementation
that ignored `Style` entirely would also pass. Two dedicated tests therefore
restyle and require the output to follow, and all three routing reversions
(renderer glyphs, panel width, sigil) were perturbation-verified to fail them.

**Ownership.** `EditorRuntime::Impl` holds the one `Style`, beside the
`ThemeSnapshot` it resembles, and copies it into each `ShellLayoutRequest`.
`Renderer` holds its own assignable `Style` because it takes only a snapshot;
unifying the two is Y4's job, when style becomes a published section. Until
then a client that restyles the renderer must restyle the runtime to match --
recorded here because nothing enforces it yet.

**Y0 was dropped as originally written, deliberately.** The plan called for
golden assertions pinning today's literal glyphs before refactoring. That is the
wrong test for this work: it would freeze the very values the abstraction exists
to make configurable, and every future style change would have to edit the tests
that supposedly guard it -- rigidity bought at the price of the goal. What
matters is not that the thumb is `#` today; it is that a style configured with a
given glyph renders *that* glyph.

So `tests/test_style.cpp` constructs styles with deliberately distinctive
multi-character glyphs and asserts the output follows the configuration. An
implementation that ignored its configuration and returned hardcoded `|`/`#`
fails every case. The size rule is additionally pinned by a property -- the
resolved thumb always covers exactly its requested rows, across all heights and
offsets -- which catches cap-rule errors at sizes no hand case enumerates.

All behaviors were perturbation-verified (size-1 cap selection, both clamps, the
size-0 gutter contract, the bottom cap, and the derived sigil width); each
perturbation produced failures, so none of these assertions is decorative.

The sigil width is measured through `GraphemeLayout`, not declared: setting a
fullwidth sigil yields 2, a narrow one yields 1, with no second constant to keep
in step. This retires the `"> "` / `kInputLineSigilWidth` pair that Considerations
flagged as certain to drift.

**Review findings folded.** The code review returned one MUST and two SHOULDs,
all verified in code before folding:

- Layout budgeted in cells but measured labels in **bytes**
  (`query.size()`, the ghost, the tab title, the footer action). Harmless while
  every glyph was ASCII; making glyphs configurable made it reachable, so a
  fullwidth sigil or a non-ASCII tab title would have mis-sized its region.
  All four now measure through `displayCells`, which is `GraphemeLayout`.
- `headerHeight`, `footerHeight`, `tabBarHeight` and `scrollbarGutterWidth` were
  **exposed but ignored** -- the worst kind of API, one that accepts a value and
  drops it. They are now wired through the header, footer, panel, tab bar, tab
  hit rects, pane content and both scrollbar gutters (including `layoutPanes`,
  which held its own copy of the gutter literal). Defaults are unchanged, so the
  shipped geometry is identical.

Both fixes are pinned by tests and perturbation-verified.

## Rationale

The request asked for scrollbars, tabs, tree, header and footer to share a style
abstraction. Investigation showed the scrollbars already share their *painter*
(`paintScrollGutter`, one function, three callers) and their glyphs are
identical -- so the visible "the bar scrollbar looks different" is not a style
divergence at all. It is `ScrollbarTrack` and `TreeBackground` resolving to the
same palette index.

That matters for scoping: the abstraction is still worth building, but it should
be built for the reason that actually holds -- six literals with no test
pinning any of them, and four unnamed dimensions -- rather than for a
divergence that does not exist. Fixing the contrast is a one-line theme change
in a different document, and doing it first would be sensible.
