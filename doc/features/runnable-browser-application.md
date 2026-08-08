# spec-editor-runtime (historical: runnable-browser-application)

> **Status: partially superseded.** The `EditorRuntime` production composition
> seam and the `HttpEditorServer`/`HttpEditorRoute` WebSocket adapter described
> here shipped and remain the library's real entry points. The bundled
> `ssg-editor` loopback executable and the browser reference client this spec was
> originally framed around were **removed as out of scope**: SSG ships the
> library, the WebSocket server adapter, and a reference TUI adapter, and bundles
> no client application. The protocol stays browser-deliverable (spec.md I18), so
> a host may serve its own standards-based browser client. This document is
> retained as the design record for `EditorRuntime`; sections describing the
> removed executable and its browser assets no longer reflect shipped code.

## Goals

`EditorRuntime` is the pit-of-success production composition of the `ssg`
library: one object owns a real workspace-backed editor session, binds every
command to a real handler, and produces live per-client snapshots for
in-process, TUI, and WebSocket hosts. A host serves the `/session` WebSocket to a
remote client through the server adapter; SSG bundles no client of its own.

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

`HttpEditorRoute` registers the editor WebSocket protocol on an externally owned
`Http::Server` and owns all route connection state. `HttpEditorServer` owns one
`Http::Server` plus one `HttpEditorRoute` as a convenience API. A host that also
serves its own browser assets can register them on the same shared
`Http::Server`; defaulting the client to `location.host` avoids CORS and
split-origin clipboard behavior.

The sibling `http` library adds `enum class BindAddress { any, loopback }`,
keeps `Server(int)` equivalent to `BindAddress::any` for compatibility, and
adds `Server(int, BindAddress)` plus `boundPort()`. `boundPort()` reports no
value before a successful start and the operating-system-selected port after
start, including when constructed with port `0`. The library also serves `.mjs`
as `text/javascript`. These additions shipped and remain useful to any host.

> The remainder of this Design section — the `ssg-editor` executable, its
> browser-asset manifest and discovery/install rules, its `ssg-editor
> [--port PORT] [--assets PATH] [CWD]` command-line contract, and its
> POSIX/Windows main-thread shutdown — described the removed loopback
> application and no longer reflects shipped code. It is elided; a host that
> wants a runnable binary composes `EditorRuntime` with `HttpEditorServer`
> itself.

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
  loopback. Native socket oracles cover the boundary.
- **Static modules fail in browsers:** HTTP tests assert JavaScript MIME types,
  and a real-browser smoke test loads the application from the C++ server.
- **Shutdown loses recovery data:** lifecycle tests mutate a document, signal
  shutdown, restart, and verify the existing scratch durability contract.

## Acceptance (Definition of Done)

- **Observable:** A TUI fixture and a WebSocket client fixture open a directory
  through `EditorRuntime` and perform typing, multi-cursor selection,
  copy/cut/save, per-file undo/redo, close/reopen, read-only rejection, diff
  viewing, and crash-recovery scripts through the same production runtime API,
  with saved bytes present on disk. The removed `ssg-editor` executable's
  clean-checkout "run and edit in a browser" walkthrough no longer applies.
- **Budgets:** Existing command/delta, 10 MiB viewport, idle CPU, queue, and
  durability budgets remain green.
- **Gates:** Release build, all CTest tests, ASan/UBSan, consumer
  `add_subdirectory`, native loopback-only server tests, and available Windows
  build/CI gates are green.
- **Oracles:** Temporary-directory ground truth verifies real file operations;
  direct-runtime and browser-WebSocket scripts compare snapshots after every
  command; exact catalog comparison proves handler completeness; HTTP response
  fixtures prove `/`, the six-asset manifest, `.mjs` MIME, and `/session` share
  one port; OS socket inspection and non-loopback connection tests prove
  loopback binding; restart fixtures prove recovery; source scans prove application targets do
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
| ~~11~~ | ~~Loopback `ssg-editor` executable, six-asset discovery/install, cross-platform shutdown~~ — REMOVED (out of scope; SSG bundles no client application) | — | — | — |
| ~~12~~ | ~~Real workspace-backed browser parity and thin-client UI enforcement~~ — SUPERSEDED: `tests/test_end_to_end.cpp` retains the in-process/TUI/WebSocket parity oracle; the browser-client suites were removed | `tests/test_end_to_end.cpp` | direct/TUI/WebSocket state parity plus saved disk bytes | I1, I2, I16, I17 |
| ~~13~~ | ~~Rewrite the README around the human launch path~~ — done separately (visitor-facing README + `development.md`) | `README.md`, `development.md` | link/content smoke test | - |

## Rationale (optional, skippable)

Serving the current test fixture would make the README executable but still
would not let a user edit their files. The missing deliverable is not an HTTP
one-liner; it is the production binding between already implemented feature
models. Making that binding a library-owned runtime preserves the headless goal
and gives browser, TUI, desktop, and custom embedders one complete behavior path.
