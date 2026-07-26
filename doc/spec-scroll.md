# Spec: generalized scrollable regions

## Status (verified against code)

R1-R6 are delivered and the machinery still exists, under different names than
this document originally used: `Viewport::listScrollView` (not
`compute_list_scroll_view`) and `Viewport::scrollbarMetrics`. Verified by
rendering both gutters through the real renderer: the document draws
`#||||||||||||||||||||`, the panel draws `######|||||||||||||||` -- same glyphs,
same semantic roles, differing only in thumb size, which is proportional and
correct.

**Sharing is partial, and the split matters.** `scrollbarMetrics` (thumb
geometry) genuinely serves all three. `listScrollView` (window + clamp) serves
the tree and the picker, but **not** the editor: `Viewport::compute`
(`src/Viewport.cpp:578-584`) computes its own `maximumFirst` and clamps
`requestedFirstVisualRow` itself, then calls `scrollbarMetrics` directly. So the
editor's clamp is a second implementation of the one rule `listScrollView`
exists to own -- a real duplication, not a naming difference.

**One piece of this document's Design was never planned or built.** The
"Scroll-command targeting" section below specifies an optional region target on
the scroll arguments. No R-step ever implemented it: `ScrollFractionArguments`
(`include/ssg/Keymap.h:222`) carries only `{numerator, denominator}` and
`view.scroll_to_fraction` mutates the editor offset unconditionally. That
omission is the reason the panel and picker gutters are not draggable --
`apps/pointer_routing.cpp:24` says so outright ("Panel/palette gutters are not
draggable yet") -- because there is no command such a drag could dispatch.

The S-steps at the end of this document close that gap.

## Goal

Make every long panel scroll through one shared, server-laid-out abstraction, so
the filesystem bar and the command palette scroll the way the editor already
does, and so each scrollable region publishes the hit-test classification that
milestone 8 (mouse) needs. Preserve the client-side fuzzy-find latency path for
the palette: typing must never round-trip to scroll.

This spec also owns the **reveal policy** — when the editor viewport follows the
caret — and the fix for the hardcoded viewport dimensions in the editor
scroll/reveal math. R1–R4 (delivered) built the scrollable-region model, layout,
rendering, and hit data. R5–R6 make edits reveal the caret (today typing
off-screen does not scroll into view) and replace the fake `{80, 24}` viewport in
the reveal, page-scroll, and scrollbar-fraction math with the real pane size.


This is groundwork for M8. It delivers the scrollable-region **model, layout,
rendering, and hit data**. It does **not** implement pointer input (click,
drag) — that is M8 — but it is designed so M8 is a thin client-side router over
the data this refactor publishes.

## Current state (facts)

- **The editor is the working reference.** `compute_viewport`
  (`src/viewport.cpp`) turns a document into `ViewportViewState { first_visual_row,
  visible_rows, total_visual_rows, hit_targets: CellHitTarget[], scrollbar:
  ScrollbarMetrics }`. The scroll offset is server state
  (`EditorRuntime::Impl::requested_first_visual_row`,
  `src/runtime/editor_runtime_internal.h`), mutated by
  `view.scroll_lines`/`scroll_pages`/`scroll_to_fraction`
  (`src/runtime/presentation.cpp`) and clamped in `compute_viewport`. `render.cpp`
  `paint_document` draws `visible_rows`; `paint_scrollbar` draws the thumb from
  `ScrollbarMetrics` into `PaneGeometry.scrollbar` (a 1-column rect at the pane's
  right edge; `PaneGeometry.content` is the pane minus that column).
- **`scrollbar_metrics(total_rows, viewport_rows, first_row) -> ScrollbarMetrics`**
  (`src/viewport.cpp`, anonymous namespace) is already the reusable primitive:
  it computes `maximum_first_row`, `thumb_start`, `thumb_size`. When
  `total <= viewport` it returns a zero-thumb sentinel.
- **`CellHitTarget { viewport_row, viewport_column, logical_line, cell,
  byte_offset, byte_len }`** (`include/ssg/viewport.h`) maps each visible editor
  cell back to a document position. Only the editor has this.
- **The filesystem bar does not scroll.** `paint_panel_tree` (`src/render.cpp`)
  loops tree nodes from index 0 and `break`s at `panel.height - 1`; nodes past
  the fold never render. `TreeViewState`/`TreeProviderView` (`include/ssg/tree.h`)
  hold `selected` (a node id) but **no scroll offset**, and
  `TreeModel::select_next/previous` (`src/tree.cpp`) move the selection without
  keeping it visible. The panel `Rect` from `compute_shell_layout`
  (`src/ui_layout.cpp`) has **no scrollbar gutter** — it is the full width.
- **The palette does not scroll.** The client ranks candidates locally
  (`apps/ssg_main.cpp` `build_report` + `palette_rank`) and sends **all** ranked
  rows + a `selected` index in `PaletteReport` (`include/ssg/palette_searcher.h`).
  `shell_view` (`src/runtime/snapshot.cpp`) copies them into `PaletteProjection`
  { rect, rows, selected } (`include/ssg/ui_layout.h`); `paint_palette`
  (`src/render.cpp`) loops from index 0 and `break`s at `rect.height`. No offset,
  no keep-visible, no windowing. The palette pane *does* already receive a
  `PaneGeometry` (content + scrollbar gutter), but the projection uses
  `panes.front().content` and nothing draws the gutter.
- **Shell geometry is server-owned.** `compute_shell_layout` produces
  `ShellViewState { header, footer, tab_bar, panel, prompt, panes:
  PaneGeometry[], accessibility_nodes: AccessibilityNode[], palette }`.
  `AccessibilityNode { kind: ShellNodeKind, id, label, rect, role, content }`
  classifies header/footer/tab/pane/scrollbar/prompt/empty regions — but carries
  **no row-level item mapping** for tree or palette.
- **R1–R4 are delivered** (this spec's original scope: the shared
  `scrollbar_metrics`/`compute_list_scroll_view` primitive, the scrolling tree and
  palette, and the `hit_test` seam). The remaining work below (R5–R6) is the
  **reveal policy** and the **real-viewport-dimension** fixes layered on top.
- **The reveal primitive already exists but is applied unevenly.**
  `revealed_first_row(model, state, dims, center)` (`src/selection.cpp`) is the one
  pure keep-visible function: it returns the minimal `first_visual_row` so the
  **primary** caret (`state.selections.primary().active`) lies in the viewport
  (or centers it when `center`). It is reached through the `view_reveal_caret` /
  `view_center_caret` selection commands. Every **selection-navigation** command
  ends by writing `selection.first_visual_row = revealed_first_row(...)`, and
  `bind_selection` copies that into the scroll offset
  (`requested_first_visual_row`), so cursor motion reveals the caret. Find does the
  same explicitly via `reveal_active_find_match`. **Edits do not.**
- **Edits never reveal the caret (the bug).** Typing, delete, indent/comment
  (`apply_transaction`, shared by `bind_text`/`bind_edit`), undo/redo
  (`bind_history`), and cut/paste (`bind_clipboard`) all set
  `selection.selections` and clamp, but **none touch `requested_first_visual_row`**.
  So an edit whose caret is off-screen leaves the viewport where it was — the user
  types where they are not looking. Three call sites update the selection after a
  document mutation without revealing.
- **Reveal/scroll math uses a hardcoded `{80, 24}` viewport, not the real pane.**
  `bind_selection` reveals against `ViewportDimensions{80, 24}`
  (`src/runtime/editing.cpp`); `view.scroll_pages` advances by `pages * 24`; and
  `view.scroll_to_fraction` derives `maximum_first_row` from
  `compute_viewport(runs, {80, 24})` (`src/runtime/presentation.cpp`). The real pane
  size is cached every snapshot as `last_pane_content_rows`/
  `last_pane_content_columns`/`last_reserved_prompt_rows`
  (`src/runtime/snapshot.cpp`) — the same cache `reveal_active_find_match` already
  uses. Because the navigation reveal is later re-clamped in `compute_viewport`
  against the *actual* dimensions, navigation mostly self-corrects; but a page is
  the wrong size and scrollbar-drag maps to the wrong maximum on any terminal that
  is not exactly 24 rows tall.


## Design

### One model: a scrollable region

A **scrollable region** is a rectangular list view with:

1. **Server-owned box geometry** — a `content` rect and an **always-reserved**
   1-column `scrollbar` gutter rect, exactly as `PaneGeometry` already provides
   for editor panes. The gutter is reserved whether or not a thumb is currently
   shown, so the content width is **stable**: a region whose item count changes
   rapidly (the palette as the user types) never reflows its text because a
   scrollbar appeared or vanished. The thumb is simply hidden (empty track) when
   `total <= viewport_rows`. This is the fix for "scrollbars disappear and resize
   the area."
2. **A scroll view** — the triple `(total_items, viewport_rows, first_visible)`
   resolved through **one shared library function** into a clamped
   `first_visible`, a visible window, `ScrollbarMetrics`, and a row→item hit map.
   The same function serves the editor, the tree, and the palette. No layer
   reimplements thumb math. **Keep-visible is an explicit input, not implied by
   passing a selection:** the function takes a `keep_selection_visible` flag.
   Selection-movement callers set it (the window shifts minimally to include the
   selection); explicit-scroll callers (wheel, gutter drag, PageUp/Down) clear it
   (the window honours the requested `first_visible` and the selection may leave
   the viewport, exactly as the editor caret can). See R1 for the API and the
   per-call-site rule.
3. **A hit map** — `viewport_row -> item` for the content area (item =
   document position / tree node id / palette row index), plus the gutter's
   track and thumb rects (already in `PaneGeometry.scrollbar` +
   `ScrollbarMetrics`). This is the data M8 routes pointer events against.

### Ownership split: follow the content

The client/server division that keeps the palette fast follows **who owns the
content**, not who draws the box. The server **always** owns box geometry; scroll
offset and scroll-view computation live with the content owner:

- **Server-owned content → server-owned scroll.** The **editor** (document text)
  and the **filesystem bar** (tree nodes) are server state. The server holds the
  scroll offset, computes the scroll view with the shared function, and keeps the
  selection visible. Already true for the editor; R2 extends it to the tree.
- **Client-owned content → client-owned scroll.** The **palette's** ranked list
  is produced client-side by fuzzy find for latency. The client already owns the
  query, ranking, and selection. It therefore also owns the palette **scroll
  offset**: it keeps its own selection visible and computes the palette scroll
  view with the **same shared library function** (linked into the app), then
  reports the **windowed** rows plus the resolved `ScrollbarMetrics` in
  `PaletteReport`. The server projects them into the snapshot verbatim and paints
  the gutter. A keystroke never round-trips: ranking, windowing, and thumb math
  are all client-local.

The shared function is the seam that makes this safe: because both sides call one
implementation, the palette's client-computed scrollbar is pixel-identical to
what the server would have produced, and there is no duplicated thumb math to
drift.

**Selection is an absolute index everywhere.** For every region, `selected`
denotes the item's position in the *full* content list (document visual row,
flattened tree-node index, or ranked-candidate index) — never a window-relative
row. The window is `[first_visible, first_visible + visible_count)`; a control
computes its on-screen row as `selected - first_visible` only at paint/hit-test
time, and only when `first_visible <= selected < first_visible + visible_count`.
The palette specifically reports its **absolute** `selected` (index into the full
ranked order) plus `first_visible`; the projection carries the windowed rows but
keeps `selected` and `first_visible` absolute, so render highlights
`selected - first_visible` and hit-testing maps a clicked window row `r` back to
the absolute candidate `first_visible + r`. R3 and R4 state this mapping at each
call site.

Everything the server lays out (the content rect and the reserved gutter) is
authoritative for **all three** regions; only the palette adds a small,
already-per-keystroke payload (its windowed rows + metrics) on top.

### Reveal policy: the editor viewport follows the caret

The editor's scroll offset (`requested_first_visual_row`) changes for exactly two
reasons, and every command falls into one:

1. **A caret-affecting change → reveal (keep-visible).** Any command that moves or
   edits the primary caret pulls the viewport minimally so the caret is on-screen,
   via `revealed_first_row` against the **real** pane. This covers:
   - **Navigation** — cursor/select commands (arrows, word/line/page/document
     motion, click-to-caret, drag-select). Already reveals.
   - **Edits** — insert/newline/delete/indent/outdent/comment/duplicate/move-line/
     etc. (`apply_transaction`), undo/redo (`bind_history`), and cut/paste
     (`bind_clipboard`). **R5 makes these reveal** — the gap today.
   - **Find** — `find.next`/`previous`/`update_query` reveal the active match.
     Already reveals (`reveal_active_find_match`), reserving the prompt rows.
2. **An explicit scroll gesture → free scroll (no reveal).** `view.scroll_lines`
   (wheel), `view.scroll_to_fraction` (scrollbar drag), and `view.scroll_pages`
   (PageUp/Down) set the offset directly and deliberately let the caret leave the
   viewport — scrolling is the one gesture that decouples the view from the caret.
   These must **not** snap back.

The **single reveal target is `selections.primary().active`** — the primary
caret. This resolves the multi-cursor ambiguity by contract: an edit at N cursors
inserts at all of them but the viewport follows the **primary** caret only (the
well-defined lead), exactly as most editors do. No averaging, no "nearest",
no per-cursor policy. `view.center_caret` is the sole command that centers rather
than minimally reveals; every other reveal is minimal (bring just onto the edge).

**One reveal helper, one real viewport.** R5 introduces a single internal
`reveal_primary_caret(runtime)` that mirrors `reveal_active_find_match` for the
plain caret: it builds the reveal `ViewportDimensions` from the cached real pane
(`last_pane_content_columns` × the pane rows minus any reserved prompt rows),
runs `view_reveal_caret`, and writes `requested_first_visual_row`. It is called
from the three edit call sites (R5) and replaces `bind_selection`'s hardcoded
`{80, 24}` reveal (R6), so navigation and edits share one reveal path at the real
size. Like the existing tree-scroll and find-reveal, it reads the pane dimensions
cached from the previous snapshot — a one-frame lag that self-corrects on the next
frame, the same discipline already in use (never a cross-client hazard: the offset
is this document's server state).

**Reveal is a post-mutation obligation (the anti-drift rule).** The invariant a
fresh implementer must uphold: **any command that mutates `selection.selections`
following a document mutation reveals the primary caret** (calls
`reveal_primary_caret`), unless it is one of the three explicit scroll commands.
The three known edit hooks today are `apply_transaction` (typing + edit commands),
`bind_history` (undo/redo), and `bind_clipboard` (cut/paste) — but the rule is the
contract, not the list, so future mutating commands cannot silently regress.
Because `apply_transaction` is already the shared seam for `bind_text`/`bind_edit`,
R5 should prefer routing every document-mutating command through it (or a single
post-mutation helper it calls) rather than sprinkling reveal calls; where a path
cannot yet share that seam (`bind_history`, `bind_clipboard`), it calls the helper
directly.

**Audit of the other caret-moving paths (outcome).** Every command that can move
the editor caret was checked against the reveal obligation:
- **`replace.current`/`replace.all`** — reveal. `reveal_active_find_match` reveals
  the next remaining match; when none remains (common after `replace.all`) the
  handler falls back to `reveal_primary_caret`. (R5.)
- **`file.open`/`file.open_recent`/`tree.activate` opening a file** — reveal via
  `reset_selection_for_active_document`, which puts the caret at the document start
  and sets `requested_first_visual_row = 0`; a freshly opened document shows its
  top with the caret visible, and a stale scroll from the previous document does
  not carry over (regression-tested). This is the reveal for a top caret; if a
  future feature restores a non-top caret on reopen, that path must call
  `reveal_primary_caret` explicitly.
- **LSP `goto.definition`/`goto.reference`/`rename.symbol`** — no-op today
  (unconfigured stubs returning failure); they move no caret, so there is nothing
  to reveal. **When LSP navigation is implemented, its caret move must reveal**
  (the navigation view state already carries a `reveal_primary_caret` intent flag).
- **`goto.file`/`goto.line`/`goto.symbol`/`goto.back`/`goto.forward`** — the
  runtime handler currently discards the `NavigationTransition` (which carries the
  target and `reveal_primary_caret = true`), so goto does not yet move the editor
  caret. **When goto application is wired, it must honour that flag and reveal.**
- **`tab.activate`/`tab.next`/`tab.previous`/`tab.close`/`reopen_closed` (switching
  the active document)** — reveal. `bind_tab` reveals the primary caret whenever a
  tab command changes the active document, so the newly active tab's caret is
  on-screen instead of inheriting the previous tab's scroll offset.
  `tab.move_left`/`tab.move_right`/`tab.close_others` keep the same active document
  and deliberately do NOT reveal (they must not snap a scroll the user set). Per-tab
  caret/scroll persistence across switches remains a separate future feature.



### Scroll-command targeting

`view.scroll_*` implicitly targets the editor today. Generalize with an optional
region target on the scroll arguments (absent = editor, preserving every current
binding):

- **editor, panel** → dispatched to the server, which mutates that region's
  server-owned offset.
- **palette** → **never dispatched.** The client owns the offset and mutates it
  locally (wheel, PageUp/Down while the palette is focused, or keep-selection-
  visible after a rank change). There is intentionally no server command for
  palette scroll.

### Mouse classification (data only; input is M8)

Each region publishes, in the snapshot, what a pointer router needs:

- the region's `content` and `scrollbar` rects and its `ShellNodeKind`
  (extend the enum with the tree/palette content + a `panel`/`palette` scrollbar
  classification as needed);
- a `viewport_row -> item` hit map for the content (the editor keeps its richer
  `CellHitTarget[]`; the tree and palette get a simple row→id / row→index list);
- the scrollbar track rect (`PaneGeometry.scrollbar`) and thumb extent
  (`ScrollbarMetrics.thumb_start`/`thumb_size`) so a gutter click/drag maps to a
  scroll fraction.

R4 delivers a pure client-side `hit_test(snapshot, column, row) -> RegionHit`
helper (region kind + item or scrollbar-fraction) with unit tests, which is the
exact seam M8's pointer handling will call. R4 implements **no** input handling.

### Divisibility

Seven independently shippable steps, in order: R1, R2a, R2b, R3, R4 (all
**delivered**), then R5, R6. R1 is a pure refactor with no behavior change; R2a
and R3 each end in a hand-testable scrolling panel; R2b and R4 add the M8-facing
hit contract. R5 (reveal on edit) and R6 (real-viewport dimensions) are the two
new steps: R5 fixes the type-off-screen bug, R6 fixes page size and
scrollbar-drag maximum on non-24-row terminals. R5 depends on the
`reveal_primary_caret` helper it introduces and ships/validates independently
(its edit reveal already uses the real cached dimensions); R6 then reuses that
helper for `bind_selection` and applies the same real-pane cache to the two
scroll commands.

## Plan

| Step | Work | Files | Oracle |
|---|---|---|---|
| R1 | Promote `scrollbar_metrics` to public library API (`include/ssg/viewport.h`), and add a pure `compute_list_scroll_view(total_items, viewport_rows, first_visible, selected, keep_selection_visible) -> ListScrollView { first_visible, visible_count, ScrollbarMetrics }`. It always clamps `first_visible` to `[0, maximum_first_row]`. Only when `keep_selection_visible` is true AND `selected` is set does it further adjust `first_visible` minimally so `selected` lies in `[first_visible, first_visible+visible_count)`; when false it honours the clamped `first_visible` verbatim (selection may be off-screen). `selected` is an absolute item index. No caller changes yet (pure addition). | `include/ssg/viewport.h`, `src/viewport.cpp`, `cmake` (if a new TU), `tests/test_viewport.cpp` | unit table over `(total, viewport, first, selected, keep)`: empty; shorter-than-viewport (thumb hidden, `first=0`); exactly full; over-scrolled `first` clamped to `maximum_first_row`; with `keep=true` — selection above/below/inside the window forces the minimal up/down shift; with `keep=false` — the same selection leaves `first_visible` untouched (selection off-screen allowed). In all cases the returned `ScrollbarMetrics` equals `scrollbar_metrics(total, viewport, first')`. Reference-check `compute_viewport`'s existing metrics are byte-identical (no regression). |
| R2a | Filesystem bar scrolls (server-owned, behavior + render). Reserve a 1-column scrollbar gutter for the panel in `compute_shell_layout` (panel `content` = width−1, panel `scrollbar` = right column), mirroring `PaneGeometry`; add a server tree scroll offset; on `TreeModel::select_next/previous` and on tree mutation, recompute the offset via R1 with `keep_selection_visible=true`; window `paint_panel_tree` from `first_visible`; paint the panel scrollbar thumb from the R1 metrics. | `include/ssg/ui_layout.h`, `src/ui_layout.cpp`, `include/ssg/tree.h`, `src/tree.cpp`, `src/render.cpp`, `tests/test_tree.cpp`, `tests/test_render.cpp`, `tests/fixtures/tui/*` | runtime: a tree taller than the panel scrolls so `select_next` past the fold keeps the selected node in the visible window; `select_previous` back above the top scrolls up. render: the panel draws a thumb sized/positioned by R1 when `nodes > rows`, an empty gutter (content width unchanged) when `nodes <= rows`; the selected node is always painted. |
| R2b | Publish the tree hit map + wire it. Add a bounded `viewport_row -> TreeNodeId` hit map (visible window only) plus the tree scroll offset to `TreeProviderView`/`TreeViewState`; serialize the new fields in `src/protocol.cpp` with a round-trip test. | `include/ssg/tree.h`, `src/runtime/snapshot.cpp`, `src/protocol.cpp`, `tests/test_protocol.cpp`, `tests/test_tree.cpp` | hit map: for a scrolled tree, `viewport_row 0..k` map to exactly the on-screen `TreeNodeId`s (absolute index `first_visible + row`); the map length never exceeds the visible window; protocol round-trip preserves the offset + hit map. |
| R3 | Command palette scrolls (client-owned). Client computes its palette scroll view with R1 (`keep_selection_visible=true` on selection move; false on explicit scroll), reports the **windowed** rows, the **absolute** `selected`, `first_visible`, and the resolved `ScrollbarMetrics` in `PaletteReport`; `shell_view` projects them into `PaletteProjection` verbatim; `paint_palette` renders the window and the gutter thumb from the reported metrics, highlighting `selected - first_visible`; content width is unchanged whether or not a thumb shows. **Wire scope:** both `PaletteReport` (a client→server call parameter) and `PaletteProjection` (a render-derived field of `ShellViewState`) are **in-process only** — the palette projection is already excluded from the `ShellViewState` protocol codec and from `shell_equal`, because a remote client owns its own fuzzy-find/report and renders its own palette. R3 keeps this: no new codec, no `shell_equal` change. The client caches the palette pane height from the previous snapshot's `panes.front().content.height` (identical to the editor pane), so the window is resolved against the real height with no cold-start (the editor pane rect exists before the palette opens). | `include/ssg/palette_searcher.h`, `include/ssg/ui_layout.h`, `src/runtime/snapshot.cpp`, `src/render.cpp`, `apps/ssg_main.cpp`, `tests/test_render.cpp`, `tests/test_ssg_app.cpp` | app unit: with N ranked candidates and a pane of `rows` < N, the client's reported window is exactly the R1 window around the absolute `selected`, and shrinking N by typing so `N <= rows` hides the thumb without changing `content` width. render: a projection with more rows than fit draws exactly the windowed rows + a correctly sized thumb, highlighting `selected - first_visible`; `selected` stays visible after windowing. PTY: type to produce a long list, arrow past the fold, the list scrolls and the selected row stays on screen. |
| R4 | Uniform hit-test seam (data + pure helper; no input handling). Add a client-side pure `hit_test(SessionSnapshot const&, int column, int row) -> RegionHit` returning `{ kind: editor/panel/palette/scrollbar, item: document offset / TreeNodeId / palette row index, or scrollbar fraction }`, built from the editor `CellHitTarget[]`, the R2 tree hit map, the R3 palette window, and the region rects + `ScrollbarMetrics`. | `include/ssg/hit_test.h` (or `apps/`-local), `src/hit_test.cpp`, `cmake`, `tests/test_hit_test.cpp` | unit: for a known composed snapshot (bar + editor + open palette), a column/row in the editor returns the right document offset; in the bar returns the right `TreeNodeId`; in the palette returns the right row index; on a scrollbar returns the right region + a fraction that round-trips through `scroll_to_fraction`; a cell in the reserved-but-empty gutter and out-of-bounds return "no target". |
| R5 | Reveal the caret on edit (the type-off-screen fix), consolidated. Add an internal `reveal_primary_caret(EditorRuntime::Impl&)` that resolves the real reveal viewport from the cached pane (`last_pane_content_columns` × `last_pane_content_rows` minus reserved prompt rows, ≥1), runs `apply_selection_navigation(..., SelectionCommand::view_reveal_caret, real_viewport)`, and writes `requested_first_visual_row` (the plain-caret analog of `reveal_active_find_match`). Call it after the selection is updated in the three edit paths: `apply_transaction` (covers `bind_text` typing/newline and `bind_edit` edit commands), `bind_history` (undo/redo), and `bind_clipboard` (cut/paste; skip copy, which does not move the caret off-screen). It always reveals the **primary** caret and never centers. Explicit scroll commands are untouched (they must still let the caret leave the viewport). | `src/runtime/editing.cpp`, `src/runtime/editor_runtime_internal.h`, `tests/runtime/test_runtime_editing.cpp`, `tests/runtime/test_runtime_presentation.cpp` | runtime with a **concrete, hand-computed** expectation (independent of `revealed_first_row`, so the test catches a mis-wired helper): open a document of, say, 100 single-cell lines into a pane of exactly `H` content rows (snapshot once at a fixed size so the cache is `H`); `view.scroll_lines(+40)` moves `first_visual_row` to 40 with the caret (line 0) now off-screen above; then a `text.insert` at the caret must set `first_visual_row` to exactly **0** (the minimal offset that brings visual row 0 onto the top edge) — assert the literal `0`, not a re-derivation. Symmetrically, put the caret on line 99, scroll to `first_visual_row = 0`, and assert an edit reveals it to exactly `100 - H` (the last-line-visible offset). Plus behavior cases: a **plain `view.scroll_lines` with no edit does NOT snap back** (free scroll preserved); newline/delete/paste/undo each reveal; with two cursors (primary on line 0, secondary on line 99) an insert reveals to `0` (the **primary**, not the secondary). A dispatch-level wiring guard rounds it out: after each of `text.insert`, `edit.duplicate_line`, `edit.move_line_down`, `clipboard.paste`, and `edit.undo` from a scrolled-away view, the primary caret's visual row is within `[first_visual_row, first_visual_row + H)`. |

| R6 | Real viewport dimensions for editor scroll (row×col resolution limits). Replace the hardcoded `{80, 24}` in the editor scroll/reveal math with the cached real pane: `bind_selection` reveals via the R5 `reveal_primary_caret` helper (real dimensions) instead of `ViewportDimensions{80, 24}`; `view.scroll_pages` advances by `pages * max(last_pane_content_rows, 1)` (a page == the real pane height) instead of `pages * 24`; `view.scroll_to_fraction` derives `maximum_first_row` from `compute_viewport(runs, real_dimensions)` instead of `{80, 24}`. (Pane-focus geometry `pane.focus_*`'s `{80, 24}` and the follow-edits attach default are layout/geometry, not editor scroll, and are out of this step's scope.) | `src/runtime/presentation.cpp`, `src/runtime/editing.cpp`, `tests/runtime/test_runtime_presentation.cpp`, `tests/runtime/test_runtime_editing.cpp` | runtime on a non-24-row terminal (e.g. a 40-row pane): `view.scroll_pages(+1)` advances `first_visual_row` by the real pane rows (not 24); `view.scroll_to_fraction(1/1)` reaches the real `maximum_first_row` for that terminal (the last line becomes visible, not the 24-row-derived max); a cursor move at the far right column of a >80-column line reveals correctly (no clamp at column 80). Navigation reveal continues to keep the caret visible after routing through the shared helper. |


## Invariants and fit

- **M7-2 / library owns behavior.** All scroll offsets and scroll-view math for
  server-owned content remain in the library/runtime; the app only owns terminal
  I/O and (for the palette alone, whose content it owns) its local offset. R1's
  shared function is library code linked into both, so the client computes but
  does not *define* palette scroll behavior.
- **Single source of truth.** Each region has exactly one owner of its scroll
  offset (server for editor/tree, client for palette). No region's offset is
  stored in two places.
- **Stable content width.** The scrollbar gutter is always reserved for a
  scrollable region; a thumb appearing or disappearing never changes the content
  rect. (New invariant this spec establishes.)
- **Pit of success.** Adding a new scrollable panel means: give it a
  `PaneGeometry`-style content+gutter, own its offset with the content, and call
  the shared scroll-view function. No new thumb math, no new clip-from-zero loop.
- **Snapshot/delta discipline.** Any new published field (tree offset + hit map;
  palette window + metrics) gets protocol encode/decode + a round-trip test, per
  the existing wire discipline.

## Considerations and risks

- **Palette metrics on the wire.** The client already sends the ranked rows every
  keystroke; adding `first_visible` + `ScrollbarMetrics` is a few integers. If
  `PaletteProjection` is re-derived server-side rather than sent, keep the
  client-owned offset authoritative and pass it through unchanged.
- **Keep-visible vs. free scroll.** Selection movement keeps the selection
  visible; explicit scroll (wheel/gutter/PageUp-Down) may move the selection
  off-screen (like the editor). R1's `keep_selection_visible` flag makes this a
  call-site decision: selection-change handlers pass `true`; explicit-scroll
  handlers pass `false`. No handler infers the mode from whether a selection
  exists. **R5 extends "selection movement" to include edits:** an edit is a
  caret-affecting change and reveals; only the three explicit scroll commands are
  free-scroll. The distinction is per-command (edit vs. scroll), never inferred.
- **Reveal target is the primary caret.** Every editor reveal (navigation, edit,
  find) targets `selections.primary().active` at the real pane size. Multi-cursor
  never averages or picks "nearest" — the viewport follows the primary caret. This
  is the single rule that keeps "where to scroll" unambiguous across N cursors.
- **Reveal is a post-mutation obligation.** Any command that mutates
  `selection.selections` after a document mutation must reveal the primary caret
  (only the three explicit scroll commands are exempt). Prefer routing mutating
  commands through the one post-mutation seam that reveals, so a new mutating
  command cannot silently ship without a reveal. (New invariant this spec's R5
  establishes.)
- **Cached pane dimensions have a one-frame lag.** R5/R6 read
  `last_pane_content_rows`/`columns`/`last_reserved_prompt_rows` cached from the
  previous snapshot (the same cache find-reveal and tree-scroll already use). A
  resize lags one frame before a reveal settles; it self-corrects on the next
  snapshot, and `compute_viewport` re-clamps the offset to the real dimensions
  regardless, so a stale cache can never place the offset out of range.
- **Tree hit map size.** Publish only the visible window's row→id entries, not
  the whole tree, to bound snapshot size.
- **R4 placement.** If a future `--http` client needs hit-testing too, `hit_test`
  belongs in the library (`include/ssg/`); if it is only ever a TUI concern, it
  may live app-side. Prefer the library for reuse, consistent with M7-2.
- **Not in scope.** Horizontal scrolling, momentum/smooth scroll, and pointer
  input handling (M8) are out of scope; the model leaves room for them.

## Consolidation (S1-S4)

### What is already shared, and what is not

Verified in code, because the answer changes the shape of the work:

- **Shared already:** the thumb geometry (`Viewport::scrollbarMetrics`, used by
  all three); the gutter reservation; the painter (`paintScrollGutter`,
  `src/Renderer.cpp:230`); the hit classification (all of `EditorScrollbar`,
  `PanelScrollbar`, `PaletteScrollbar` are published by `src/HitTester.cpp`);
  and the wheel (`route_wheel` handles all three).
- **Shared by two of three:** `Viewport::listScrollView` -- the window-and-clamp
  rule -- is called by the trees (`src/runtime/snapshot.cpp:239,267,281`) and
  the picker (`src/PaletteSearcher.cpp:104`,
  `apps/ssg_main.cpp:810,821`). The **editor does not use it**:
  `Viewport::compute` clamps with its own `maximumFirst` arithmetic. Two
  implementations of one rule.
- **Not shared:** the *offset state* and the *operations on it*. Three
  independent integers -- `requestedFirstVisualRow`, `treeFirstVisible`
  (`src/runtime/editor_runtime_internal.h:194`), and the client's
  `window.firstVisible` (`apps/ssg_main.cpp:944`) -- each mutated by bespoke
  handlers. And the *pointer routing*: `route_pointer` handles press and drag
  for `EditorScrollbar` only.

So this is not "three scrollbar implementations to merge" -- drawing and thumb
geometry are already one implementation. What diverged is the clamp rule (twice)
and everything that owns or mutates an offset, which is precisely the part no
shared *function* could capture: a pure function cannot own state, so each
caller grew its own field and its own mutators.

### Mechanism: a state-owning ScrollOffset, not a bigger function

`ScrollOffset` is a small value type owning the one durable field
(`firstVisible`) and exposing the operations every scrollable surface needs:
`byLines`, `byPages`, `toFraction`, `revealSelection`, and `resolve`. Each
operation is expressed in terms of the existing `listScrollView`, so the math
stays where it already is and nothing is reimplemented.

Geometry (`totalItems`, `viewportRows`, `selected`) is passed **at each call**
rather than stored in the object. Those change every frame, and a class that
cached them would be a stale-cache bug waiting to happen; the current code
already threads them per-frame, so this preserves the working discipline.

Rejected alternative: a `ScrollableRegion` base class that each view inherits.
The three views have irreconcilable ownership -- editor and tree offsets are
server state, the picker's is client state by deliberate latency design (a
keystroke must not round-trip) -- so a common base would either drag the picker
onto the server or force the server types to satisfy a client-shaped interface.
A value type sidesteps that: both sides hold one, neither inherits anything.

### Mechanism: a scrollable-region catalog for routing

`route_pointer` handles `EditorScrollbar` and silently ignores the other two.
Adding a `case` per region is what already failed, so the fix must make
forgetting one impossible.

A `ScrollableRegionDescriptor { contentRegion, scrollbarRegion, target }`
catalog lists the scrollable surfaces; `route_pointer` and `route_wheel` both
drive from it. A test enumerates the catalog and asserts each entry's gutter
answers press and drag.

**A `switch` is not sufficient here, and this is measured, not assumed.** While
implementing `doc/spec-file-management.md` a switch over an enum was perturbed
by adding an enumerator: it compiled cleanly with no warning, because this build
does not enable `-Wswitch`. A descriptor table with an exhaustiveness test is
the pattern that does hold, and is already used by `PickerKind`,
`FileCommandDescriptor::pathPrompt`, and
`FileCommandDescriptor::mutatesActiveDocumentFile`.

The catalog has exactly three entries and `HitRegion`
(`include/ssg/HitTester.h:17`) confirms there is no fourth scrollable surface --
live diffs are document tabs, find results render in prompt rows, and the status
queue is a footer. Three entries is thin for a catalog, and the justification is
not size but evidence: two of those three were already forgotten, and the
cheaper mechanism provably does not catch that.

### Routing a gesture the server must not see

`route_pointer` returns `PointerDispatch { commands, begins_drag, ends_drag }`
(`apps/pointer_routing.h:35`) -- commands only. That shape can express an editor
or panel gutter drag, both of which dispatch a server command, but it **cannot
express a picker gutter drag**, because S-I5 forbids sending picker scroll to
the server. Routing all three uniformly therefore needs the return type to carry
a client-local outcome as well, not just commands.

`PointerDispatch` gains an optional `scrollFraction { region, numerator,
denominator }` that the caller applies to a client-owned offset. The panel and
editor keep emitting commands; the picker emits this instead. `route_pointer`
stays a pure function of the hit -- which is what makes it testable -- and the
app remains the only thing that touches client state.

Rejected alternative: give the picker a server command after all, so every
region routes identically. That is simpler here and wrong overall: it puts a
per-keystroke-frequency scroll on the wire for a list the server does not own,
which is the exact latency property the picker's client-side design exists to
protect.

### Region-targeted scroll commands

`ScrollFractionArguments` and `ScrollLinesArguments` gain an optional region
target; absent means editor, so every existing binding and payload keeps
working. `view.scroll_to_fraction` with a panel target moves the tree offset.
The picker is deliberately excluded: its content and offset are client-owned for
latency, and it handles its own fraction locally with the same `ScrollOffset`.

This is the piece the original Design specified and no R-step built.

## Invariants (consolidation)

- **S-I1** Exactly one owner per offset; no offset stored twice.
- **S-I2** Every scrollable surface responds to wheel, gutter click, and thumb
  drag. Enforced by a catalog-driven test, not by review.
- **S-I3** No layer computes thumb geometry or window bounds except
  `listScrollView` / `scrollbarMetrics`. This is **not** true today -- the
  editor's clamp in `Viewport::compute` is a second implementation -- and S2 is
  what makes it true.
- **S-I4** An explicit scroll never reveals the selection; a selection move
  always does.
- **S-I5** The picker's offset stays client-owned. No command dispatches picker
  scroll to the server.

## Considerations (consolidation)

- The picker uses `ScrollOffset` in the app while the editor and tree use it in
  the library, so it must live in `include/ssg/` and carry no runtime
  dependency.
- Panel gutter drag needs the tree's total node count and panel height at
  dispatch time. The tree scroll path already caches `lastPanelContentRows`;
  the fraction handler must use the same cache rather than introduce a second.
- A gutter click currently maps to a fraction via
  `HitTester::scrollbarHit`, which divides by the gutter height. That is already
  region-agnostic, so no hit-testing change is expected -- verify rather than
  assume.
- Thumb-relative dragging (grabbing a thumb mid-point and preserving the grab
  offset) is a refinement, not this work: today's editor drag jumps the thumb
  centre to the pointer. Whatever it does, all three must do the same.
- Retiring the now-redundant `tree.scroll` is tempting but is a command-catalog
  cascade touching six sites. Keep it; it is the wheel's path.

## Plan (consolidation)

| # | Step | Files | Oracle |
|---|------|-------|--------|
| S1 | Add `ScrollOffset` owning `firstVisible` with `byLines`/`byPages`/`toFraction`/`revealSelection`/`resolve`, each implemented via `listScrollView`. Pure addition, no callers yet | `include/ssg/Viewport.h`, `src/Viewport.cpp`, `tests/test_viewport.cpp` | ref-impl: for a table of (total, rows, offset, selected), every `ScrollOffset` operation equals the existing hand-called `listScrollView` result. Perturb: break one operation, table fails |
| S2 | Route the three offsets through `ScrollOffset`, including replacing `Viewport::compute`'s own `maximumFirst` clamp so the editor stops being a second implementation. Behavior-preserving | `src/Viewport.cpp`, `src/runtime/presentation.cpp`, `src/runtime/snapshot.cpp`, `apps/ssg_main.cpp` | per region, asserted directly rather than relying on the suite: over-scrolling clamps to the same `maximumFirstRow` as before; an explicit scroll leaves the selection off-screen; a selection move reveals minimally. Plus the existing suite green with no test edits. Perturb: an off-by-one in the shared clamp must fail all three regions, proving they now share it |
| S3 | Add the optional region target to the scroll arguments and honour it for the panel; add the scrollable-region catalog; extend `PointerDispatch` with a client-local `scrollFraction`; route press and drag for `PanelScrollbar` and `PaletteScrollbar` | `include/ssg/Keymap.h`, `src/Protocol.cpp`, `src/runtime/presentation.cpp`, `apps/pointer_routing.h`, `apps/pointer_routing.cpp`, `apps/ssg_main.cpp` | catalog-driven: every descriptor's gutter yields either a command or a `scrollFraction` on press AND on drag -- never nothing. Perturb: drop one region's routing, test fails. Protocol round-trip for the new field. Absent target still means editor. Invariant: no descriptor whose target is the picker emits a command (S-I5) |
| S4 | Record the delivered state: mark the S-steps done here, and confirm `doc/spec-ux.md`'s gesture invariant is satisfied rather than aspirational | `doc/spec-scroll.md`, `doc/spec-ux.md` | doc-consistency tests; the S-I2 catalog test from S3 is the evidence the invariant is true |

Acceptance: a user can drag the file-explorer thumb and the git-status thumb and
the picker thumb, and each scrolls live exactly as the document's does; clicking
any gutter jumps. **Visual output -- requires signoff, not assumed.**
