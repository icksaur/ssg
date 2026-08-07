# SSG

A C++20 text editor you embed as a **library**, not an application. The editor
and all of its presentation state run headlessly and in-process; terminal,
remote (WebSocket), and in-process clients all drive the *same* editor through
one typed command-and-snapshot model. There is no hidden UI thread and no
escape-sequence soup — your host renders a cell grid and sends semantic
commands.

If you have ever wanted a real editor core — multiple cursors, undo, find and
replace, syntax highlighting, LSP, a command palette — without adopting someone
else's UI framework, that is what SSG is.

## Editor features

- **Multiple cursors** — add next occurrence, add cursor up/down, split a
  selection into one caret per line.
- **Undo / redo** with word-granular history, and named clipboard registers.
- **Find and replace**, incremental, with literal/regex/whole-word toggles and
  a whole-workspace replace.
- **Command palette** and fully configurable keymaps over 160 stable commands.
- **Unicode 15** grapheme and cell layout, UTF-8 validation, and correct
  handling of wide and combining characters.
- **Tree-sitter syntax highlighting** (C, C++, JavaScript, TypeScript, C#, Lua,
  Markdown), plus **LSP** synchronization, diagnostics, language features, and
  atomic workspace edits.
- **Workspaces** rooted at a working directory: tabs, split panes, filesystem /
  Git / symbol trees, live diffs, and follow-edits.
- **Safe files** — atomic writes, encoding and mixed-line-ending preservation,
  scratch recovery after a crash, and external-change detection.
- **Fully themeable** per-role and per-syntax-scope colors, adapting down to
  256- and 16-color terminals.
- **Scriptable** through a capability-limited Lua 5.4 command host and
  user-authored `init.lua` startup configuration.

## Terminal (TUI) editing

SSG is built for the terminal first: the core emits **terminal-style cell runs
and semantic hit targets**, so a TUI host never parses or generates escape
sequences. Out of the box the model gives you:

- word wrap and a 16-color themeable cell grid;
- mouse support — click to place the cursor, double-click to select a word,
  drag to select, wheel and scrollbar scrolling, middle-click to close a tab;
- a collapsible left panel (files / Git / symbols), tabs and split panes;
- a status header and footer with collapsible fields;
- a line-number gutter, live diffs, and follow-edits.

A terminal host does four things: capture events and translate them with the
supplied keymap, provide viewport dimensions, render the `SessionSnapshot` cell
grid, and submit semantic commands. `examples/tui/tui_fixture.{h,cpp}` is a
complete, deterministic reference adapter (a fixture, not a packaged terminal
application).

## Browser and remote editing

The same session speaks a versioned, bounded binary protocol over an
HTTP/WebSocket server adapter (`ssg::HttpEditorServer`, from
`<ssg/HttpEditorServer.h>`) with authentication, reconnect/replay, backpressure,
clipboard exchange, and typed errors. Because every client — terminal, remote,
or in-process — drives the session through the same semantic commands, a remote
front end never reimplements editor behavior; it translates input events and
renders snapshots. `tests/test_end_to_end.cpp` composes the direct API, a TUI
adapter, and a WebSocket server against one runtime.

## Try it: headless editing

`ssg::Document` is the smallest useful entry point. Transactions carry byte
offsets from the pre-transaction revision and are accepted atomically.

```cpp
#include <ssg/Document.h>

#include <iostream>

int main() {
    ssg::Document document{"hello"};

    ssg::EditTransaction transaction{
        .baseRevision = document.revision(),
        .edits = {{
            .offset = ssg::ByteOffset{5},
            .erasedBytes = 0,
            .insertedText = " world",
        }},
    };

    auto result = document.apply(transaction);
    if (!result.accepted()) {
        std::cerr << result.message << '\n';
        return 1;
    }

    std::cout << document.snapshot().text << '\n';   // hello world
}
```

For a batteries-included editor — tabs, workspaces, trees, syntax, the command
catalog — construct an `ssg::EditorRuntime` with `EditorRuntime::create(config)`
from `<ssg/EditorRuntime.h>`. For custom command composition, bind commands with
`ssg::EditorSessionBuilder` (`add()` / `services()` / `build()`) from
`<ssg/EditorSessionBuilder.h>`. The most complete composition examples are:

- `tests/test_end_to_end.cpp` — direct API, TUI, and WebSocket composition
- `examples/tui/tui_fixture.{h,cpp}` — terminal input and cell-grid adapter

## Add it to your project

SSG is consumed from source with `add_subdirectory` (there is no installed
package configuration yet):

```cmake
add_subdirectory(path/to/ssg)

add_executable(my_editor main.cpp)
target_link_libraries(my_editor PRIVATE ssg)
```

Link `ssg_http_server` as well when hosting WebSocket clients. When loaded as a
subdirectory, SSG does not register its internal tests in the parent project.

### Requirements

- A C++20 compiler and CMake 3.14+
- Lua 5.4 headers and library
- The [`http`](../http) repository checked out beside SSG as `../http`

Linux and Windows are the supported platforms.

## Configuration

User-authored startup configuration — custom theme colors, keymaps, chrome
glyphs — lives in `init.lua`, documented in
[`doc/config.md`](doc/config.md). Runtime settings resolve through default,
user, workspace, language, and document scopes.

## Scope

SSG is an editor core, deliberately. Desktop integration (via `../gridui`),
streaming image/output tabs, DAP debugging, and very-large-file mode are stretch
goals, and there is intentionally no package marketplace or plugin registry.

## Documentation

- [`doc/spec.md`](doc/spec.md) — architecture, invariants, and scope
- [`doc/features/`](doc/features/) — detailed feature contracts
- [`doc/config.md`](doc/config.md) — writing `init.lua`
- [`development.md`](development.md) — building, testing, benchmarks, and
  contributor workflow
