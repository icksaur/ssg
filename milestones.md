# Milestones

TUI-first `ssg` editor. The library owns all behavior, layout, and state; the
`ssg` app owns only terminal I/O (drawing cells, colors, keyboard, mouse,
resize). Every milestone ends in something you can run and validate by hand.

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

## 1. Launch and frame
`ssg [path]` starts, draws the full shell, and quits with `ESC Q`.
- No arg or a directory: open that directory (CWD if none). File: open its
  parent directory and that file in a tab.
- Header row, filesystem bar, document area (empty state or the file), footer
  row are all drawn from the library snapshot.

Demo: `ssg`, `ssg src`, `ssg README.md` — see the VSCode-like frame; `ESC Q`
restores the terminal cleanly.

## 2. Read-only document view
Open a file and see its text rendered with the 16-color theme, scrollable.
- Vertical scroll by keyboard and mouse wheel; the doc-view scrollbar tracks
  position and size.
- Long lines and the last-line/short-file edges render correctly.

Demo: `ssg somefile.cpp`, scroll top-to-bottom; scrollbar matches.

## 3. Filesystem bar
The bar shows the workspace tree; navigate and open files.
- Move selection, expand/collapse directories, `Enter` opens a file in a tab.
- Toggle the bar with `ESC b`; the document area reclaims the width.

Demo: browse the tree, open two files, `ESC b` to hide/show the bar.

## 4. Editing and save
Type into a document and persist it.
- Insert, delete, newline; caret movement; selection; undo (`ESC z`) / redo.
- Save with `ESC s`; dirty state shows in the tab and footer.

Demo: edit a scratch file, save, confirm the bytes changed on disk.

## 5. Tabs
Work across multiple open documents.
- Open several files, switch tabs (`ESC ]` / `ESC [`), close a tab (`ESC w`).
- Dirty indicator per tab; closing a dirty tab is recoverable.

Demo: open three files, switch, edit one, close another.

## 6. Command palette and chords
Every action is a library command reachable by a 2-key chord and the palette.
- Palette (`ESC p`) lists and runs commands.
- All default chords are two keys: `ESC` then one key (`ESC b`, `ESC B`).
- A configuration escape hatch (open settings) is reachable in every state.

Demo: run commands from the palette; invoke the same ones by chord.

## 7. Find, select, multi-cursor
The editing feature set beyond basic typing.
- Find / replace in the current document.
- Multiple selections and multi-caret edits.

Demo: find-all a token, add cursors, edit them together.

## 8. Mouse
Pointer supplements the keyboard; it never becomes the only path.
- Click to place the caret, drag to select, click tabs and tree nodes, drag
  either scrollbar, wheel-scroll.

Demo: place the caret by click, drag-select, drag the scrollbar thumb.

## 9. Terminal robustness
The app behaves under real terminal conditions.
- Resize (`SIGWINCH`) re-lays out from the library with no artifacts.
- Wide/combining Unicode occupies correct cells; truecolor and 256-color
  terminals both render the theme.
- Clean terminal restore on quit, error, and signal.

Demo: resize the window while editing; open a Unicode-heavy file.

## 10. Fast startup
Cold start competes with comparable editors.
- Time from `ssg <file>` to first drawn frame under the target budget; no
  optional subsystem (Lua, LSP, Tree-sitter, watchers) on the startup path.

Demo: `time ssg <file>` and compare against a peer editor.

## 11. Library API is the contract
The `ssg` app contains no editor or layout behavior; all of it is library API.
- The command/snapshot/delta surface the TUI consumes is the same contract a
  future browser or `--http` client must adhere to.
- A headless test drives the same snapshots the TUI renders.

Demo: a render fixture reproduces a TUI screen from a library snapshot with no
app-side logic.
