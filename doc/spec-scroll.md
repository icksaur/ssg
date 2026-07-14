# Spec: generalized scrollable regions

## Goal

Make every long panel scroll through one shared, server-laid-out abstraction, so
the filesystem bar and the command palette scroll the way the editor already
does, and so each scrollable region publishes the hit-test classification that
milestone 8 (mouse) needs. Preserve the client-side fuzzy-find latency path for
the palette: typing must never round-trip to scroll.

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
  rows + a `selected` index in `PaletteReport` (`include/ssg/palette.h`).
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

Five independently shippable steps, in order (R2 is split into R2a behavior/render
and R2b wire/hit-map). R1 is a pure refactor with no behavior change; R2a and R3
each end in a hand-testable scrolling panel; R2b and R4 add the M8-facing hit
contract.

## Plan

| Step | Work | Files | Oracle |
|---|---|---|---|
| R1 | Promote `scrollbar_metrics` to public library API (`include/ssg/viewport.h`), and add a pure `compute_list_scroll_view(total_items, viewport_rows, first_visible, selected, keep_selection_visible) -> ListScrollView { first_visible, visible_count, ScrollbarMetrics }`. It always clamps `first_visible` to `[0, maximum_first_row]`. Only when `keep_selection_visible` is true AND `selected` is set does it further adjust `first_visible` minimally so `selected` lies in `[first_visible, first_visible+visible_count)`; when false it honours the clamped `first_visible` verbatim (selection may be off-screen). `selected` is an absolute item index. No caller changes yet (pure addition). | `include/ssg/viewport.h`, `src/viewport.cpp`, `cmake` (if a new TU), `tests/test_viewport.cpp` | unit table over `(total, viewport, first, selected, keep)`: empty; shorter-than-viewport (thumb hidden, `first=0`); exactly full; over-scrolled `first` clamped to `maximum_first_row`; with `keep=true` — selection above/below/inside the window forces the minimal up/down shift; with `keep=false` — the same selection leaves `first_visible` untouched (selection off-screen allowed). In all cases the returned `ScrollbarMetrics` equals `scrollbar_metrics(total, viewport, first')`. Reference-check `compute_viewport`'s existing metrics are byte-identical (no regression). |
| R2a | Filesystem bar scrolls (server-owned, behavior + render). Reserve a 1-column scrollbar gutter for the panel in `compute_shell_layout` (panel `content` = width−1, panel `scrollbar` = right column), mirroring `PaneGeometry`; add a server tree scroll offset; on `TreeModel::select_next/previous` and on tree mutation, recompute the offset via R1 with `keep_selection_visible=true`; window `paint_panel_tree` from `first_visible`; paint the panel scrollbar thumb from the R1 metrics. | `include/ssg/ui_layout.h`, `src/ui_layout.cpp`, `include/ssg/tree.h`, `src/tree.cpp`, `src/render.cpp`, `tests/test_tree.cpp`, `tests/test_render.cpp`, `tests/fixtures/tui/*` | runtime: a tree taller than the panel scrolls so `select_next` past the fold keeps the selected node in the visible window; `select_previous` back above the top scrolls up. render: the panel draws a thumb sized/positioned by R1 when `nodes > rows`, an empty gutter (content width unchanged) when `nodes <= rows`; the selected node is always painted. |
| R2b | Publish the tree hit map + wire it. Add a bounded `viewport_row -> TreeNodeId` hit map (visible window only) plus the tree scroll offset to `TreeProviderView`/`TreeViewState`; serialize the new fields in `src/protocol.cpp` with a round-trip test. | `include/ssg/tree.h`, `src/runtime/snapshot.cpp`, `src/protocol.cpp`, `tests/test_protocol.cpp`, `tests/test_tree.cpp` | hit map: for a scrolled tree, `viewport_row 0..k` map to exactly the on-screen `TreeNodeId`s (absolute index `first_visible + row`); the map length never exceeds the visible window; protocol round-trip preserves the offset + hit map. |
| R3 | Command palette scrolls (client-owned). Client computes its palette scroll view with R1 (`keep_selection_visible=true` on selection move; false on explicit scroll), reports the **windowed** rows, the **absolute** `selected`, `first_visible`, and the resolved `ScrollbarMetrics` in `PaletteReport`; `shell_view` projects them into `PaletteProjection` verbatim; `paint_palette` renders the window and the gutter thumb from the reported metrics, highlighting `selected - first_visible`; content width is unchanged whether or not a thumb shows. **Wire scope:** both `PaletteReport` (a client→server call parameter) and `PaletteProjection` (a render-derived field of `ShellViewState`) are **in-process only** — the palette projection is already excluded from the `ShellViewState` protocol codec and from `shell_equal`, because a remote client owns its own fuzzy-find/report and renders its own palette. R3 keeps this: no new codec, no `shell_equal` change. The client caches the palette pane height from the previous snapshot's `panes.front().content.height` (identical to the editor pane), so the window is resolved against the real height with no cold-start (the editor pane rect exists before the palette opens). | `include/ssg/palette.h`, `include/ssg/ui_layout.h`, `src/runtime/snapshot.cpp`, `src/render.cpp`, `apps/ssg_main.cpp`, `tests/test_render.cpp`, `tests/test_ssg_app.cpp` | app unit: with N ranked candidates and a pane of `rows` < N, the client's reported window is exactly the R1 window around the absolute `selected`, and shrinking N by typing so `N <= rows` hides the thumb without changing `content` width. render: a projection with more rows than fit draws exactly the windowed rows + a correctly sized thumb, highlighting `selected - first_visible`; `selected` stays visible after windowing. PTY: type to produce a long list, arrow past the fold, the list scrolls and the selected row stays on screen. |
| R4 | Uniform hit-test seam (data + pure helper; no input handling). Add a client-side pure `hit_test(SessionSnapshot const&, int column, int row) -> RegionHit` returning `{ kind: editor/panel/palette/scrollbar, item: document offset / TreeNodeId / palette row index, or scrollbar fraction }`, built from the editor `CellHitTarget[]`, the R2 tree hit map, the R3 palette window, and the region rects + `ScrollbarMetrics`. | `include/ssg/hit_test.h` (or `apps/`-local), `src/hit_test.cpp`, `cmake`, `tests/test_hit_test.cpp` | unit: for a known composed snapshot (bar + editor + open palette), a column/row in the editor returns the right document offset; in the bar returns the right `TreeNodeId`; in the palette returns the right row index; on a scrollbar returns the right region + a fraction that round-trips through `scroll_to_fraction`; a cell in the reserved-but-empty gutter and out-of-bounds return "no target". |

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
  exists.
- **Tree hit map size.** Publish only the visible window's row→id entries, not
  the whole tree, to bound snapshot size.
- **R4 placement.** If a future `--http` client needs hit-testing too, `hit_test`
  belongs in the library (`include/ssg/`); if it is only ever a TUI concern, it
  may live app-side. Prefer the library for reuse, consistent with M7-2.
- **Not in scope.** Horizontal scrolling, momentum/smooth scroll, and pointer
  input handling (M8) are out of scope; the model leaves room for them.
