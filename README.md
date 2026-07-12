# SSG

SSG is a C++20, library-first text editor. The authoritative editor and
presentation state runs headlessly, and in-process, TUI, and WebSocket/browser
clients use the same typed command, snapshot, and delta model.

## User-facing deliverables

- Headless editing library with UTF-8 validation, Unicode 15 grapheme/cell
  layout, multiple selections, undo/redo, clipboard registers, find/replace,
  command palette, configurable keymaps, and 160 stable commands.
- CWD-focused workspaces with tabs and split panes, atomic file operations,
  encoding and mixed-EOL preservation, scratch recovery, external-change
  handling, filesystem/Git/symbol trees, live diffs, and follow-edits.
- Shared monospace presentation model with wrapping, mouse hit targets, wheel
  and scrollbar navigation, a collapsible left panel, status header/footer, and
  themes containing exactly 16 colors.
- Tree-sitter syntax state, LSP synchronization/diagnostics/language features
  and atomic workspace edits, plus a capability-limited Lua 5.4 command host.
- Versioned, bounded binary protocol and an HTTP/WebSocket server adapter with
  authentication, reconnect/replay, backpressure, clipboard exchange, and
  typed errors.
- Reference TUI and standards-based browser clients. Required browser workflows
  are exercised in Chromium, Firefox, and WebKit.
- Cross-transport parity fixtures and a deterministic 10,000-operation
  performance benchmark.

Desktop integration through `../gridui`, streaming image/output tabs, DAP, and
very-large-file mode remain stretch goals. SSG intentionally has no package
marketplace or plugin registry.

## Requirements

- CMake 3.14 or newer
- A C++20 compiler
- Lua 5.4 headers and library
- The `http` repository checked out beside SSG as `../http`

Node.js and Chromium, Firefox, and WebKit runtimes are needed only for the
browser test matrices. Linux and Windows are the required platforms.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Enable AddressSanitizer and UndefinedBehaviorSanitizer with GCC or Clang:

```sh
cmake -S . -B build-sanitize \
  -DCMAKE_BUILD_TYPE=Release \
  -DSSG_SANITIZE=ON
cmake --build build-sanitize --parallel
ctest --test-dir build-sanitize --output-on-failure
```

Browser tests discover installed runtimes automatically. Override executable
paths with `SSG_CHROMIUM`, `SSG_FIREFOX`, or `SSG_WEBKIT`.

## Use SSG from CMake

SSG currently supports source-tree consumption with `add_subdirectory`; it
does not install a package configuration.

```cmake
add_subdirectory(path/to/ssg)

add_executable(my_editor main.cpp)
target_link_libraries(my_editor PRIVATE ssg)
```

Link `ssg_http_server` as well when hosting WebSocket clients:

```cmake
target_link_libraries(my_editor PRIVATE ssg_http_server)
```

When loaded as a subdirectory, SSG does not register its internal tests in the
parent project.

## Basic headless editing

`ssg::Document` is the smallest useful entry point. Transactions use byte
offsets from the pre-transaction revision and are accepted atomically.

```cpp
#include <ssg/document.h>

#include <iostream>

int main() {
    ssg::Document document{"hello"};

    ssg::EditTransaction transaction{
        .base_revision = document.revision(),
        .edits = {{
            .offset = ssg::ByteOffset{5},
            .erased_bytes = 0,
            .inserted_text = " world",
        }},
    };

    auto result = document.apply(transaction);
    if (!result.accepted()) {
        std::cerr << result.message << '\n';
        return 1;
    }

    std::cout << document.snapshot().text << '\n';
}
```

For a complete editor session, use `ssg::EditorSessionBuilder` from
`<ssg/editor_session_assembly.h>`. The host binds handlers for every descriptor
returned by `ssg::p0_command_descriptors()`, optionally injects
`CommandServices`, and then calls `build()`. Construction rejects missing,
extra, or duplicate command bindings.

The most complete composition examples are:

- `tests/test_end_to_end.cpp` — direct API, TUI, and WebSocket composition
- `tests/browser/client/fixture.cpp` — authenticated browser server host
- `examples/tui/tui_fixture.{h,cpp}` — terminal input and cell-grid adapter

## WebSocket and browser client

`ssg::HttpEditorServer` from `<ssg/http_server.h>` attaches an
`EditorSession` to the versioned protocol. The embedding host supplies
authentication/session policy through `HttpEditorSessionHost`, protocol
argument codecs, and bounded server configuration.

The reference browser client is in `examples/browser/`. Serve those files from
the same origin as the server, or pass connection settings in the URL:

```text
index.html?websocket=ws://127.0.0.1:8080/session&credential=remote
```

The client converts browser keyboard, IME, mouse, wheel, scrollbar, and
clipboard events into semantic commands. Editor behavior remains in the C++
session.

## TUI integration

The core emits terminal-style cell runs and semantic hit targets, not escape
sequences. A TUI host is responsible for:

1. Capturing terminal events and translating them with the supplied keymap.
2. Providing viewport dimensions and a snapshot assembler.
3. Rendering the 16-color `SessionSnapshot` cell grid.
4. Submitting semantic commands through the attached `EditorSession`.

`examples/tui/tui_fixture.{h,cpp}` demonstrates input capture, snapshot refresh,
and deterministic screen rendering. It is a reference adapter, not a packaged
terminal application.

## Performance benchmark

The benchmark verifies the pinned corpus and deterministic operation script:

```sh
cmake --build build --target editor_benchmark
./build/editor_benchmark --verify-only
./build/editor_benchmark --enforce
```

The designated-host limits are:

- Edit latency below 1 ms p50 and 4 ms p99
- Command-to-delta latency below 2 ms p50 and 8 ms p99
- First viewport for a 10 MiB document below 250 ms
- No unchanged-viewport cell payload and no polling CPU while idle

## Data and configuration

- `data/default-keymap.json` — browser-deliverable default bindings
- `data/required-commands.json` — exact required command catalog
- `data/themes/` — bundled exactly-16-color themes
- `data/unicode/` — pinned Unicode 15 source data and provenance
- `data/ui/status_fields.json` — header/footer collapse priorities

Runtime settings support default, user, workspace, language, and document
scopes. Workspace file authority and persisted relative paths are rooted at the
canonical CWD.

## Project documentation

- `doc/spec.md` — architecture, invariants, acceptance gates, and scope
- `doc/features/` — detailed feature contracts
- `doc/learnings.md` — durable implementation and integration constraints
- `cpp-lib-values.md` — public C++ API design values
- `process.md` — development and review workflow
- `doc/backlog.md` — deferred work
