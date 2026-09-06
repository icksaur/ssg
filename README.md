# SSG

A fast, modern **terminal text editor** — multiple cursors, a command palette, a
fuzzy file finder, find-and-replace, syntax highlighting, mouse support, and
themes — with the editing conveniences you expect from Sublime Text or VS Code,
in your terminal.

## Install it

On Arch Linux (or a derivative), one script builds an optimized binary and
installs it to `/usr/local/bin/ssg`:

```sh
scripts/install.sh
```

## Run from source

```sh
cmake --preset dev && cmake --build build   # build the `ssg` binary
./build/ssg .            # open the current directory as a workspace
./build/ssg path/to/file.cpp   # or open a file directly
```

You get a full-screen editor: a file tree on the left, tabs across the top, your
document in the middle, and a status header/footer. It opens instantly and should
use no CPU while idle.

## Editing

The things you reach for in a modern editor, in the terminal:

- **Multiple cursors** — add the next occurrence of the selection (`Alt+D`),
  add a cursor on the line above/below (`Alt+K` / `Alt+J`), or split a selection
  into one cursor per line (`Alt+I`). Type once, edit everywhere.
- **Command palette** (`Alt+Shift+P`) — fuzzy-search every command by name, the
  way `Ctrl+Shift+P` works elsewhere.
- **Fuzzy file finder** (`Alt+P`) — jump to any file in the workspace by typing
  part of its name.
- **Find & replace** (`Alt+/` find, `Alt+R` replace) — incremental, with
  literal / regex / whole-word toggles, plus find-word-under-cursor (`Alt+8`)
  and replace across the whole workspace.
- **Undo / redo** (`Alt+Z` / `Alt+Shift+Z`) with word-granular history, and
  cut / copy / paste (`Alt+X` / `Alt+C` / `Alt+V`) with multi-cursor-aware
  clipboard registers.
- **Selection & movement** — arrows and `Shift`+arrows, `Home`/`End`,
  `Ctrl+Home`/`Ctrl+End`, select-all (`Alt+A`).
- **Tabs & panes** — next/previous tab (`Alt+.` / `Alt+,`), close (`Alt+W`),
  split panes, and a collapsible left panel (`Alt+B`) that switches between the
  filesystem tree and Git status.

Modern editors lean on `Ctrl` for these, but terminals reserve most `Ctrl`
combinations, so SSG's default scheme is `Alt`-based and leans on an escape
key for the rest. Every binding is **remappable** — see Configuration.

## Mouse, too

SSG isn't keyboard-only. Click to place the cursor, double-click to select a
word, drag to select a range, use the wheel and scrollbar to scroll, and
middle-click a tab to close it. `Alt`+click adds or removes a cursor.

## Under the hood

- **Syntax highlighting** via Tree-sitter for C, C++, JavaScript, TypeScript,
  C#, Lua, and Markdown — on by default.
- **Workspaces** rooted at a directory: Git diff views with live follow-editing,
  a line-number gutter, and word wrap. Symlinked files are searchable;
  symlinked directories appear in the file tree but are not searched or
  descended into.
- **Safe files** — atomic saves, encoding and line-ending preservation, and
  crash-safe scratch recovery, so an interrupted session doesn't lose work.
- **Unicode 15** — correct grapheme and wide/combining-character layout.
- **Themes** — fully themeable per-role and per-syntax-scope colors, adapting
  down to 256- and 16-color terminals.

## Configuration

Custom colors, keymaps, and chrome glyphs live in `init.lua`
(`~/.config/ssg/init.lua`). A capability-limited Lua 5.4 host lets you script and
rebind commands. See [`doc/config.md`](doc/config.md).

## Requirements

A C++20 compiler, CMake 3.14+, and Lua 5.4. Linux and Windows are supported. Full build and test
instructions are in [`development.md`](development.md).

## License

MIT — see [`LICENSE`](LICENSE).
