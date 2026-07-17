# Spec: mouse input (M8)

## Goal

Deliver milestone 8: the pointer supplements the keyboard without ever becoming
the only path. Click to place the caret, drag to select, click tabs / tree nodes
/ palette rows, drag the editor scrollbar, and wheel-scroll. All pointer
classification goes through the library `hit_test` seam from the scroll refactor
(R4); the `ssg` app owns only the terminal-specific decode of mouse escape
sequences and the routing of a `RegionHit` to a command.

## Current state (facts)

- **The terminal already enables mouse reporting and decodes the wheel.** The app
  writes `\x1b[?1049h\x1b[5 q\x1b[?1000h\x1b[?1006h` on entry
  (`apps/ssg_main.cpp`): `1000h` = button tracking, `1006h` = SGR extended
  reporting. `decode_input` (`apps/ssg_terminal.cpp`) parses the SGR form
  `ESC [ < Cb ; Cx ; Cy (M|m)` but **only** recognizes the wheel buttons
  (`Cb == 64` up, `65` down), returning `DecodeStatus::scroll` with a signed line
  count; the legacy X10 form `ESC [ M b x y` likewise handles only the wheel. All
  other button codes fall through to `DecodeStatus::none` and are dropped. The
  main loop dispatches `view.scroll_lines` on `DecodeStatus::scroll`. **Button
  press/release/drag are not decoded at all, and `1002h` (button-event motion,
  needed for drags) is not enabled.**
- **`DecodeStatus` / `Decoded`** (`apps/ssg_terminal.h`) are: `enum class
  DecodeStatus { none, incomplete, key, scroll }` and `struct Decoded { status,
  KeyStroke stroke, string text, int64 scroll }`. There is no field for a pointer
  position or button.
- **The `hit_test` seam exists (R4).** `hit_test(SessionSnapshot const&, int
  column, int row) -> RegionHit` (`include/ssg/hit_test.h`) classifies a cell as
  `editor` (+ `byte_offset`/`byte_len`), `panel` (+ `TreeNodeId node_id`),
  `palette` (+ absolute `item_index`), or a `*_scrollbar` (+
  `scroll_numerator`/`scroll_denominator`/`scrollbar_fraction`), else `none`. It
  does **not** classify the tab bar (returns `none` over tabs) — R4 covered only
  the scrollable content regions and gutters.
- **Commands a pointer drives already exist, except tree-by-id.**
  - Caret: `cursor.set_position` with `SelectionCommandArguments{position,
    nullopt}`. A byte offset becomes a `DocumentPosition` via
    `resolve_document_position(text, ByteOffset)` (`include/ssg/selection.h`); the
    document text is on the snapshot (`sections().document.text`).
  - Selection: `select.set_range` with `SelectionCommandArguments{nullopt,
    Selection{anchor, active}}` (two `DocumentPosition`s); the handler validates
    both positions against the document.
  - Editor scroll-to: `view.scroll_to_fraction` with
    `ScrollFractionArguments{numerator, denominator}` → `first_row =
    maximum_first_row * numerator / denominator`. `hit_test`'s editor-scrollbar
    result yields exactly these integers.
  - Wheel: `view.scroll_lines` with `ScrollLinesArguments{lines}` (already wired).
  - Tab: `tab.activate` with an optional `TabId` payload (absent → active). The
    ordered `sections().tabs.tabs` vector lets the client recover the `TabId` for
    a clicked tab index.
  - Palette: `palette.execute` with `PaletteExecuteArguments{command_id}`, already
    client-fulfilled from the ranked candidate list.
  - **Tree: no command selects/activates a specific node.** `tree.activate` acts
    on the current selection (moved only by `tree.select_next/previous`);
    `tree.toggle_expanded`/`tree.invoke_node_command` take a
    `TreeCommandInvocation{provider_id, node_id, command_id}`. There is **no**
    `tree.select` that takes a `TreeNodeId`, so a clicked tree node cannot be
    acted on. M8 adds one.
- **The tab bar publishes per-tab rects.** `compute_shell_layout`
  (`src/ui_layout.cpp`) adds an `AccessibilityNode{ShellNodeKind::tab,
  id="tab.{i}", rect}` per visible tab. The id encodes the index into
  `sections().tabs.tabs`.
- **The main loop already refreshes a snapshot before each input event**
  (`refresh()` in `apps/ssg_main.cpp`), so a fresh snapshot is available to
  `hit_test` at the moment a pointer event is handled, and the app knows the
  terminal size via `terminal_size()`.

## Design

### Ownership

- **Library owns classification and behavior.** `hit_test` maps a cell to a
  region + item; the runtime owns caret/selection/scroll/tab/tree behavior behind
  existing (and one new) commands. M8 adds **no** editor behavior to the app.
- **App owns terminal decode + routing only.** The app decodes the SGR/X10 mouse
  sequences into a neutral `PointerEvent`, and translates a `(RegionHit, button,
  kind)` into a command dispatch. This is the same "app owns terminal I/O"
  boundary as the keyboard decoder.

### Pointer decode (app)

Extend the terminal decoder to recognize button press/release/drag, not just the
wheel:

- Enable `1002h` (button-event motion) alongside the existing modes, so the
  terminal reports motion **while a button is held** (drags) without flooding
  motion events when no button is down. Disable it with the other modes on exit.
- Add `DecodeStatus::pointer` and a `PointerEvent { int column; int row;
  PointerButton button; PointerKind kind; }` to `Decoded`, where `PointerButton`
  is `{left, middle, right, other}` and `PointerKind` is `{press, release,
  drag}`. Columns/rows are converted from the terminal's 1-based SGR coordinates
  to the 0-based grid `hit_test` expects.
- SGR decode (`ESC [ < Cb ; Cx ; Cy (M|m)`): the low two bits of `Cb` select the
  button; bit 5 (`32`) marks motion (a drag when a button is held); the final
  byte is `M` for press/drag and `m` for release. The wheel bits (`64`/`65`)
  keep returning `DecodeStatus::scroll` unchanged. The legacy X10 form keeps
  wheel-only handling (SGR is what modern terminals send under `1006h`); a
  non-wheel X10 button is decoded best-effort or dropped as today — SGR is the
  contract we target.
- Partial sequences return `DecodeStatus::incomplete` at every parameter boundary
  (same discipline as the existing arrow/modified-key decode).

### Routing (app)

On a `DecodeStatus::pointer` event the app takes the already-refreshed snapshot,
calls `hit_test(snapshot, column, row)`, resolves the caller-side lookups (the
document position for an editor hit, the candidate id for a palette hit, the
`TabId` for a tab hit), then calls the pure `route_pointer` helper (below) to get
the command to dispatch. The mapping `route_pointer` implements:

- **Left press on `editor`** → `cursor.set_position` with
  `resolve_document_position(document.text, ByteOffset{byte_offset})`. Also record
  a client-local **drag anchor** = that `DocumentPosition` and enter a "dragging"
  mode.
- **Left drag while dragging** (any `editor` cell) → `select.set_range` with
  `Selection{anchor, active}` where `active` = `resolve_document_position` of the
  hit cell. A drag over a cell with no document target (past a short line's end,
  a blank row, or **beyond the viewport edge**) dispatches nothing, so the
  selection holds at the last in-viewport position. **M8-S is within-viewport
  only:** dragging past the top/bottom edge does not scroll the document —
  timer-driven edge auto-scroll is a separate step (M8-S2, below), because it
  requires a periodic wake in the event loop rather than a per-event mapping.
- **Left release** → end dragging (no dispatch; the last `set_range`/`set_position`
  already reflects the state).
- **Left press on `editor_scrollbar`** → `view.scroll_to_fraction` with the hit's
  `scroll_numerator`/`scroll_denominator`. **Left drag on `editor_scrollbar`** →
  the same, each motion, so dragging the thumb scrolls live. (This reuses the
  server offset; the drag anchor is not used here.)
- **Left press on a `tab`** → `tab.activate` with the `TabId` recovered from
  `sections().tabs.tabs[index]`.
- **Left press on `panel`** → `tree.select` with the hit's `node_id` (new command,
  below), then — matching the keyboard `tree.activate` behavior — a second
  dispatch of `tree.activate` so a click both selects and opens/expands the node,
  exactly as `Enter` does on the keyboard. (A future refinement could split
  single- vs double-click; M8 treats a click as select-then-activate.)
- **Left press on a `palette` row** → `palette.execute` with the candidate id the
  client holds for that absolute `item_index` (the client already owns the ranked
  order, so it maps `item_index` → candidate id and dispatches, exactly as
  `Enter` does).
- **Wheel** (`DecodeStatus::scroll`) → routed by the region under the pointer via
  the pure `route_wheel(HitRegion) -> WheelTarget` helper (`WheelTarget` ∈
  {`editor`, `tree`, `palette`, `none`}): a wheel over `panel`/`panel_scrollbar`
  → `WheelTarget::tree` → dispatch `tree.scroll(delta)` (scrolls the tree viewport
  without moving the selection); a wheel over `palette`/`palette_scrollbar` →
  `WheelTarget::palette` → the app scrolls its **client-owned** palette window
  (no command — the palette scroll offset lives only in the app; see M8-P and
  doc/spec-scroll.md R3); every other region — editor, editor gutter, tab bar,
  `none`, or no snapshot — → `WheelTarget::editor` → dispatch
  `view.scroll_lines(delta)`. The wheel decode carries the pointer position for
  this. The helper is pure and unit-tested per region, mirroring `route_pointer`.
- Any other `(region, button, kind)` (right/middle click, press on `none`) is a
  no-op in M8.

The drag anchor and dragging flag are client-local state in `apps/ssg_main.cpp`,
alongside the existing palette/find client state; nothing about the drag is
server state (the server only ever sees `cursor.set_position` then a sequence of
`select.set_range`).

### New library pieces

1. **A typed tab hit map, published by layout.** Rather than have `hit_test` parse
   the stringly-typed `tab.{i}` accessibility-node id, `compute_shell_layout`
   publishes a typed `std::vector<TabHit{ Rect rect; std::uint32_t index; }>` on
   `ShellViewState` (one per visible tab, `index` into `sections().tabs.tabs`).
   `hit_test` matches a cell against these rects and returns `HitRegion::tab` with
   `tab_index`. This mirrors the tree's typed `visible_node_ids` hit map and
   avoids an implicit id-format contract between layout and input. The tab hit map
   crosses the wire as part of `ShellViewState` (protocol codec + `shell_equal`
   coverage + round-trip test), consistent with the panel-scrollbar discipline.
   The app maps `tab_index` → `TabId` from the snapshot.
2. **`tree.select` command with a dedicated `TreeNodeId` payload.** A new command
   `tree.select` whose argument is a bare `TreeNodeId` (not the
   `TreeCommandInvocation` triple — a click needs only the node id, and a
   dedicated payload keeps the argument shape minimal and validated). The handler
   sets the active provider's selection to that node, rejecting an id absent from
   the active provider, and calls `reveal_tree_selection` so the choice stays in
   view. Wire/catalog cascade like any client-fulfilled argument command
   (`keymap:false`, `palette:false`, `lua:true`): a `TreeSelectArguments{TreeNodeId
   node_id}` payload with a protocol codec + round-trip (reusing the existing
   `TreeNodeId` codec), plus `data/required-commands.json`,
   `test_required_commands.cpp` (list + count + surface-exclusion branch), and
   `command_cases.h`.

### Routing helper (app, unit-testable)

To give the `(RegionHit, button, kind) → command` mapping a strong, deterministic
oracle, the routing decision is a **pure function** in the app, separate from the
I/O loop:

```
struct PointerCommand {             // one command to dispatch
    std::string command_id;
    std::any payload;               // the typed argument, or empty for none
};
struct PointerDispatch {            // what the loop should dispatch, or nothing
    std::vector<PointerCommand> commands;  // 0, 1, or (for a tree click) 2,
                                           // dispatched in order
    bool begins_drag = false;       // press that starts an editor drag
    bool ends_drag = false;         // release that ends a drag
};
PointerDispatch route_pointer(RegionHit const& hit, PointerButton button,
                              PointerKind kind, bool dragging,
                              DocumentPosition const& hit_position,  // resolved by caller
                              std::optional<DocumentPosition> drag_anchor,
                              /* candidate id / tab id lookups */ ...);
```

The exact signature is an implementation detail, but the contract is: **a pure
function that, given a classified hit + the button/kind + the current drag state
(and the caller-resolved document position / candidate / tab id), returns an
ordered list of commands to dispatch (empty for none) and how the drag state
changes.** The list is a sequence because a single pointer event can drive more
than one command — a tree-row click returns `[tree.select(node_id),
tree.activate]`, dispatched in order; every other event returns zero or one
command. The I/O loop does only: decode → refresh snapshot → `hit_test` → resolve
the document position/candidate/tab id → `route_pointer` → dispatch each command
in order. Every M8 routing oracle tests `route_pointer` directly with synthesized
inputs (asserting the exact command sequence); `test_tui_fixture`/PTY are used
only for end-to-end confirmation, not as the primary correctness oracle.

### Deferred (stated, not implemented)

- **Panel and palette scrollbar *drag*.** `hit_test` already classifies the panel
  and palette gutters, but there is no free-scroll command for the tree (R2 only
  scrolls it to keep the selection visible) and the palette's explicit-scroll path
  was deferred in R3. So M8 wires scrollbar drag for the **editor only**; a click
  or drag on the panel/palette gutter is a no-op. Implementing those needs a tree
  `view.scroll_to_fraction` equivalent and a client palette explicit-scroll,
  which are follow-ups to the scroll spec, not M8.
- **Double-click / click-count semantics** (word/line select), **right-click
  context menus**, and **hover**. Out of scope; the decode leaves room (the
  `PointerEvent` carries the button and kind).

## Plan

| Step | Work | Files | Oracle |
|---|---|---|---|
| M8-D | Decode SGR button press/release/drag into `DecodeStatus::pointer` + `PointerEvent{column,row,button,kind}`; enable/disable `1002h`; wheel decode unchanged; 0-based coordinate conversion; incompleteness at every boundary | `apps/ssg_terminal.{h,cpp}`, `tests/test_ssg_app.cpp` | decode table (exact `PointerEvent`s) for: left/middle/right press (`M`), release (`m`), drag (button+`32`, `M`), wheel still → `scroll`, out-of-order/partial split reads → `incomplete` at each parameter boundary; a 1-based `(1,1)` SGR coord decodes to grid `(0,0)` |
| M8-C | Left click on the editor places the caret: introduce the pure `route_pointer` helper; route `pointer` press on `editor` → `cursor.set_position(resolve_document_position(document.text, byte_offset))` | `apps/ssg_main.cpp` (+ a small routing header/TU the test links), `tests/test_ssg_app.cpp` | `route_pointer` unit tests: a left press on an `editor` hit returns `cursor.set_position` with the resolved position; a press on `none`/`tab`/header returns no command; wired end-to-end, a synthesized left press moves the caret |
| M8-S | Drag selects (within-viewport): press records a client anchor + dragging mode (`route_pointer` sets `begins_drag`); each drag → `select.set_range{Selection{anchor, active}}`; release clears dragging (`ends_drag`). A drag over a cell with no document target (short line, blank row, past the viewport edge) dispatches nothing | `apps/ssg_main.cpp` (routing), `apps/pointer_routing.{h,cpp}`, `tests/test_ssg_app.cpp` | `route_pointer` unit tests: press returns set_position + begins_drag; a subsequent drag with an anchor returns `select.set_range` with `Selection{anchor, active}`; release returns ends_drag + no command; drag with no anchor / non-editor hit / unresolved position returns no command; end-to-end press-then-drag spans the two cells |
| M8-S2 | Edge auto-scroll during drag (event-loop timer): while a drag is active and the pointer is at/beyond the top or bottom of the editor content, wake the loop periodically (bounded `input_ready` timeout) to dispatch `view.scroll_lines(±1)` and re-extend the selection to the new edge, so a drag past the fold scrolls and keeps selecting. No pointer event is required to keep scrolling (the mouse may hold still at the edge) | `apps/ssg_main.cpp` (event loop: timed read while edge-dragging), `tests/test_ssg_app.cpp` (a pure edge-scroll-decision helper) | a pure helper `edge_scroll(drag active, pointer row, editor content rect) -> optional<int lines>` returns −1 above the top, +1 below the bottom, none inside; unit-tested at both edges and the interior; end-to-end (PTY) a drag held past the bottom keeps scrolling and extending the selection |
| M8-B | Editor scrollbar click/drag scrolls: route press/drag on `editor_scrollbar` → `view.scroll_to_fraction{numerator, denominator}` from the hit; panel/palette gutter → no command | `apps/ssg_main.cpp` (routing), `tests/test_ssg_app.cpp` | `route_pointer` unit tests: an `editor_scrollbar` hit at the bottom (num==denom) returns `view.scroll_to_fraction` yielding `maximum_first_row`, at the top yields `0`; a `panel_scrollbar`/`palette_scrollbar` hit returns no command; end-to-end drag on the editor gutter scrolls |
| M8-T | Tabs + palette clicks: layout publishes a typed `TabHit{rect,index}` list on `ShellViewState` (codec + `shell_equal` + round-trip); `hit_test` gains `HitRegion::tab` + `tab_index`; route tab press → `tab.activate(TabId from tabs[index])`, palette-row press → `palette.execute(candidate id for item_index)` | `include/ssg/ui_layout.h`, `src/ui_layout.cpp`, `include/ssg/hit_test.h`, `src/hit_test.cpp`, `src/protocol.cpp`, `src/session_snapshot.cpp`, `tests/test_hit_test.cpp`, `tests/test_protocol.cpp`, `tests/test_editor_session_assembly.cpp`, `apps/ssg_main.cpp`, `tests/test_ssg_app.cpp` | `hit_test` over a tab cell returns `tab` + the right index; padding on the tab bar → `none`; `TabHit` round-trips + a tab-hit-only shell change is not suppressed by `shell_equal`; `route_pointer` maps a tab hit → `tab.activate(TabId)` and a palette hit → `palette.execute(id)`; end-to-end a tab click activates it |
| M8-R | Tree node click: add `tree.select` with a `TreeSelectArguments{TreeNodeId}` payload (`keymap:false`/`palette:false`/`lua:true`) + catalog cascade + protocol round-trip; route panel press → `tree.select(node_id)` then `tree.activate` | `include/ssg/tree.h`, `src/tree.cpp`, `src/runtime/navigation.cpp`, `data/required-commands.json`, `tests/test_required_commands.cpp`, `tests/runtime/command_cases.h`, `src/protocol.cpp`, `tests/test_protocol.cpp`, `apps/ssg_main.cpp`, `tests/runtime/test_runtime_navigation.cpp`, `tests/test_ssg_app.cpp` | runtime: `tree.select(node_id)` makes that node the selection (and rejects an unknown id); `route_pointer` maps a panel hit → the ordered pair `[tree.select(node_id), tree.activate]`; end-to-end a click on a tree row selects that node and then activates it (opens a file / toggles a directory) as the keyboard select+activate does |
| M8-W | Wheel routes by region so the side panel (tree) scrolls: the wheel decode carries the pointer position; add a `tree.scroll` command (`ScrollLinesArguments{rows}` payload, `keymap:false`/`palette:false`/`lua:true`) that adjusts the server-owned `tree_first_visible` by `rows` clamped to `[0, maximum_first_row]` WITHOUT moving the tree selection (the editor's `view.scroll_lines` analog); route a wheel over `panel`/`panel_scrollbar` → `tree.scroll(delta)`, `palette`/`palette_scrollbar` → no-op (inert overlay; client-owned palette wheel is a follow-up), every other region (editor, gutter, tab bar, none) → `view.scroll_lines(delta)` unchanged | `apps/ssg_terminal.{h,cpp}`, `include/ssg/tree.h`, `src/tree.cpp`, `src/runtime/navigation.cpp`, `src/runtime/editor_runtime_internal.h`, `data/required-commands.json`, `tests/test_required_commands.cpp`, `tests/runtime/command_cases.h`, `src/protocol.cpp`, `apps/pointer_routing.{h,cpp}`, `apps/ssg_main.cpp`, `tests/test_tree.cpp`, `tests/runtime/test_runtime_navigation.cpp`, `tests/test_ssg_app.cpp` | decode: an SGR wheel event decodes to `scroll` with the delta AND the 0-based grid position; runtime: `tree.scroll(+n)` advances `tree_first_visible` clamped to `maximum_first_row` and `tree.scroll(-n)` clamps at 0, and it does NOT change the selection (a later `reveal_tree_selection` still re-snaps); a pure `route_wheel(HitRegion) -> optional<command id>` returns `tree.scroll` for `panel`/`panel_scrollbar`, nothing for `palette`/`palette_scrollbar`, and `view.scroll_lines` otherwise (unit-tested per region) *(superseded by M8-P: `route_wheel` now returns a `WheelTarget` enum — see M8-P and the Wheel bullet)*; end-to-end (PTY) the wheel over the panel scrolls the tree without moving selection, and over the editor still scrolls the document |
| M8-P | Client-owned palette wheel scroll: a wheel over the open palette scrolls its list. The palette scroll offset (`palette_first_visible`) is app-local, and `build_report` currently re-snaps it to the selection every frame (`keep_selection_visible=true`), so a free scroll cannot persist. Refactor the palette keep-visible discipline to mirror the tree: introduce a `reveal_palette_selection()` client helper that resolves the window with `keep_selection_visible=true` and is called ONLY when the selection changes (`palette.next`/`palette.previous`, palette open, printable typed, backspace); make the per-frame `build_report` resolve with `keep_selection_visible=false` (honor the offset, just clamp + window). **Shrink-clamp path:** when `build_report`'s defensive clamp of `palette_selected` (results shrank under the selection, e.g. the server republished fewer candidates) actually moves the selection, that frame resolves with `keep_selection_visible=true` so the forced-new selection is not left off-screen; a frame that does not move the selection honors the free offset. Change `route_wheel` to return a `WheelTarget` enum ({`editor`,`tree`,`palette`,`none`}); the app maps `palette` → a client `scroll_palette(delta)` that adjusts `palette_first_visible` clamped to `[0, maximum_first_row]` (saturating, since the delta is an `int64`) WITHOUT moving the selection, gated on `palette_open`. `editor`→`view.scroll_lines`, `tree`→`tree.scroll`, `none`→nothing (unchanged). | `apps/pointer_routing.{h,cpp}`, `apps/ssg_main.cpp`, `tests/test_ssg_app.cpp` | `route_wheel` returns `WheelTarget::palette` for `palette`/`palette_scrollbar`, `tree` for `panel`/`panel_scrollbar`, `editor` for editor/gutter/tab/none (unit-tested per region); keyboard palette navigation still keeps the selection visible after the `build_report` keep-visible move to `reveal_palette_selection` (existing palette scroll behavior unchanged: arrowing past the window edge still scrolls); end-to-end (PTY) with the palette open, a wheel over it scrolls the candidate list (windowed rows shift) and the selection can leave the viewport, while a wheel over the editor region is inert to the palette |

## Invariants and fit

- **M7-2 / library owns behavior.** All caret/selection/scroll/tab/tree *behavior*
  stays in the runtime behind commands; `hit_test` (library) owns pointer
  classification. The app adds only terminal decode + a `(RegionHit) → dispatch`
  switch — no editor logic. **One deliberate exception, predating M8:** the command
  palette is a client-owned overlay (query, selection, and scroll offset are
  app-local, ranked against the server's published candidates — doc/spec-scroll.md
  R3). So the palette *scroll offset* (`palette_first_visible`) is the single scroll
  state the app owns; every other scroll (editor, tree) stays server-side behind a
  command. M8-P scrolls that already-client-owned offset directly, adding no editor
  logic and no server command.
- **Keyboard remains sufficient.** Every mouse action maps to a command already
  reachable by keyboard, or to a client/pointer-fulfilled convenience command
  whose *outcome* is already keyboard-reachable. Two such carve-outs exist:
  `tree.select` (a click selects a node by id; the keyboard selects the same nodes
  via `tree.select_next`/`tree.select_previous`) and `tree.scroll` (a wheel scrolls
  the tree viewport; the keyboard reaches every node — and thus scrolls the view —
  via `tree.select_next`/`tree.select_previous`, which move the selection and
  `reveal_tree_selection`). Both are `keymap:false`/`palette:false`/`lua:true`. The
  palette wheel (M8-P) likewise mirrors keyboard palette navigation (arrow keys move
  the selection and scroll the window). The pointer is purely an alternate input,
  never the only path.
- **Single source of truth.** The drag anchor and the client-owned palette
  offset (pre-existing) are the only client scroll/selection state; the
  authoritative editor/tree/tab/selection state stays server-side. The palette
  candidate mapping reuses the client's existing ranked order.
- **Snapshot/delta discipline.** The new published/argument types
  (`tree.select`'s `TreeSelectArguments{TreeNodeId}`; `tree.scroll`'s reused
  `ScrollLinesArguments`; the `ShellViewState` tab hit map) each get a protocol
  codec + round-trip test and, for the shell field, `shell_equal` coverage, per the
  existing wire discipline.
- **Pit of success.** Button routing is a single pure `route_pointer` over
  `RegionHit.region` and wheel routing a single pure `route_wheel(HitRegion)`, both
  unit-tested in isolation; adding a future clickable/scrollable region means
  teaching `hit_test` about it and adding one routing case, not threading logic
  through the I/O loop.

## Considerations and risks

- **Coordinate origin.** SGR reports 1-based `(Cx, Cy)`; `hit_test` and the grid
  are 0-based. The decoder converts once, so routing never re-derives it.
- **Drag flooding.** `1002h` reports motion only while a button is held, bounding
  drag events to actual drags; each maps to one `select.set_range`. If this proves
  chatty, the app may coalesce consecutive drag events between snapshots (the loop
  already coalesces buffered input) — an optimization, not required for
  correctness.
- **Edge auto-scroll is M8-S2, not M8-S.** `hit_test` returns `none` for a cell
  past the last line or below the pane, and `1002h` reports motion only while the
  pointer *moves*, so a within-viewport drag (M8-S) cannot scroll: it holds the
  selection at the last in-viewport position. Continuous auto-scroll — where a
  drag held still at the bottom edge keeps scrolling — needs a periodic wake in
  the event loop (the loop currently blocks on `::read`; it already has
  `input_ready(timeout)` for Escape disambiguation, so a timed read is feasible).
  Because that is an event-loop change with a loop-behavior oracle rather than a
  pure-`route_pointer` mapping, it is a dedicated step (M8-S2) after M8-S, keeping
  each step hand-validatable.
- **Click = select + activate for the tree.** This mirrors `Enter` (select then
  open/expand). It means a single click on a directory toggles it and on a file
  opens it. If that feels too eager, a later refinement can distinguish single
  click (select only) from double click (activate); the decode already carries
  enough to add click-counting without reworking routing.
- **Palette item mapping.** The client maps the clicked absolute `item_index` to a
  candidate id through its own ranked `order` (the same structure
  `execute_selected_candidate` uses); if the click index is out of range (stale
  snapshot), it is a no-op.
- **Tab index vs id.** The layout publishes a typed `TabHit{rect, index}` list, so
  `hit_test` returns a tab index without parsing any id string; the client
  recovers the `TabId` from `sections().tabs.tabs[tab_index]`. An out-of-range
  index (stale snapshot) is a no-op.
- **Not in scope.** Panel/palette scrollbar drag, double-click semantics,
  right-click menus, hover, and mouse-driven resize are out of scope (see
  Deferred).
