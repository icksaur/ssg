# spec-windows-build

## Goals

SSG builds and passes its tests with GCC or Clang on Linux and with MSVC on
Windows. The Windows executable runs as a native console application with the
same editing, keyboard, mouse, resize, clipboard, watcher, Git-diff, init-script,
terminal-restoration, and idle-wait behavior as Linux. Shared editor and
application code contains no operating-system calls: one platform layer owns
all such calls, with one Linux implementation and one Windows implementation.

## Design

`ssg_platform` becomes the compile-time platform boundary. Its existing Windows
filesystem and watcher backends and the corresponding Linux backends are
relocated into implementation families selected by CMake from
`src/platform/linux/` or `src/platform/windows/`. Its public,
operating-system-neutral contract is extended by
`include/ssg/PlatformRuntime.h`. Shared validation and protocol logic remains in
`src/platform/` when it makes no native calls. The existing
`test_file_seam_guard` expands from named-header ownership into the single
source-boundary oracle; no second source scanner is introduced.

`PlatformWake` is a move-only, level-preserving cross-thread notification. A
producer queues authoritative data before calling `notify()`. A consumer calls
`consume()` before draining that data. Multiple notifications may coalesce, but
once notified the wake remains observable until consumed. Linux implements it
with `eventfd`; Windows implements it with an auto-reset event. `GitDiffWorker`
and `InitScriptWatcher` own `PlatformWake` instances instead of pipes and expose
borrowed, lifetime-bound references for the application wait set.

`PlatformEventLoop` owns process-control registration and waits for terminal
input, process termination, resize, and a span of borrowed `PlatformWake`
objects. It returns platform-neutral readiness containing terminal-input and
resize flags, an optional opaque `TerminationRequest`, and the ready wake
indices. Linux implements the wait with `poll`, `signalfd`, standard input, and
`eventfd`. Windows implements it with `WaitForMultipleObjects`, the console
input handle, control-handler events, and `PlatformWake` events. Waiting is
blocking when the application has no deadline.

`TerminalSession` retains shared escape-mode stack behavior but stores native
state behind an implementation object, so `Terminal.h` exposes no `termios` or
Win32 types. Linux preserves its current terminal setup. Windows saves both
console modes, enables VT output, writes the renderer's UTF-8 bytes, and restores
the exact prior modes and output code page. Non-console standard handles remain
a clear startup refusal.

Windows uses `ReadConsoleInputW` as the sole consumer of the console input
queue. Printable text becomes UTF-8. Unmodified functional keys use the
canonical legacy CSI or SS3 sequence already accepted by `decodeInput`;
modified or otherwise ambiguous keys use canonical Kitty CSI-u encoding. Mouse
records use canonical SGR mouse encoding. This keeps `Decoded` as the shared
semantic oracle without making the Win32 implementation depend on terminal
capability detection. The platform implementation translates key, text,
repeat, modifier, mouse, wheel, and resize records into those byte sequences and
resize readiness. It does not combine
`ReadConsoleInputW` with `ReadFile` or `ENABLE_VIRTUAL_TERMINAL_INPUT`; avoiding
two consumers of one queue makes input ordering and resize delivery explicit.
The translation is pure apart from the Win32 read and is tested from synthetic
input records against hand-authored expected terminal events.

Windows terminal activation saves the original input and output modes and
output code page before mutation. Input enables `ENABLE_WINDOW_INPUT`,
`ENABLE_MOUSE_INPUT`, and `ENABLE_EXTENDED_FLAGS`; disables QuickEdit, line,
echo, processed, and virtual-terminal input; and therefore leaves Ctrl-based
editing chords in the input queue rather than routing Ctrl+C through the process
control handler. Output enables `ENABLE_PROCESSED_OUTPUT` and
`ENABLE_VIRTUAL_TERMINAL_PROCESSING` and selects UTF-8. Restoration writes back
the exact saved state.

Terminal capability replies received as character records still flow through
the shared reply decoder. Windows key translation does not depend on the
keyboard-protocol capability, and `TerminalSession::enableKeyboardProtocol` is
a Windows no-op so the terminal host is not asked to change the record format.

The Windows console control handler performs no editor or terminal work. It
records the control reason, signals the event loop, and, for operating-system
shutdown events, waits only for a bounded cleanup acknowledgement. The runtime
thread restores the terminal and passes the opaque request to the platform
termination function. Linux restores and re-raises the original signal;
Windows acknowledges cleanup and exits with the platform-defined status.

`SystemClipboardReader` remains a platform-neutral facade and
`planSystemClipboardPaste` remains shared. Linux moves helper discovery and
bounded child-process execution into its platform family. Windows opens
`CF_UNICODETEXT`, closes the clipboard on every acquired path, validates the
terminated buffer inside its allocation, converts UTF-16 to UTF-8 strictly, and
applies the existing read deadline and byte-limit policy. Native calls are
isolated behind narrow adapters so contention, malformed data, size, and API
failure tests do not require a desktop clipboard.

The current `windows-latest` matrix leg is not a trusted gate: it does not
provision Lua or an explicit compiler environment and asks CMake to build a
nonexistent per-suite target. The current required Lua ABI is built from a pinned vendored release as a
private static C target. Project-owned compiler warnings and vendored-source
warning suppression select MSVC or GNU/Clang forms explicitly. A Windows MSVC
preset and CI job configure, build, link the platform-interface gate, and run
the test suite without requiring an interactive desktop. CI builds the real
`ssg_tests` target and runs the platform-interface suite through CTest.

## Invariants

- **P1 — Native-call boundary:** operating-system headers, types, constants,
  and calls in product code occur only below `src/platform/linux/` or
  `src/platform/windows/`; platform-specific oracle code may occur below the
  matching `tests/platform/` family. State this in
  `include/ssg/PlatformRuntime.h` and enforce it by extending
  `tests/test_file_seam_guard.cpp`.
- **P2 — Single implementation family:** at completion, every build compiles
  and links exactly one complete platform family. Intermediate Linux-only
  stages need not link Windows. State this at the platform source selection in
  `CMakeLists.txt`; `test_platform_interface` takes every public platform symbol.
- **P3 — Wake ordering:** producers queue data before notification; consumers
  consume notification before draining data; coalescing cannot lose queued
  work. State this on `PlatformWake` in `PlatformRuntime.h`.
- **P4 — Borrowed waitables:** a wake registered with `PlatformEventLoop` must
  outlive that wait; the event loop neither owns nor retains it after `wait`
  returns. State this on `PlatformEventLoop::wait`.
- **P5 — Runtime-thread mutation:** background workers only queue data and
  notify; editor, script-host, terminal, and presentation state remain mutated
  by the runtime thread. State this on `PlatformWake` and retain the worker
  class contracts.
- **P6 — Terminal restoration:** every successful native terminal-state change
  is restored exactly once on normal exit, startup failure after activation,
  exception, and platform termination. State this on `TerminalSession`.
- **P7 — One Windows input consumer:** only `ReadConsoleInputW` consumes the
  Windows console input queue; no byte-read API is mixed with it. State this in
  the Windows runtime implementation.
- **P8 — Input parity:** platform translation preserves text, repeat count,
  supported key identity, SSG's Ctrl-or-Alt `Mod`, `Meta`, `Shift`, pointer
  position, button, drag, release, and wheel direction before shared routing.
  State this on the platform input translation contract.
- **P9 — Clipboard bounds:** platform clipboard reads preserve the existing
  deadline, byte limit, UTF-8 validity, status mapping, and internal-register
  fallback contracts. State this on `SystemClipboardReader`.
- **P10 — Idle blocking:** with no editor deadline and no platform event, the
  runtime blocks in the platform wait and consumes no polling loop. State this
  on `PlatformEventLoop::wait`.
- **P11 — No silent platform degradation:** failure to create required wake,
  wait, console, or control-handler resources fails startup with a diagnostic;
  optional clipboard unavailability retains its existing explicit status.
  State this on the creating platform APIs.

## Considerations

- `PlatformWake::consume()` precedes queue extraction. A notification racing
  after consume either accompanies data drained now or remains signaled for the
  next drain; consuming after extraction would permit a lost wake.
- A Windows wait may report one of several simultaneously ready objects.
  `PlatformEventLoop::wait` gathers all currently ready sources before
  returning, while callers still fully drain each authoritative queue.
- Windows console key records contain UTF-16 code units and repeat counts.
  Translation must join valid surrogate pairs, reject or replace malformed
  pairs according to the existing text-codec contract, and reproduce repeats
  without splitting a UTF-8 sequence.
- Windows modifier mapping folds Ctrl or Alt into `KeyStroke::mod`, preserves
  `meta` only where the shared contract represents it, and preserves `shift`.
  AltGr-produced text is committed text and must not dispatch a `Mod` binding;
  Ctrl+Alt remains outside SSG's chord vocabulary.
- Mouse coordinates are console-buffer coordinates. Translation must use the
  visible window origin so shared pointer routing receives viewport-relative
  cells.
- Windows buffer-size records are resize notifications; dimensions remain
  authoritative from the visible-window console query performed on every input
  wake and when projecting a frame. The allocated-console tests and observable
  signoff cover hosts where visible-window and buffer changes differ.
- Focus, menu, and key-up records may make the console handle ready without
  producing input bytes. The Windows wait implementation drains ignorable
  records and waits again rather than returning empty input readiness.
- Linux blocks the handled signal set and creates `signalfd` before constructing
  `Editor`, `GitDiffWorker`, or `InitScriptWatcher`; spawned threads inherit the
  mask. `SignalEvents`, `classifySignalTags`, and the self-pipe handler are
  removed rather than retained beside `signalfd`.
- The console control callback runs on an operating-system thread. It may use
  only atomics and synchronization primitives owned by the Windows platform
  object; cleanup remains on the runtime thread.
- `CF_UNICODETEXT` allocation size is only a bound. The terminator determines
  content length, and conversion occurs before closing the clipboard only after
  copying data owned by SSG.
- Headless Windows CI can test input translation using synthetic records and
  native resource behavior using a console owned for the whole isolated CTest
  suite process. The suite detaches any inherited console before
  `AllocConsole`; rendering in Windows Terminal remains an observable signoff,
  not a CI screenshot test.
- Moving existing filesystem and watcher sources is structural. Their public
  contracts and behavior do not change.
- The specification is complete when MSVC is the supported Windows toolchain.
  MinGW may work through the same Win32 implementation but is not a release
  gate.
- The compiler/dependency stage is the first stage expected to configure with
  MSVC. Earlier stages are Linux-verifiable extraction work.

## Risks and Mitigations

- **Input semantic drift:** translating Win32 records could diverge from Linux
  terminal decoding. Use shared expected `Decoded` fixtures for both Linux byte
  sequences and Windows synthetic records.
- **Lost worker notifications:** event and pipe reset semantics differ. Pin the
  ordering and coalescing contract with cross-platform concurrency tests before
  changing either worker.
- **Terminal left modified after failure:** partial setup and asynchronous
  termination create multiple exits. Make native setup transactional, retain
  original state before mutation, and test restoration with injected native
  adapters and a child-process termination test.
- **Control-handler deadlock:** shutdown callbacks have constrained lifetimes.
  Keep the callback independent of editor locks and use a bounded cleanup
  acknowledgement owned entirely by the platform object.
- **Windows CI lacks an attached console or clipboard:** use `AllocConsole` for
  native console tests and adapter-based clipboard tests. Do not make desktop
  clipboard availability a test prerequisite.
- **Vendored dependency drift:** pin the Lua source and license beside existing
  vendored dependencies; compile only the library sources and verify the ABI at
  compile time.
- **Boundary erosion:** future native calls could reappear in shared code. Run
  the cross-platform source-boundary gate in every CI build.

## Acceptance (Definition of Done)

- Observable: On Linux, existing behavior is unchanged. In Windows Terminal,
  SSG starts in the alternate screen, renders Unicode, accepts text and all
  currently supported key chords, handles pointer and wheel input, resizes,
  pastes from the desktop clipboard, reloads `init.lua`, refreshes Git state,
  saves files, idles without polling, and restores the console after normal
  quit and control termination. Windows behavior requires user signoff.
- Budgets: no periodic idle wake; no additional long-lived thread beyond the
  existing workers; platform input and wake handling preserve current
  interactive latency; clipboard reads remain bounded by
  `kSystemClipboardReadDeadline` and `kSystemClipboardReadLimit`.
- Gates: `cmake --build build` and `ctest --preset dev` are green on Linux; the
  Windows MSVC configure preset, build preset, `ssg_tests` target,
  `test_platform_interface` CTest selection, and full test preset are green on
  `windows-latest`; `test_file_seam_guard` is green on both.
- Oracles:
  - Native-call boundary -> the expanded `test_file_seam_guard` fails on the current tree
    before extraction and passes only when native calls are confined to the two
    platform families.
  - Platform interface -> compile/link test takes every public symbol under
    each selected family.
  - Wake behavior -> cross-platform tests cover pre-wait notification,
    concurrent notification, coalescing, timeout, consume-before-drain ordering,
    and destruction while idle.
  - Linux parity -> existing terminal, clipboard, watcher, Git-diff, init-script,
    platform-file, recovery, and startup tests remain unchanged in outcome.
  - Windows input -> table-driven synthetic console records are compared with
    hand-authored `Decoded` fixtures; the existing independent Linux
    `decodeInput` cases establish the canonical legacy, Kitty CSI-u, and SGR
    sequence meanings.
  - Windows terminal -> injected native adapter tests prove exact mode/code-page
    restoration after full setup, each partial failure, and repeated restore.
  - Windows termination -> child-process test injects supported console-control
    requests and verifies cleanup acknowledgement, terminal restoration, and
    platform exit status.
  - Windows clipboard -> fake-adapter tests cover contention, absent format,
    missing terminator, malformed UTF-16, empty text, multiline Unicode,
    oversized text, and close-on-every-acquired-path.
  - Vendored Lua -> existing Lua host tests run against the vendored target on
    both toolchains.
  - Windows watchers -> existing watcher suites execute against the selected
    Windows filesystem and Git-metadata watcher implementations and include
    create, modify, rename, delete, overflow/reconcile, and shutdown cases.

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Make dependency, packaging, and compiler configuration reproducible before runtime work: declare C at `project`, replace system Lua discovery with a pinned static target, select project and vendor flags by compiler, and establish a non-required staged MSVC job that configures and builds `ssg_platform` | `CMakeLists.txt`; `CMakePresets.json`; `.github/workflows/ci.yml`; `vendor/lua/`; `vendor/lua/LICENSE`; `vendor/lua-pin.txt`; `PKGBUILD`; `README.md`; `development.md` | clean Linux and MSVC configure; staged Windows `ssg_platform` build; Linux build and full test suite; existing Lua host suite links only the vendored target; Arch package metadata and build no longer resolve a system Lua dependency | P2, P11 |
| 2 | Remove POSIX dependencies from otherwise portable tests by centralizing collision-resistant runtime paths in the shared helper, replacing executable permission setup with `std::filesystem::permissions`, and deleting unused native headers | `tests/test_helpers.h`; `tests/test_command_dispatch.cpp`; `tests/test_script_host.cpp`; `tests/test_theme.cpp`; `tests/test_tui_contract.cpp`; `tests/test_system_clipboard_reader.cpp`; `tests/test_file_seam_guard.cpp` | affected Linux suites pass; concurrent helper calls produce distinct paths; a named portable-test scan proves these files contain no POSIX headers or calls; staged MSVC target remains green | P1 |
| 3 | Add a `TerminalSession` implementation object and move `termios` state out of the public header without changing terminal behavior or source ownership | `include/ssg/Terminal.h`; `src/Terminal.cpp`; `tests/test_file_seam_guard.cpp`; `tests/test_terminal.cpp` | the seam guard permits `<termios.h>` only in `src/Terminal.cpp` and fails before the move; existing terminal mode-stack and signal-classification tests remain unchanged in outcome; Linux full suite and staged MSVC target remain green | P6 |
| 4 | Introduce the final `PlatformWake` and `PlatformEventLoop` interfaces, implement wake plus the current Linux `select`/self-pipe behavior behind them, and adopt wake notification in Git-diff and init-script producers without changing signal, input, or deadline semantics | `include/ssg/PlatformRuntime.h`; `src/platform/linux/LinuxPlatformRuntime.cpp`; `src/platform/windows/WindowsPlatformRuntime.cpp`; `include/ssg/GitDiffWorker.h`; `src/GitDiffWorker.cpp`; `include/ssg/GitDiffIngress.h`; `src/GitDiffIngress.cpp`; `include/ssg/InitScriptWatcher.h`; `src/InitScriptWatcher.cpp`; `include/ssg/Editor.h`; `src/Editor.cpp`; `src/main.cpp`; `tests/test_platform_interface.cpp`; `tests/test_platform_runtime.cpp`; `tests/test_git_diff_worker.cpp`; `tests/test_init_script_watcher.cpp`; `CMakeLists.txt` | cross-platform wake contract covers pre-wait, concurrent, coalesced, consume-before-drain, timeout, and idle destruction; new producer tests independently prove queue-before-notify and complete drain; existing application behavior remains green; staged MSVC job widens to build `ssg_core` with the Windows wake implementation satisfying the same wake oracle | P3, P4, P5, P10, P11 |
| 5 | Relocate shared platform code and the existing Linux and Windows filesystem/watcher backends into permanent source families, then expand the seam guard with exact-path native-header ownership and a shrinking exemption list for native calls not yet extracted | `src/platform/platform_files.cpp`; `src/platform/FilesystemWatcher.cpp`; `src/platform/linux/linux_files.cpp`; `src/platform/linux/LinuxFilesystemWatcher.cpp`; `src/platform/linux/LinuxGitMetadataWatcher.cpp`; `src/platform/windows/windows_files.cpp`; `src/platform/windows/WindowsFilesystemWatcher.cpp`; `src/platform/windows/WindowsGitMetadataWatcher.cpp`; `tests/test_file_seam_guard.cpp`; `tests/test_platform_interface.cpp`; `CMakeLists.txt` | moved platform-file and watcher suites remain green; `ssg_tests` links and `ctest -R test_platform_interface` passes; the guard rejects an injected native header outside its exact owner and names only the remaining terminal, clipboard, and runtime exemptions; staged MSVC `ssg_core` build remains green | P1, P2 |
| 6 | Replace the provisional Linux event-loop implementation with `signalfd`, `poll`, standard-input acquisition, process identity, startup clock, and termination operations; install the signal mask before any worker thread; move deadline selection into a pure shared policy | `include/ssg/PlatformRuntime.h`; `include/ssg/RuntimeTiming.h`; `src/RuntimeTiming.cpp`; `src/platform/linux/LinuxPlatformRuntime.cpp`; `src/main.cpp`; `include/ssg/Terminal.h`; `src/Terminal.cpp`; `tests/test_platform_runtime.cpp`; `tests/test_runtime_timing.cpp`; `tests/platform/linux/test_linux_platform_runtime.cpp`; `tests/test_terminal.cpp`; `tests/test_file_seam_guard.cpp`; `CMakeLists.txt` | inherited signal-mask child-thread test; signal-to-`TerminationRequest` and Linux re-raise child test; simultaneous-ready-source and empty-readiness tests; hand cases pin escape disambiguation, edge-scroll cadence, pending-search zero deadline, ordinary timed wait, and indefinite idle wait; boundary exemptions shrink; Linux full suite and staged MSVC `ssg_core` build remain green | P1, P3, P4, P5, P10, P11 |
| 7 | Move Linux terminal and clipboard native operations into the Linux family, retain shared mode-stack and paste policy in `ssg_terminal_objects`, move `InitScriptWatcher` into that test-linked target, and platform-select the POSIX helper oracle | `include/ssg/Terminal.h`; `src/Terminal.cpp`; `src/platform/linux/LinuxTerminal.cpp`; `include/ssg/SystemClipboardReader.h`; `src/SystemClipboardReader.cpp`; `src/platform/linux/LinuxSystemClipboardReader.cpp`; `src/InitScriptWatcher.cpp`; `tests/test_terminal.cpp`; `tests/test_system_clipboard_reader.cpp`; `tests/platform/linux/test_linux_terminal.cpp`; `tests/platform/linux/test_linux_system_clipboard.cpp`; `tests/test_file_seam_guard.cpp`; `CMakeLists.txt` | one injected terminal-state contract suite proves setup rollback and exact idempotent restoration; shared paste-planning tests; Linux-only helper suite proves exact output, timeout, reaping, and signal cleanup; boundary guard has no product-code exemption outside the two platform families; staged MSVC job widens to build `ssg_terminal_objects` | P1, P2, P6, P9, P11 |
| 8 | Implement pure Windows console-record translation and express canonical Linux byte cases and Windows records through shared expected `Decoded` fixtures | `src/platform/windows/WindowsConsoleInput.cpp`; `tests/fixtures/platform_input_cases.h`; `tests/test_terminal_input.cpp`; `tests/platform/windows/test_windows_console_input.cpp`; `CMakeLists.txt` | independent hand-authored `Decoded` fixtures cover text, surrogate pairs, repeats, functional and CSI-u keys, Ctrl-or-Alt `Mod`, AltGr text, pointer coordinates, buttons, drag, release, wheel, resize, malformed records, and ignorable records; Linux bytes and Windows records must produce the same fixture result; staged MSVC platform and terminal-object builds remain green | P2, P7, P8 |
| 9 | Complete the Windows event-loop backend with wake waiting, console acquisition, resize readiness, process control, process identity, clock, and cleanup acknowledgement | `src/platform/windows/WindowsPlatformRuntime.cpp`; `tests/platform/windows/test_windows_platform_runtime.cpp`; `CMakeLists.txt`; `.github/workflows/ci.yml` | shared wake oracle runs on Windows; a console-owning isolated CTest suite covers simultaneous handles, timeout, indefinite wait, resize, and ignorable-record draining; child-process control tests prove cleanup acknowledgement and termination status; staged MSVC job builds `ssg_tests` and runs the platform runtime and watcher suites | P2, P3, P4, P5, P7, P10, P11 |
| 10 | Implement Windows terminal modes, lifecycle, UTF-8 output, and native clipboard reading, reusing the shared terminal-state contract | `src/platform/windows/WindowsTerminal.cpp`; `src/platform/windows/WindowsSystemClipboardReader.cpp`; `tests/platform/windows/test_windows_terminal.cpp`; `tests/platform/windows/test_windows_system_clipboard.cpp`; `CMakeLists.txt`; `.github/workflows/ci.yml` | shared terminal-state suite covers QuickEdit, processed input, partial setup rollback, exact restoration, and repeated restore; Windows clipboard adapter tests cover contention, absent format, missing terminator, malformed UTF-16, empty and multiline Unicode, size, deadline, and close on every acquired path; staged MSVC full build and platform-selected suites pass | P2, P6, P9, P11 |
| 11 | Platform-select all native test registrations, remove the Windows leg from the generic release matrix, and promote the staged MSVC job into the required Windows configure, full-build, platform-interface, watcher, and full-CTest gate | `CMakeLists.txt`; `cmake/ssg_tests.cmake`; `.github/workflows/ci.yml`; `CMakePresets.json` | fresh `windows-latest` configure and full build; `ctest -R test_platform_interface`; Windows watcher suites; full Windows CTest; unchanged Linux CI | P1, P2, P6, P10, P11 |
| 12 | Document the native Windows build and runtime contract, perform Windows Terminal signoff, promote durable contracts into owning headers and the seam guard, and delete this specification in the implementation merge | `README.md`; `development.md`; `include/ssg/PlatformRuntime.h`; `include/ssg/Terminal.h`; `include/ssg/SystemClipboardReader.h`; `tests/test_file_seam_guard.cpp`; `spec-windows-build.md` | all automated acceptance gates; manual Windows Terminal signoff covers rendering, input, mouse, resize, clipboard, reload, Git refresh, idle, normal quit, and control termination | P1-P11 |

## Rationale (optional, skippable)

Compile-time platform selection matches the existing filesystem seam and avoids
runtime polymorphism in a single-process, single-client application. A single
platform subtree also makes accidental native dependencies visible and
machine-checkable.

Using Win32 console records instead of VT input bytes is deliberate. VT byte
input would reuse more parsing code, but reliable resize notification requires
the record queue, and mixing record and byte consumers leaves ordering and
consumption behavior unclear. Translating records once at the platform boundary
keeps native behavior testable and leaves routing, commands, editing, and
presentation shared.

`PlatformWake` is shared by both background producers rather than creating
worker-specific abstractions. The authoritative payload remains in each worker's
existing queue; the platform primitive carries readiness only, so notification
coalescing cannot change editor data.
