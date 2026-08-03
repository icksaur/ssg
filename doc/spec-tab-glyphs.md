# Spec: configurable tab edge/separator glyphs + single-space fix

Status: draft
Backlog items: 4 (left/right tab-edge glyphs), 5 (inter-tab separator glyph),
6 (fix "two spaces after tab text" -> one space). Item 6 is a consequence of
4/5, so all three ship together.

## Problem

Each tab's width is `displayCells(title[+dirtySuffix]) + dimensions.labelPadding`
(labelPadding = 2). The renderer fills the whole chip and left-aligns the label,
so every tab draws two trailing background cells. Tabs are placed adjacently, so
the visible result is "two spaces after each tab" -- the reported bug. There is
also no way to configure how a tab begins/ends or how tabs are separated.

## Goal

- A tab's on-screen text is composable from three configurable glyphs plus the
  title: `leftEdge + title[+dirtySuffix] + rightEdge`.
- Consecutive tabs are separated by a configurable `separator` glyph.
- Defaults reproduce a *tight* tab row with exactly one space between tabs:
  `leftEdge = ""`, `rightEdge = ""`, `separator = " "`. No trailing padding.
- Users can set e.g. `leftEdge="["`, `rightEdge="]"`, `separator=" | "`.

## Design

### Style: a variable-width glyph category

`TabGlyphs` (include/ssg/Style.h) gains three fields:
- `std::string leftEdge = "";`
- `std::string rightEdge = "";`
- `std::string separator = " ";`
style keys: `tab_left_edge`, `tab_right_edge`, `tab_separator`.

The existing glyph validator (`rejectGlyph`) locks a replacement to the DEFAULT
glyph's cell width, because most glyphs fill a fixed slot (scrollbar cells,
toggles, dirty/liveDiff/readOnly suffixes that occupy reserved columns). Tab
edges and the separator are **layout-affecting**: changing their width is legal
because the tab layout recomputes tab rects from their actual width. Locking
them to width 0 / 1 would make them unconfigurable (the whole point).

Introduce a second glyph category: **variable-width glyphs**.
- New setter map `variableGlyphSetters()` alongside `glyphSetters()`, holding
  the three tab keys.
- New validator `rejectVariableGlyph(key, value)` = `rejectGlyph` minus the
  width check (still: no newline/CR, no control chars, valid UTF-8).
- `applyStyleDefine` consults `variableGlyphSetters()` before/after
  `glyphSetters()`; a key lives in exactly one map.
- **Disjointness is an invariant, not an ordering convention.** The two maps
  MUST NOT share a key; otherwise validation order becomes a hidden contract and
  a duplicated key silently takes whichever path is checked first. Enforce with
  a test that asserts `glyphSetters()` and `variableGlyphSetters()` key sets are
  disjoint (and that their union equals the full set of style glyph keys).
- The style scan test `everyGlyphFieldIsValidatedAndEveryDefaultIsValid` must
  cover variable glyphs too (each default round-trips), plus a new assertion
  that a *different-width* value is ACCEPTED for a variable glyph and REJECTED
  for a fixed glyph.

### Layout (src/ShellState.cpp)

Replace the `labelPadding`-based width with glyph composition. A single helper
builds the painted display string for a tab:

```
tabDisplay(tab) = leftEdge + tab.title + (tab.dirty ? dirtySuffix : "") + rightEdge
```

`tabWidth(tab) = max(1, displayCells(tabDisplay(tab)))` -- no labelPadding.

Separators sit BETWEEN tabs. To keep the separator paintable (a configured
`" | "` or "|" must be visible, not just empty band space) it is emitted as part
of the layout, but it is **not part of any tab's clickable hit rect**.

**Single source of truth: the separator is its own painted node, never appended
to a tab's content.** A dedicated non-interactive node
`ShellNodeKind::TabSeparator` (role `SemanticRole::TabInactive`) carries the
separator string over the gap rect. In the placement loop, for each tab except
the last placed one:
- place the tab node + `TabHit` at width `tabWidth(tab)` (the chip:
  `leftEdge+title[+dirtySuffix]+rightEdge`);
- advance `tabX` by `displayCells(separator)` and emit one `TabSeparator` node
  covering that gap rect.

The tab's node rect and `TabHit` rect cover only the chip; the separator gap is
a distinct node with no hit and no command. The renderer paints the separator
node like any tab-bar leaf; the hit-tester ignores it. Because the separator is
never baked into a tab's content string, there is no double-paint and the width
math (`tabWidth`) and hit math (`TabHit`) stay identical to the chip.

The windowing math (`firstTab`) must include separators when summing used width
so the active tab stays visible: `used = sum(tabWidth) + separators between the
counted tabs`.

### Renderer (src/Renderer.cpp)

`ShellNodeKind::TabSeparator` paints exactly like a tab-bar text leaf: fill the
gap rect with `TabInactive` background, paint the separator string. No special
casing beyond adding it to the tab-bar background branch.

### Hit-testing (src/HitTester.cpp)

`TabSeparator` produces no hit (it is not in `tabHits`, and the generic node
loop must skip it -- it carries no command). Clicking a separator does nothing,
matching the empty tab-bar area.

### Protocol (src/Protocol.cpp)

- Style codec: encode/decode/aggregate-init the three new `TabGlyphs` strings.
- `ShellNodeKind` enum codec: add `TabSeparator`.
Regenerate protocol goldens (`SSG_REGEN_PROTOCOL_FIXTURES=1`) and the ui_layout
golden (`SSG_REGEN_GOLDEN=1`) -- the golden serializes node kinds + rects, which
change (separator nodes appear, tab widths shrink by 2).

## Testing (oracle-first)

1. Style: variable glyph accepts a different-width value; fixed glyph still
   rejects one (extend `test_style.cpp`, including `currentGlyph` coverage for
   the three new keys).
2. Layout: with defaults, N tabs of known titles produce tab rects with NO
   trailing padding and exactly `displayCells(separator)` between adjacent tab
   rects; total row consumption = sum(titleWidths) + (N-1)*sepWidth.
3. Layout: a configured `leftEdge`/`rightEdge` widens each tab rect by the edge
   widths; a configured multi-cell separator widens the inter-tab gap and
   emits `TabSeparator` nodes with the separator text.
4. Windowing: the active tab stays visible when separators push earlier tabs
   off the left (regression of the firstTab math with separator width included).
   Pin a case with a MULTI-CELL separator AND non-empty edge glyphs in a tight
   viewport, so a separator-width regression cannot pass under single-cell
   defaults.
5. Hit-testing: a click on a separator gap yields no tab activation; a click on
   a tab chip still activates that tab (rects unchanged by the padding removal
   except being tighter).

## Non-goals

- No per-tab (as opposed to global) glyphs.
- No change to `dirtySuffix`/`liveDiffPrefix`/`readOnlySuffix` (still
  fixed-width, still inside `title`/appended as today).
- No new theming/colors for the separator (uses TabInactive).

## Visual signoff

Required before merge (tab row is visible chrome): render the default (tight,
one-space) row and at least one configured row (`leftEdge="["`, `rightEdge="]"`,
`separator=" | "`).
