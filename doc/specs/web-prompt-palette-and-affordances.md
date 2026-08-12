# Web prompt/palette UI and the native-affordance boundary

Status: in-flight. Delete on merge; the durable residue lands in the served
client, the host's palette/clipboard bridges, types, tests, and any CONTRACT
line.

## Why this phase

Three user-visible gaps remain after M3, and they share a boundary question — how
much the browser does natively versus what it drives through the library:

1. Opening the file finder or command palette (Alt+P / Alt+Shift+P) moves focus
   into a prompt the web client cannot draw, so input vanishes into an invisible
   query and the editor feels stuck. There is no fuzzy open on the web.
2. The mouse does nothing: clicks, drag-selection, tab and tree clicks, and wheel
   scrolling are unhandled.
3. Copy/paste and selection are split: a semantic (shift+nav) selection and a
   browser-native (mouse) selection are unrelated, and the OS clipboard is not
   bridged to the library's copy/cut.

This phase draws the prompts, wires the mouse to semantic commands, and settles
the clipboard/selection boundary. It ships in three reviewable sub-phases.

## The design choices this spec exists to settle

### 1. Where palette/finder ranking runs for the web client

`PaletteSearcher` — the fuzzy ranker that turns a query plus the published
candidates into an ordered, windowed `PaletteReport` — is a C++ library service.
The TUI app owns the picker query/selection/scroll as a client-owned derived view
and runs `PaletteSearcher` in-process. The browser cannot run C++.

- Option A — the browser reimplements fuzzy ranking in JavaScript. The order in
  which candidates match a query is product behavior (which command "gto" selects
  is a decision, not a rendering), so a second ranker is a second behavior path.
  Rejected.
- Option B — the browser owns only the query text and selection index (the same
  client-owned derived view the TUI app owns), and the **host runs
  `PaletteSearcher` on its behalf**, shipping the resulting `PaletteReport` back
  for the browser to render. One ranker, the library's; the browser is pure
  presentation plus query ownership.

**Decision: Option B.** The candidates already ride the semantic snapshot
(`sections.palette`). When the palette/finder is the active prompt, the browser
holds the query string (append on keypress, backspace, etc. — the seam's existing
`AppendPaletteQuery` result) and its selection index, and sends them to the host;
the host runs `PaletteSearcher::report(candidates, window)` and returns the
`PaletteReport` for the browser to render. Execution and cancellation stay library
commands: Enter dispatches `palette.execute{commandId}` for the selected row;
Escape dispatches `prompt.cancel`; Up/Down move the browser-owned selection index.

#### The envelope framing and request correlation

The current outbound frame is `[u64 settledClientEditId]` followed by an optional
single library message (`[u8 version][u8 kind][value]`). The report cannot be a
second bare message — the browser could not tell it from the library body — so
the envelope becomes explicitly sectioned, still leaving `ProtocolCodec`
untouched:

- Outbound: `[u64 settledClientEditId][u8 sectionCount]`, then `sectionCount`
  sections each `[u8 tag][u32 littleEndianLength][bytes]`. `tag=0` (LibraryBody)
  is the existing `version+kind+value` message; `tag=1` (PaletteReport) is the
  ranked report. A frame may carry zero, one, or both — a keystroke settlement
  with no state change and no open palette is header + `sectionCount=0`.
- The `PaletteReport` section body carries the report fields the client renders
  (query echo, ghost, rows of `{id,label,detail}`, selected index, firstVisible,
  scrollbar) plus the **`paletteRequestId`** it was ranked for.
- Inbound palette request: `PICK:<requestId>:<selected>:<query>` (query last so it
  may contain `:`). `requestId` is a per-connection monotonic counter the browser
  increments on every query or selection change.
- Ordering/staleness: the browser ignores any `PaletteReport` whose
  `paletteRequestId` is older than the highest request it has sent, so an
  out-of-order or slow report never overwrites newer query/selection state. The
  report is advisory presentation for a client-owned query; it never gates the
  semantic document, which continues to arrive as snapshot/delta in `tag=0`.

### 2. What computes a mouse target on the web

The library's `hit_test` maps a monospace **cell** to a `RegionHit`
(Editor+byteOffset, Panel+nodeId, Tab+tabIndex). It is grid geometry, and the
grid is an optional service a native-layout client ignores (a standing rule).

- Option A — the web host feeds the grid hit-tester synthetic cell coordinates.
  This forces the web client back onto a grid it does not use and re-imposes the
  geometry the reframe removed. Rejected.
- Option B — the browser maps a DOM pointer position to a **document byte
  offset** using its own layout (the caret position from the DOM range, converted
  UTF-16→UTF-8 with the existing offset code), and dispatches the same semantic
  command a keyboard route would. A tab click reads the clicked tab element's
  index; a tree row click reads its node id.

**Decision: Option B.** The web client owns its geometry and derives semantic
targets from the DOM, never from the grid. Every pointer gesture resolves to an
existing library command already on the one behavior path — the same commands the
TUI's `pointer_routing` dispatches:

- Click in the document → `cursor.set_position` with `SelectionCommandArguments`
  carrying the byte offset (DOM caret position → UTF-16 index → byte offset via
  the existing conversion).
- Drag in the document → `select.set_range` (anchor = press offset, active =
  current offset); double-click → `select.word_at_position`.
- Tab click → resolve the clicked tab element's index to
  `sections.tabs.tabs[index].id` (a `TabId`) and dispatch `tab.activate{tabId}`;
  the close control → `tab.close{tabId}`. Neither takes a bare index.
- Tree row click → `tree.select{nodeId}` then `tree.activate` (no payload), from
  the row's node id.
- Scrolling is **native on the web**: the document and tree surfaces are DOM
  overflow the browser scrolls, so no wheel gesture dispatches a scroll command.
  Reveal-on-edit is honoured by the client calling `scrollIntoView` on the caret
  element after an authoritative update, not by the library's grid scroll. The
  library's `view.scroll_to_fraction` / `tree.scroll_to_fraction` are grid-scroll
  projection and are not used by a native-layout client; a future scrollbar-thumb
  affordance, if wanted, is native DOM scrolling too. This resolves the
  native-vs-library-scroll choice: the web owns scroll geometry, the library owns
  only caret/selection semantics.

No new product behavior is invented on the pointer path; each gesture names an
existing command and payload type.

### 3. Selection and clipboard: one model, bridged both ways

Today shift+nav yields a semantic selection (the library's `SelectionSet`,
rendered via the theme's selection role) and a mouse drag yields a browser-native
DOM selection; they are unrelated, and copy/cut of the semantic selection
(`ClipboardViewState.systemWrite`) never reaches the OS clipboard.

- Option A — keep the browser-native DOM selection as the document's selection.
  Familiar browser copy, but the editor cannot see it, so no editor command
  (copy into the register, delete, replace, multi-cursor) acts on it, and two
  highlights overlap. Rejected for the document surface.
- Option B — the **semantic selection is the document's one selection**. A mouse
  drag drives the semantic `SelectionSet` (the same model shift+nav drives),
  rendered the same way; browser-native selection on the document surface is
  suppressed so there is one highlight. Copy is bridged **out** (the library's
  `systemWrite` → `navigator.clipboard.writeText`) and paste is bridged **in**
  (a browser `paste` event → `text.insert`), so the OS clipboard works both
  directions without a second selection.

**Decision: Option B.** One selection model, consistent across keyboard and
mouse, on which every editor command operates; the OS clipboard is bridged rather
than owned. This is the sense in which "the web does not need app-provided
clipboard": the library's clipboard register is not the browser's clipboard — it
is bridged to the real one. Browser-native selection remains available on chrome
outside the document surface (status text, labels) where it is harmless.

#### The event and focus contract

Ctrl/Meta *keydown* passes through to the browser (shipped), which is what lets
the browser *generate* the `copy`/`cut`/`paste` clipboard events. The client does
not intercept the keydown; it handles the resulting **clipboard events** on the
document surface, so copy is not left broken by the suppressed native selection:

- The document surface is the `#doc` element. `user-select: none` is set on
  `#doc` only, so the semantic selection is the sole highlight there while status
  text and labels stay natively selectable. `selectstart`/`dragstart` on `#doc`
  are prevented so a drag builds the semantic selection rather than a native one.
- `copy`/`cut` over `#doc`: the client already knows the selected text (it renders
  the semantic selection from the document + `SelectionSet`), so it writes that
  text into `event.clipboardData` and calls `preventDefault` — copy works with no
  native selection. It also dispatches `clipboard.copy` (and `cut` additionally
  performs the delete) so the library register stays in sync; the register's
  `systemWrite` is a redundant confirmation path, not the source of the copied
  text.
- `paste` over `#doc`: the client reads `event.clipboardData` text and dispatches
  a caret-anchored `text.insert` (predicted like typing) — or `clipboard.paste`
  where the register content is authoritative — and calls `preventDefault`.

So keydown passthrough and clipboard bridging are complementary, not
contradictory: the browser owns the shortcut, the client owns the clipboard event
over the editor surface.

## Client ownership boundary

The browser owns: the palette query text and selection index (a client-owned
derived view), DOM geometry and the pointer→offset mapping, the OS-clipboard
bridge, and all rendering. It owns no product behavior — ranking is the library's
`PaletteSearcher`, every pointer gesture and palette action is a library command
on the one behavior path, and the authoritative state still arrives as a snapshot
or delta. The host owns identity/capabilities, the runtime seam, delta
derivation, and now the per-connection `PaletteSearcher` invocation.

## Plan

Files: the host framing and `PaletteSearcher` invocation in
`apps/http_serve.cpp`; the browser request/apply/render path in
`apps/web/client.mjs` with the pure envelope-section parse, stale-report guard,
and pointer→offset mapping in `apps/web/reconcile.mjs`; oracles in
`tests/web/test_reconcile.mjs` and a host-side test alongside the existing app
tests.

### Phase A — prompt/palette UI
- Host: when the active prompt is the palette/finder, run
  `PaletteSearcher::report(sections.palette.candidates, window)` for the
  connection's query+selection and emit a `tag=1` PaletteReport section (with the
  echoed `paletteRequestId`) alongside the `tag=0` library body. Inbound
  `PICK:<requestId>:<selected>:<query>` frames carry the browser's query and
  selection.
- Client: render the header query line and candidate rows from the report
  (label + detail + selection highlight + scrollbar), themed by role; ignore a
  report whose `paletteRequestId` is stale. Editing the query (append/backspace)
  updates the browser-owned query and sends a new request; Up/Down move the
  selection; Enter dispatches `palette.execute`; Escape dispatches
  `prompt.cancel`. Draw the find/replace footer prompts from the already-shipped
  `findReplace` state.
- Oracles: the section framing + stale-report guard (a report with an older
  `paletteRequestId` must not overwrite newer query/selection state) as a pure
  node test; a host test that a query yields the library ranker's order, so the
  browser is proven never to rank.

### Phase B — mouse/pointer
- Client: map a pointer position to a document byte offset via the DOM range and
  the existing UTF-16↔UTF-8 conversion, then dispatch the named command per
  gesture — `cursor.set_position`, `select.set_range`,
  `select.word_at_position`, `tab.activate{tabId}`/`tab.close{tabId}` (index
  resolved to `sections.tabs.tabs[index].id`), `tree.select{nodeId}` then
  `tree.activate`. Scrolling is native DOM overflow; reveal-on-edit is
  `scrollIntoView` on the caret element, so no scroll command is dispatched.
  `user-select: none` on `#doc` and prevented `selectstart`/`dragstart` keep the
  semantic selection the sole document highlight.
- Oracles: the pointer→offset mapping over multi-byte and wrapped text (pure, in
  reconcile.mjs); a gesture→command-id mapping test pinned like the input-routing
  parity test, asserting each gesture names an existing command with the correct
  payload type (a `TabId`, not an index).

### Phase C — clipboard bridge
- Client: `copy`/`cut`/`paste` listeners on `#doc`. `copy` writes the
  semantic-selection text to `clipboardData`, `preventDefault`, and dispatches
  `clipboard.copy`; `cut` additionally performs the library delete; `paste` reads
  `clipboardData` and dispatches a caret-anchored `text.insert`. The register's
  `systemWrite` is forwarded to `navigator.clipboard` as a redundant confirmation,
  de-duplicated on the write id.
- Oracle: the systemWrite→clipboard de-duplication (keyed on the write id, so one
  copy pushes once) as a pure test.

## What survives this spec

- The palette query-ownership + host-ranker seam → the report request/response
  types and a test that the browser never ranks. A CONTRACT line only if no test
  name carries it.
- The pointer→semantic-command mapping → tests; the "web derives targets from the
  DOM, not the grid" rule is a standing invariant already, reinforced by a test
  that the web path calls no grid hit-tester.
- The clipboard bridge (systemWrite out, paste in; one selection model) → tests
  and, if load-bearing, a CONTRACT line on the boundary that the library's
  clipboard register is bridged, not authoritative, for the web.
- Everything else — the phase list, the rejected options — is git history.

## Explicitly out of scope

IME/composition input, touch gestures, accessibility semantics, multi-client, and
the configurable chord-modifier setting (Alt-vs-Ctrl) — each its own later phase.
