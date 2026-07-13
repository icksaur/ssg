# spec-runnable-browser-application

## Goals

A user can build SSG, run `ssg-editor [CWD]`, open the printed loopback URL, and
edit real files through the bundled browser client. One process owns the real
workspace-backed editor session, serves the browser assets and `/session`
WebSocket on one port, and shuts down cleanly. The README leads with this
workflow and clearly distinguishes the runnable application from embedding,
fixtures, and development gates.

## Design

`EditorRuntime` is the production composition seam in the `ssg` library. It
owns the canonical-CWD `Workspace`, editor/session state, feature models,
recovery services, and the `EditorSession`. It implements `CommandServices`,
binds every descriptor returned by `p0_command_descriptors()` to a real handler,
and is the sole producer of live `SessionSnapshotSections`. Existing feature
types and pure operations remain the behavior owners; the runtime coordinates
them and does not reimplement their algorithms. Construction either returns a
fully bound runtime or an actionable error.

`EditorRuntime` exposes attachment, dispatch, and per-client snapshot operations
needed by in-process/TUI hosts and transport adapters. `EditorSessionBuilder`
remains available for custom composition, while the default runtime is the pit
of success for a complete editor. Synthetic `FixtureModel` state remains only
an independent protocol/client oracle and is never used by the application.

`HttpEditorRoute` registers the existing editor WebSocket protocol on an
externally owned `Http::Server` and owns all route connection state.
`HttpEditorServer` is refactored to own one `Http::Server` plus one
`HttpEditorRoute`, preserving its current convenience API. The
`ssg_editor` application instead owns the shared `Http::Server`, registers the
browser assets through `Http::FileServer`, constructs `HttpEditorRoute` for
`/session`, then starts and stops the shared server once. The server outlives
the route, and the route outlives the started server. This is chosen over two
ports because the browser client intentionally defaults to location.host,
and it avoids CORS and split-origin clipboard behavior.

The sibling `http` library adds `enum class BindAddress { any, loopback }`,
keeps `Server(int)` equivalent to `BindAddress::any` for compatibility, and
adds `Server(int, BindAddress)` plus `boundPort()`. `boundPort()` reports no
value before a successful start and the operating-system-selected port after
start, including when constructed with port `0`. The library also serves `.mjs`
as `text/javascript`. The application always selects `BindAddress::loopback`;
non-loopback application exposure is not configurable. Port `0` is the default
so concurrent local instances do not collide, and the exact selected port is
printed only after successful startup.

Each application launch obtains 32 random bytes from the platform CSPRNG
(`getrandom` on Linux and `BCryptGenRandom` on Windows), hex-encodes them, and
accepts exactly that bearer credential. The printed URL carries it in the
fragment as `#credential=TOKEN`, which is not sent in HTTP requests.
examples/browser/app.mjs
reads the fragment into memory, removes it from browser history before opening
the WebSocket, and sends it only in `SSG1 ATTACH`. Missing, malformed, stale,
and incorrect credentials receive the same rejected-attach result. The token
maps to one local principal with exactly the `local_file_drop` capability;
commands with no required capability remain available through the existing
registry rules. `/` serves examples/browser/index.html;
examples/browser/local.html remains a compatibility
asset but has no separate hard-coded credential.

Browser assets remain ordinary files. `--assets PATH` overrides discovery;
otherwise a build-tree post-build rule copies them beside the executable at
`browser/`, and an installed executable resolves
`../${CMAKE_INSTALL_DATADIR}/ssg/browser` relative to its binary. The required
manifest is index.html, local.html, app.mjs, client.mjs, protocol.mjs, and
style.css; all six files are read successfully before the
socket starts. CMake builds `ssg-editor`, uses `GNUInstallDirs`, and installs
the binary plus this manifest.

The command-line contract is:

```text
ssg-editor [--port PORT] [--assets PATH] [CWD]
```

`CWD` defaults to the process current directory. `PORT` defaults to `0`.
Startup canonicalizes and validates the CWD, initializes the real runtime,
loads the complete asset manifest, binds loopback, then prints
`SSG editor: http://127.0.0.1:PORT/#credential=TOKEN` with the selected port.
On POSIX, SIGINT and SIGTERM are blocked before worker threads start and the
main thread waits with `sigwait`; on Windows, `SetConsoleCtrlHandler` signals a
Windows event that the main thread waits on. Both paths converge on main-thread
`server.stop()` and RAII destruction. Startup and shutdown never depend on
stdin.

## Invariants

- **I1 — Headless core:** the runnable host composes the library; editor
  behavior remains usable without HTTP or browser code.
- **I2 — Single behavior path:** browser and in-process calls use the same
  runtime handlers and snapshots.
- **I3 — Authoritative revisions:** runtime mutations and snapshots use the
  session revision order.
- **I4 — Valid state by construction:** no partially bound runtime or partially
  started server is observable.
- **I9 — CWD boundary:** application filesystem authority is rooted at the
  canonical requested CWD.
- **I10 — Bounded extension failure:** runtime services preserve existing
  cancellation and resource limits.
- **I11 — Protocol compatibility:** the application uses the reviewed protocol
  and codec without an application-specific wire path.
- **I12 — Core independence:** `ssg` remains independently usable; static-file
  serving is linked only into the application/server adapter.
- **I16 — API completeness:** every visible application interaction uses typed
  commands, snapshots, and deltas.
- **I17 — Backend boundary:** the runtime contains no DOM or browser behavior.
- **I19 — Non-modal reversibility:** the application uses existing recovery and
  compensating operations.
- **I21 — Required platforms:** runtime and host build on Linux and Windows.
- **I22 — Color authority:** served clients receive only the active 16-color
  theme through snapshots.

## Considerations

- `EditorSessionBuilder` currently validates descriptor coverage but supplies no
  production handlers; every complete command binding must be real, not a
  success-shaped placeholder.
- `assemble_session_snapshot` currently accepts prebuilt sections. The runtime
  extends the sanctioned editor-session assembly seam and is the only live
  section aggregator.
- File, selection, history, tab, viewport, prompt, status, settings, syntax,
  tree, diff/follow, LSP, and clipboard state have coupled transitions. Runtime
  handlers commit each command through one transaction boundary and publish a
  coherent revision.
- Optional services remain lazy or injected. Constructing basic workspace
  editing does not launch LSP, parse Tree-sitter grammars, or create Lua states.
- Fixture hosts may retain `remote`/`local` credentials for independent tests;
  the runnable application accepts only its per-launch bearer token.
- Asset paths and error messages must work from both the build tree and an
  installed prefix.
- Windows console control handling and POSIX signals both request shutdown;
  cleanup remains owned by RAII and the main thread.

## Risks and Mitigations

- **Synthetic behavior leaks into production:** source and integration tests
  reject fixture headers/symbols in application/runtime targets and verify real
  disk bytes after browser commands.
- **A broad runtime becomes a second implementation:** handlers call existing
  feature operations; seam tests compare direct runtime, TUI, and WebSocket
  snapshots after each command.
- **Incomplete command coverage:** construction compares real bindings against
  `p0_command_descriptors()` and the independent required-command catalog.
- **Network or same-host access leaks workspace data:** the only bind mode is
  loopback and every attach requires the per-launch CSPRNG bearer. Native socket
  and rejected-credential oracles cover both boundaries.
- **Static modules fail in browsers:** HTTP tests assert JavaScript MIME types,
  and a real-browser smoke test loads the application from the C++ server.
- **Shutdown loses recovery data:** lifecycle tests mutate a document, signal
  shutdown, restart, and verify the existing scratch durability contract.

## Acceptance (Definition of Done)

- **Observable:** From a clean checkout with sibling `../http`, a user runs the
  documented build commands followed by `./build/ssg-editor PATH`. The process
  prints one bearer-bearing clickable loopback URL. Opening it loads the browser client without
  a second server; the user opens, edits, saves, closes, and reopens a real file,
  and the saved bytes are present beneath `PATH`. SIGINT/SIGTERM exits cleanly.
  This browser workflow requires visual signoff before commit.
- **Budgets:** Existing command/delta, 10 MiB viewport, idle CPU, queue, and
  durability budgets remain green. Static assets are bounded and loaded once at
  startup.
- **Gates:** Release build, all CTest tests, ASan/UBSan, required
  Chromium/Firefox/WebKit matrices, consumer `add_subdirectory`, native
  loopback-only server tests, and available Windows build/CI gates are green.
- **Oracles:** Temporary-directory ground truth verifies real file operations;
  direct-runtime and browser-WebSocket scripts compare snapshots after every
  command; exact catalog comparison proves handler completeness; HTTP response
  fixtures prove `/`, the six-asset manifest, `.mjs` MIME, and `/session` share
  one port; OS socket inspection and non-loopback connection tests prove
  loopback binding; wrong-token and stale-token attaches prove authentication;
  restart fixtures prove recovery; source scans prove application targets do
  not use test fixtures. Every command-family test initializes two equivalent
  states, dispatches each descriptor through the runtime in one, invokes the
  existing feature-owned operation directly in the other, and compares exact
  model state, snapshots, emitted adapter calls, and disk bytes as applicable.
  The table of cases is independently checked one-to-one against the command
  catalog, so a bound no-op cannot satisfy command coverage.

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Add `BindAddress`, OS-selected `boundPort()`, and ES-module MIME handling to the sibling HTTP library | `../http/{http.h,http.cpp,fileserver.cpp}`, `../http/src/platform/{socket.h,socket_common.cpp,socket_linux.cpp,socket_windows.cpp}`, `../http/tests/*` | OS-reported local endpoint plus non-loopback connection rejection; `.mjs` response fixture | I4, I21 |
| 2 | Define the production runtime ownership and construction API | `include/ssg/editor_runtime.h`, `src/editor_runtime.cpp`, `cmake/components/editor-runtime.cmake` | construction rejects invalid CWD and incomplete bindings; source scan excludes fixtures | I1, I4, I9, I12 |
| 3 | Bind text input, selection, history, edit, clipboard, and find/replace commands | `src/runtime/editing.cpp`, `tests/runtime/test_runtime_editing.cpp` | one catalog-checked case per descriptor: runtime state vs direct feature operation on cloned state | I2, I3, I5, I19 |
| 4 | Bind real file, encoding, tab, and external-modification commands | `src/runtime/files.cpp`, `tests/runtime/test_runtime_files.cpp` | one catalog-checked case per descriptor: temporary-directory runtime vs direct workspace operation and disk bytes | I2, I3, I9, I19 |
| 5 | Bind shell, prompt/status, viewport, layout, and settings commands, including global configuration access and keymap lockout prevention | `src/runtime/presentation.cpp`, `tests/runtime/test_runtime_presentation.cpp` | one catalog-checked case per descriptor: runtime snapshot section vs direct feature operation on cloned state; table-driven `settings.open` transitions from every client state; atomic rejection of keymaps without a global configuration binding | I2, I16, I17, I22, I24 |
| 6 | Bind search, tree, diff, and follow-edits commands | `src/runtime/navigation.cpp`, `tests/runtime/test_runtime_navigation.cpp` | one catalog-checked case per descriptor: runtime state vs independent ranking/diff/follow oracles and captured watcher events | I2, I9, I10, I19 |
| 7 | Bind syntax, LSP feature/workspace-edit, and Lua-dispatch paths | `src/runtime/language_services.cpp`, `tests/runtime/test_runtime_language_services.cpp` | syntax goldens; scripted LSP peer messages/workspace bytes; Lua command invocation vs direct runtime dispatch | I2, I10, I16, I20 |
| 8 | Prove full-catalog behavioral coverage and aggregate complete server-owned per-client UI snapshots | `src/runtime/snapshot.cpp`, `include/ssg/editor_runtime.h`, `tests/runtime/{command_cases.h,test_runtime_snapshot.cpp}` | case-table IDs exactly equal independent required-command catalog; snapshot goldens and delta replay; exact server UI-element inventory and grid geometry | I2, I3, I7, I14, I15, I16, I17 |
| 9 | Add externally owned `HttpEditorRoute` while preserving `HttpEditorServer` | `include/ssg/http_server.h`, `src/http_server.cpp`, `tests/test_http_server.cpp` | one-port HTTP/WebSocket lifecycle and existing convenience-server compatibility | I2, I11, I12 |
| 10 | Add CSPRNG bearer creation, fragment consumption, and exact authentication policy | `src/platform/{secure_random_linux.cpp,secure_random_windows.cpp}`, `examples/browser/{app.mjs,local.html}`, `tests/{test_secure_random.cpp,browser/*}` | token length/uniqueness, wrong/stale attach rejection, browser history has no credential after attach | I4, I11, I21 |
| 11 | Add the loopback `ssg-editor` executable, six-asset discovery/install, and cross-platform main-thread shutdown | `apps/{ssg_editor_main.cpp,platform/shutdown_posix.cpp,platform/shutdown_windows.cpp}`, `cmake/components/ssg-editor.cmake`, `CMakeLists.txt`, `tests/test_ssg_editor.cpp` | subprocess startup/URL/asset/shutdown and invalid-CWD/assets/port cases; installed-prefix smoke test | I4, I9, I21 |
| 12 | Replace synthetic product-path parity with real workspace-backed parity and thin-client UI enforcement | `tests/test_end_to_end.cpp`, `tests/browser/client/*`, `tests/browser/end_to_end/*` | direct/TUI/browser states plus exact saved disk bytes after every workflow; real-browser `Escape` leader/configuration access from every state; rendered element inventory equals the server snapshot and uses only its 16 theme colors | I1, I2, I6, I7, I16, I17, I18, I22, I24 |
| 13 | Rewrite the README around the human launch path and retain embedding as advanced usage | `README.md` | clean-checkout command transcript and link/content smoke test | - |

## Rationale (optional, skippable)

Serving the current test fixture would make the README executable but still
would not let a user edit their files. The missing deliverable is not an HTTP
one-liner; it is the production binding between already implemented feature
models. Making that binding a library-owned runtime preserves the headless goal
and gives browser, TUI, desktop, and custom embedders one complete behavior path.
