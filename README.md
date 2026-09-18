# SSG

A **terminal text editor** — multiple cursors, a command palette, a
fuzzy file finder, find-and-replace, syntax highlighting, mouse support, and
themes — with the editing conveniences you expect from Sublime Text or VS Code,
in your terminal.

## Install it

On Arch Linux (or a derivative), build a native package and install it through
pacman:

```sh
scripts/install.sh
```

The script runs `makepkg --syncdeps --install --force` against the repository's
`PKGBUILD`, so pacman owns upgrades and removal.

## Run from source

Linux:

```sh
cmake --preset dev && cmake --build build   # build the `ssg` binary
./build/ssg .            # open the current directory as a workspace
./build/ssg path/to/file.cpp   # or open a file directly
```

Windows, from an x64 MSVC developer command prompt:

```powershell
cmake --preset windows-msvc
cmake --build --preset windows-msvc
.\build-windows\ssg.exe .
```

Windows cross-build from Linux with MinGW-w64:

```sh
cmake -S . -B build-mingw -G Ninja \
  -DCMAKE_SYSTEM_NAME=Windows \
  -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
  -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++ \
  -DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres \
  -DCMAKE_BUILD_TYPE=Release \
  -DSSG_ASSERTIONS=ON -DSSG_CCACHE=OFF -DSSG_WERROR=ON
cmake --build build-mingw
```

This produces `build-mingw/ssg.exe` and `build-mingw/ssg_tests.exe`. The
cross-build verifies compilation and linking; running the executables requires
Windows or a compatible Wine environment.

You get a full-screen editor: a file tree on the left, tabs across the top, your
document in the middle, and a status header/footer. It opens instantly and should
use no CPU while idle.

## Editing

**One modifier: `Mod` is `Ctrl` or `Alt`.** Press whichever your terminal passes
through — both do the same thing. `Ctrl+Alt` together is never an SSG chord; that
combination is reserved for your terminal or window manager. Every binding is
remappable — see Configuration.

The things you reach for in a modern editor, in the terminal:

- **Multiple cursors** — add the next occurrence of the selection (`Mod+D`),
  add a cursor on the line above/below (`Mod+K` / `Mod+J`), or split a selection
  into one cursor per line (`Mod+I`). Type once, edit everywhere.
- **Command palette** (`Mod+Shift+P`) — fuzzy-search every command by name, the
  way `Ctrl+Shift+P` works elsewhere.
- **Fuzzy file finder** (`Mod+P`) — jump to any file in the workspace by typing
  part of its name.
- **Find & replace** (`Mod+/` find, `Mod+R` replace) — incremental, with
  literal / regex / whole-word toggles, plus find-word-under-cursor (`Mod+8`)
  and replace across the whole workspace.
- **Undo / redo** (`Mod+Z` / `Mod+Shift+Z`) with word-granular history, and
  cut / copy / paste (`Mod+X` / `Mod+C` / `Mod+V`) with multi-cursor-aware
  clipboard registers.
- **Selection & movement** — arrows and `Shift`+arrows, `Home`/`End`,
  `Mod+Home`/`Mod+End`, select-all (`Mod+A`).
- **Tabs & panes** — next/previous tab (`Mod+.` / `Mod+,`), close (`Mod+W`),
  split editor panes, and a collapsible left sidebar (`Mod+B`) that switches between the
  filesystem tree and Git status.
- **Full-text search** (`Mod+Shift+F`)
- **Desktop clipboard paste** — `Mod+V` reads through `wl-paste` on Wayland,
  `xclip` on X11, or the native Windows Unicode clipboard; terminal paste
  shortcuts also work.

A handful of chords are Alt-only: a terminal transmits `Ctrl+I`, `Ctrl+M`,
`Ctrl+H` and `Ctrl+[` as Tab, Enter, Backspace and Escape, so nothing survives
for SSG to tell apart.

## Mouse, too

SSG isn't keyboard-only. Click to place the cursor, double-click to select a
word, drag to select a range, use the wheel and scrollbar to scroll, and
middle-click a tab to close it. `Alt`+click adds or removes a cursor — that one
is `Alt` specifically, a mouse chord rather than a `Mod` chord.

## Under the hood

- **Syntax highlighting** via Tree-sitter for C, C++, JavaScript, TypeScript,
  C#, Lua, and Markdown — on by default.
- **Workspaces** rooted at a directory: Git diff views with live follow-editing,
  a line-number gutter, and word wrap. Symlinked files are searchable;
  symlinked directories appear in the file tree but are not searched or
  descended into.
- **Safe files** — atomic saves, encoding and line-ending preservation, and
  restoration of unsaved editable tabs after a normal exit. Session snapshots
  live in `./.ssg/session.snapshot` relative to the process starting directory.
- **Unicode 15** — correct grapheme and wide/combining-character layout.
- **Themes** — fully themeable per-role and per-syntax-scope colors, adapting
  down to 256- and 16-color terminals.

## Configuration

Custom colors, keymaps, and chrome glyphs live in `init.lua`
(`~/.config/ssg/init.lua`). A capability-limited Lua 5.4 host lets you script and
rebind commands. See [`doc/config.md`](doc/config.md).

## Requirements

A C++20 compiler and CMake 3.14+. Lua 5.4 is built from pinned vendored
sources. Linux and native Windows console builds are supported. Windows builds
use MSVC or MinGW-w64 and are intended to run in Windows Terminal or another
host with virtual-terminal output support. See
[`development.md`](development.md).

## License

MIT — see [`LICENSE`](LICENSE).
