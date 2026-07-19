# Milestones

TUI-first `ssg` editor. The library owns all behavior, layout, and state; the
`ssg` app owns only terminal I/O (drawing cells, colors, keyboard, mouse,
resize). Every milestone ends in something you can run and validate by hand.

Status legend: **DONE** · **IN PROGRESS** · **PLANNED**. Delivered work lists
the spec (if any) and the landing commits, most-recent last.

Layout the app renders (all geometry comes from the library):

```
+--------------------------------------------------+  row 0: header
| bar (filesystem) | tabs                          |
| tree        | sb | doc-view              | sb    |  rows 1..H-2: content
| ...         |    | ...                   |       |
+--------------------------------------------------+  row H-1: footer
```

`bar` = tree columns + 1 scrollbar column; `doc-view` = text columns + 1
scrollbar column. The bar and the document view use the same scrollbar
abstraction.

## 1. Launch and frame — DONE
`ssg [path]` starts, draws the full shell, and quits with `ESC Q`.
- No arg or a directory: open that directory (CWD if none). File: open its
  parent directory and that file in a tab.
- Header row, filesystem bar, document area (empty state or the file), footer
  row are all drawn from the library snapshot.

Demo: `ssg`, `ssg src`, `ssg README.md` — see the VSCode-like frame; `ESC Q`
restores the terminal cleanly.

Delivered:
- `ssg` binary (`apps/ssg_main.cpp`, `apps/ssg_terminal.{h,cpp}`), termios raw
  mode, reuses the library renderer — `f44bd93`.
- Readable 16-color dark theme replacing the grayscale placeholder — `08a50b9`.
- Build/tooling groundwork: Ninja + ccache presets (`1e112fa`); removed the
  browser client and browser conformance tests (`96bfe4c`).

## 2. Read-only document view — DONE
Open a file and see its text rendered with the 16-color theme, scrollable.
- Vertical scroll by keyboard and mouse wheel; the doc-view scrollbar tracks
  position and size.
- Long lines and the last-line/short-file edges render correctly.

Demo: `ssg somefile.cpp`, scroll top-to-bottom; scrollbar matches.

Delivered:
- Read-only scrolling (arrows / PageUp-Down / wheel → `view.scroll_*`) —
  `3a6a66a`.

## 3. Filesystem bar — DONE
The bar shows the workspace tree; navigate and open files.
- Move selection, expand/collapse directories, `Enter` opens a file in a tab.
- Toggle the bar with `ESC b`; the document area reclaims the width.

Demo: browse the tree, open two files, `ESC b` to hide/show the bar.

Delivered:
- M3a: filesystem bar renders + `ESC b` toggle — `88a6a4e`.
- Renderer promoted into the library (spec `bfac33c`): content on
  accessibility nodes (`1c87f06`), `ssg::render`/`CellGrid` in the library with
  phantom-label overlaps fixed (`1b6071e`).
- M3b: library-owned `TreeModel` selection / navigation / expand / open, three
  tree commands through the catalog machinery — `6a42cb0`.

## 4. Editing and save — DONE
Type into a document and persist it.
- Insert, delete, newline; caret movement; selection; undo (`ESC z`) / redo.
- Save with `ESC s`; dirty state shows in the tab and footer.

Demo: edit a scratch file, save, confirm the bytes changed on disk.

Delivered:
- Editing / undo / redo / save — `cfd9f20`.
- Blinking cursor at the caret (`CellGrid.caret`) — `27d99e7`.
- Word/line-boundary undo coalescing breaks — `b4388ad`.

## 5. Tabs — DONE
Work across multiple open documents.
- Open several files, switch tabs (`ESC ]` / `ESC [`), close a tab (`ESC w`).
- Dirty indicator per tab; closing a dirty tab is recoverable.

Demo: open three files, switch, edit one, close another.

Delivered:
- Tab switch / close / dirty indicator — `74f84b4`.
- Fix: closing the last tab left a phantom document in the editor; the active
  tab is now the sole source of truth — `a125869`.

## 6. Command palette and chords — DONE
Every action is a library command reachable by a 2-key chord and the palette.
- Palette (`ESC p`) lists and runs commands.
- All default chords are two keys: `ESC` then one key (`ESC b`, `ESC B`).
- A configuration escape hatch (open settings) is reachable in every state.

Demo: run commands from the palette; invoke the same ones by chord.

Delivered:
- Focus model (spec `doc/spec-navigation.md`): library-owned `FocusTarget`
  (editor / panel / prompt), stacked prompt transitions, TUI routes input by
  focus — `e403a20` (over-reach reverted at `ca5c1ee`).
- Leader hint, client-resolved and server-presented: the client derives the
  pending chord and reports it via `snapshot(..., leader_pending)`; the library
  renders a theme-colored `leader:` hint — `7541412`, `df51b2c`.
- Command palette (spec `doc/spec-palette.md`, `ca31d4f` / fold `cbe6fde`):
  server publishes the candidate list, client does the fuzzy find. Steps:
  candidate list on snapshot (`f5b3df6`); `PromptKind::palette` + results-pane
  projection (`d134f8d`); header query + ghost-text + client ranker + wiring
  (`07b5e0c`); review fold — server-owned execution, wire codec, guards
  (`3eae7e0`).
- Keymap contexts (spec `doc/spec-keymap.md`): retire the browser-input vestige
  (`a9b7998`); K1 context set + validation + pure resolver (`d276768`); K2
  curated runtime keymap + `settings.open` fix (`5371297`); K3a byte→KeyStroke
  decode with a bounded-Escape contract (`d0a4430`); K3b keymap-driven TUI
  routing (`ffb2e31`); K4 command labels + palette key-sequence detail
  (`d004e40`).

Documented deferrals (in `doc/spec-keymap.md` Considerations):
1. Palette over-lists argument-required commands (needs per-command
   palette-executable arity metadata); executing one is a safe no-op close.
2. The settings prompt is view/cancel-only (needs a server prompt-text-edit
   command) — reachability is satisfied; editing is deferred.

## 7. Find, select, multi-cursor — DONE
The editing feature set beyond basic typing.
- Find / replace in the current document.
- Multiple selections and multi-caret edits.

Demo: find-all a token, add cursors, edit them together.

Spec: `doc/spec-m7.md` (reviewed x3; F2 split into F2a/F2b after review).
Steps: S / D / M / F1 / F2a / F2b.

Delivered:
- M7-S: paint selection highlights + secondary carets — `a3c5aff`, fold
  `0cd4360`.
- M7-D: decode modified arrows (Shift/Ctrl/Alt) in the terminal — `585e72f`.
- M7-M: Shift+Arrow→select.*, multi-cursor chords, find/replace-open chords —
  `21cb765`, fold `9b1b0be`.
- M7-F1: find UI — `find.update_query` command + `FindQueryArguments` codec;
  `find.open` opens a `PromptKind::find` prompt; the snapshot projects
  query+match-count into the reserved rows; matches render (`search_match` /
  active `selection`); the client edits the query with no local copy and fulfils
  Enter/↓→next, ↑→previous, EscEsc→close — `93c58a8`. Review folds: prompt-kind
  gating, guarded close, stale-match render gate (`9ae1f8e`); bind find to
  document identity + revision, closing on document change/edit (`686772d`).
  Scroll-follow: reveal the active match, against the real pane height so it
  clears the prompt rows — `c774735`, `ef9f612`. Reviewed (gpt-5.6-sol): no
  findings.
- M7-F2a: replace workflow — `FindReplaceViewState.replacement` (published +
  wire-serialized) + `replace.update_replacement` command; three-row
  `PromptKind::replace` prompt (query display-only, replacement editable);
  `replace.current`/`replace.all` source the replacement from state; guards to a
  benign no-op unless a replace prompt is active; reveal the successor match —
  `30270f0`.
- M7-F2b: find/replace option toggles — prompt-context chords `[Escape,KeyC/KeyG/
  KeyE]`→case/word/regex, `[Escape,KeyL]`→`replace.all`; option indicators
  projected into both prompts from `options` — `855a449`.
- F2 review fold: carry find options into find.open/replace.open (sticky toggles,
  so replace acts on the reviewed match set); find.close/reconcile dismiss a find
  OR replace prompt — `f064ab5`. Reviewed (gpt-5.6-sol): no findings.

## 8. Mouse — DONE
Pointer supplements the keyboard; it never becomes the only path. Spec:
`doc/spec-m8.md` (reviewed; all findings folded). Every mouse action maps to a
command already reachable by keyboard, or to a client/pointer-fulfilled command
whose outcome is keyboard-reachable.
- Click to place the caret, drag to select, click tabs and tree nodes, drag the
  editor scrollbar, wheel-scroll the document/tree/palette by region.
- Steps: M8-D SGR decode (`5fb57fd`, fold `15be3b2`); M8-C click→caret + pure
  `route_pointer` (`bdc0bff`); M8-S within-viewport drag-select (`b726683`); M8-B
  editor scrollbar click/drag → `view.scroll_to_fraction` (`8592948`); M8-T typed
  `TabHit` map + tab/palette clicks (`35eb7bc`, fold `fd851ef`); M8-R `tree.select`
  command + tree-row click (`861b7e9`); M8-W wheel routes by region + `tree.scroll`
  (`8c5df5c`, fold `0e10b99`); M8-P client-owned palette wheel scroll (`67cbfaa`);
  M8-S2 timer-driven edge auto-scroll during drag (`f1d6a7e`). Also fixed a
  pre-existing editor click bug where viewport hit targets carried line-relative
  (not document-absolute) byte offsets, so every click resolved to line 0
  (`9dda5c9`).

Demo: place the caret by click, drag-select (including a drag held past the edge
that auto-scrolls), drag the scrollbar thumb, click tabs/tree rows, wheel-scroll
the tree and palette.

## 9. Terminal robustness — DONE
The app behaves under real terminal conditions. Spec:
`doc/spec-terminal-robustness.md` (reviewed; all findings folded). Every new
capability is a library abstraction so other clients present identically.
- Resize (`SIGWINCH`) re-lays out from the library with no artifacts.
- Wide/combining/ZWJ Unicode occupies correct cells; truecolor, 256-color, and
  16-color terminals all render the theme.
- Clean terminal restore on quit, error, and terminating signal.

Demo: resize the window while editing (reflows immediately; shrinking below 20×4
shows a placeholder and recovers on grow); open a Unicode-heavy file; run on
`TERM=xterm`/`xterm-256color`/`COLORTERM=truecolor`; `kill -TERM` restores the
terminal.

Delivered:
- M9-W: self-pipe signal wakeup — async-signal-safe handlers write one tag byte;
  the event loop `select()`s over stdin + the pipe (replacing a bare `read()`);
  pure `classify_signal_tags` — `68c3239`, fold `0c30bc6` (watch the pipe in the
  Escape/edge-scroll waits too).
- M9-R: resize consumer re-snapshots at the new `terminal_size()` (delivered by
  the M9-W loop change).
- M9-X: restore the terminal and re-raise on `SIGTERM`/`SIGHUP` (restore in
  normal context since `tcsetattr` is not async-signal-safe); pure
  setup/restore-sequence helpers; top-level `try/catch` boundary — `1d4d2b9`.
- M9-C: library `resolve_color`(SrgbColor, ColorDepth) with pinned xterm-256 and
  ANSI-16 palettes (`d165e71`), plus app depth detection + depth-aware
  `encode_ansi_frame` (`f0e0480`). I22 upheld: the swatches are terminal
  hardware palettes, not theme colors.
- M9-T: layout totality property test + a too-small placeholder — the app guards
  `render()` on a positive viewport and shows "terminal too small" below the
  20×4 minimum instead of crashing — `e038af4`, oracle fold `293b1dd`.
- M9-U: end-to-end Unicode golden (text→snapshot→render→encode) locking
  wide/combining/ZWJ placement, caret advance, and continuation skipping —
  `65fe265`.
- Repaired a baseline committed broken in `b794e3e` (two non-compiling test
  files + a no-op-paste reveal test) — `89343bc`.

## 10. Fast startup — DONE
Cold start competes with comparable editors. Spec:
`doc/spec-fast-startup.md` (reviewed; 6 MUST + 4 SHOULD folded). Measurement-first:
structural invariants are the primary, portable gate; the wall-clock budget is
measured first and pinned last on the benchmark host.
- Time from `ssg <file>` to first drawn frame under the target budget; no
  optional subsystem (Lua, LSP, Tree-sitter, watchers) on the startup path.

Demo: `time ssg <file>` and compare against a peer editor.

Delivered:
- M10-1: startup measurement harness (`benchmarks/startup_benchmark.cpp`,
  gated `STARTUP_MARK` in `ssg_main`, `startup-benchmark.cmake`,
  `startup_trace_compiled_out` ctest) + baseline. Reviewed (3 MUST folded).
- M10-3/M10-4: `prime_deferred` seam + `defer_enrichment` config defers the
  workspace tree scan and syntax pass off the first frame; the app primes after
  the first frame; `EditorSession::advance_revision()` so delta clients observe
  the primed state. `tests/test_startup_path.cpp`. Reviewed (1 MUST folded).
  Measured: deep-tree 17 ms → 2.6 ms.
- M10-2: executable optional-init audit (`include/ssg/startup_audit.h`) locking
  I12 — a per-subsystem construction ledger (Lua/LSP/Tree-sitter grammar/watcher/
  HTTP) proves the first-frame path builds nothing optional, with a static-init
  probe and a positive control. Reviewed (1 MUST + 1 SHOULD folded).
- M10-5: `--enforce` gates the small cache-warm exec→first-frame p99; `spec.md`
  Budgets documents it beside the preserved 250 ms library first-viewport gate.

The 10 MiB first frame (document read + whole-document `active_cell_runs`) is NOT
addressed here — it is Milestone 12 (large files).

## 11. Library API is the contract — DONE
The `ssg` app contains no editor or layout behavior; all of it is library API.
The command/snapshot/delta surface the TUI consumes is the same contract a future
browser or `--http` client must adhere to. Spec: `doc/spec-library-contract.md`
(reviewed; 6 MUST folded). All steps reviewed/folded.
- **M11-L**: `render()` renders the too-small placeholder (from
  `snapshot.client().viewport.dimensions`, no wire change); the app-side
  `encode_too_small_frame` special case is deleted — the app renders every frame.
- **M11-1**: production-runtime render goldens (`tests/test_library_contract.cpp`,
  `tests/fixtures/tui/runtime-{normal,palette,too-small}.txt`); the screen is a
  pure function of the runtime's `SessionSnapshot`. `SSG_REGEN_GOLDEN=1`
  regenerates. Linux-first (goldens embed the workspace path).
- **M11-3**: delta-parity — `derive_session_delta`/`replay_session_delta` after
  each command reconstruct the same snapshot and grid as a fresh one. Caught and
  fixed a real library bug: `replay_session_delta` dropped the `palette` section.
- **M11-4**: derived-view boundary — the client `PaletteReport` is a pure function
  of published candidates + query (shared `palette_rank`/`palette_ghost`/
  `compute_list_scroll_view`), invents no product data, and every rendered label
  traces to a published candidate.
- **M11-2** (capstone): an independent ANSI decoder (round-trip self-tested)
  decodes the REAL `ssg` binary's pty output and asserts the decoded screen
  equals `render(snapshot)` — text, resolved color, and cursor
  (`tests/test_terminal_parity.cpp`; Linux forkpty capture, portable decoder).

## 12. Large files — DONE
Open and edit large documents without a per-frame whole-document cost. Spec:
`doc/spec-viewport-projection.md` (reviewed; 4 MUST folded; Decision A/H0 folded).
The M10 startup work measured a ~1.7 s first frame on a 10 MiB file: the snapshot
AND render both ran `compute_cell_run` (Unicode grapheme/width) over every line to
compute exact wrapped geometry. This milestone makes the first frame
viewport-bounded. **Measured: 10 MiB first_frame p50 ~1.7 s → 9.3 ms (~180x).**
- **VP-1** (`b9aa385`): `compute_viewport_unwrapped` library seam — total row count
  is the logical line count (byte scan), only visible lines are segmented;
  equivalence oracle vs the full wrapped path for fitting lines.
- **VP-2a** (`de6a9e0`): runtime `viewport()` + scrollbar drag honor `word_wrap`
  (unwrapped projection by default).
- **VP-R** (`c383e4c`): `render()` segments only the visible logical lines
  (test hook `render_segmentation_calls`; ≤ rows, doc-length independent).
- **VP-H1** (`51b2533`) / **VP-H2** (`d8a80bc`): Decision A / H0 horizontal scroll
  — `first_visual_column` in the viewport + selection model; caret-driven reveal
  keeps the caret cell in the pane (grapheme-boundary snapped); render + hit-test
  honor the offset; no-wrap caret movement is by logical line. No bottom
  horizontal scrollbar (a faithful thumb needs O(document) max-width — deferred as
  H2). Visually signed off on the real binary.
- **VP-3** (`acce0a2`): word-wrap gate regression (ON wraps a long line to multiple
  visual rows through the runtime, OFF clips to one) + remeasure.
- **Review fold** (`fc1189e`): shared horizontal origin (report requested offset,
  per-row snap in VisualRow.start_cell) + no-wrap selection extension uses
  logical-line steps.
- **VP-2b** (review fold): lazy per-line `TextModel` — construction is a cheap
  '\n' scan; each line's `compute_cell_run` + boundaries are computed on first use
  and cached (stable addresses). No-wrap `visual_row/column/vertical_visual`
  short-circuit (columns==UINT32_MAX) and `current_viewport` uses
  `compute_viewport_unwrapped`, so caret nav + reveal segment only the visible +
  moved lines — O(visible), not O(document). Oracle: `cell_run_calls` counter shows
  identical per-move segmentation for a 50-line and 20000-line file. Closes the
  outstanding INV-viewport-bounded-work gap; first frame unregressed (9.4 ms).
Non-goal (future): lazy/mmapped file loading — the document READ (868 ms for
10 MiB) is now the dominant large-file cost and is a separate milestone.

Demo: open a 10 MiB file — first frame is effectively instant; long lines clip
(wrap off) or wrap (wrap on); caret stays visible via horizontal scroll.

Demo: open a 10 MiB file — first frame is effectively instant; long lines clip
(wrap off) or wrap (wrap on); caret stays visible via horizontal scroll.

## 12-notes (historical, superseded by the DONE list above)
- Honored `word_wrap=false` (previously long lines wrapped regardless): with wrap
  off, one logical line is one visual row, long lines clip; `total_visual_rows`
  is a cheap line count and only visible lines are segmented — across viewport,
  render, AND selection navigation.
- Added minimal horizontal scrolling (Decision A / H0) so a clipped caret stays
  visible; no bottom scrollbar.
- Word-wrap-ON path kept exact; a future incremental per-line width cache is its
  speed path. (Lazy/mmapped file loading — the document read cost — is a further
  very-large-file concern, deferred.)
