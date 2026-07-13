# spec-browser-input

## Goals

Make all required interaction workflows reachable in Chromium, Firefox, and WebKit while keeping platform input capture and clipboard permission handling in clients.

## Design

The API publishes the authoritative keymap, required command metadata, semantic
hit targets, clipboard requests, and the current principal's host-granted
capability IDs in per-client snapshot state. Clients map raw events to semantic
commands. The default keymap uses canonical `Escape` as its leader. Text
commitment, Backspace/Delete, Enter, Tab, arrows, Home/End, and Page Up/Down
remain direct semantic bindings; commands that do not require those primitive
keys use accepted leader-prefixed sequences. The complete default sequence
fixture, rather than command-list position, is authoritative.
`settings.open` defaults to `[Escape, KeyF, KeyT]` with context `*`. Context
`*` means global: it is evaluated before focus- or mode-specific bindings and
remains active in every client state. A browser-deliverable sequence is observed
by Chromium, Firefox, and WebKit in a normal SSG page, activates no browser or
OS function, and can be suppressed after matching begins. Named server-owned
keymaps may choose another leader or explicit prefix-free sequences, but
validation rejects a keymap that lacks at least one browser-deliverable global
`settings.open` sequence. The accepted required-command fixture is the exact
union of normative command IDs in all P0 feature specs and is reviewed before
registry/keymap implementation. Every entry records its owning task, required
capabilities, and Lua/keymap/palette availability.
`file.open_dropped_content` is the only capability-gated entry: it requires
`local_file_drop` and is excluded from Lua, keymaps, and the palette. Its
server-described local-file control is focusable and keyboard invokable because
the browser must obtain a user-selected payload before submitting the command.
Clipboard read has the same browser-gesture constraint; its keyboard command is
the qualifying gesture when the browser permits it. Every other required entry
has no required capability and is available through Lua, keymap, and palette
surfaces. Clipboard denial uses the internal register and produces an
actionable footer status. Browser drag-and-drop is excluded for ordinary remote
clients. A client renders and emits file ingress only when the snapshot
describes that UI element and capabilities include `local_file_drop`; common
dispatch independently enforces the same grant.

## Invariants

I6, I7, I16, I17, I18, I20, I24 from `doc/spec.md`.

## Considerations

- IME submits committed UTF-8 text, never raw composition internals.
- Browser-reserved chords cannot be required defaults. The accepted denylist
  lives at `tests/browser/fixtures/reserved-chords.json` and validates named
  keymaps as well as defaults.
- The reserved fixture includes `Ctrl+Shift+KeyM`; it is not a valid default
  because Chromium-based browsers consume it for device/mobile emulation.
- Pressing `Escape` starts the leader sequence and suppresses that key event in
  every focus state, including prompt and settings inputs. `Escape` has no
  implicit cancel/dismiss meaning; cancel and dismiss are ordinary
  server-published command bindings. Pressing `Escape` again restarts the
  leader unless an explicit complete `[Escape, Escape]` binding exists in the
  active context. A non-matching continuation clears pending leader state and
  is not suppressed, so its normal text/control behavior continues. Pending
  leader state is client-local input-capture state derived exclusively from the
  current server-published keymap and is cleared on keymap or focus changes.
  Bindings in one context are prefix-free, so no timeout or client-owned
  disambiguation policy is required.
- `settings.open` is resolved before focus- or mode-specific bindings. Invoking
  it cancels any pending leader sequence, exits distraction-free mode when
  necessary, opens the server-described configuration input, and focuses its
  first control. It does not depend on an open document or writable buffer.
- The default keymap covers every catalog entry whose `keymap` field is true;
  direct primitive aliases may give a command more than one binding. The
  capability-gated ingress command `file.open_dropped_content` has no binding;
  coverage tests reject either a missing eligible command or a binding for that
  excluded command.
- Semantic input data reuses the existing text-input, selection, and viewport
  command argument types. IME input is a validated committed UTF-8 value and
  never exposes composition internals. Hit targets carry sufficient typed
  metadata to reconstruct those semantic arguments without backend input
  capture.
- Mouse selection, wheel, and scrollbar gestures use the same semantic command path as keyboard navigation.
- Locality is host-granted; browser code may not infer or self-assert it.
- Plan 3 supplies a test-only browser client, not the product browser fixture.
  `tests/browser/input/harness.html` loads the checked-in keymap and captured
  event cases through `tests/browser/input/browser-input.mjs`. A dependency-free
  Node runner serves those files over loopback HTTP, launches already-installed
  Chromium, Firefox, and WebKit executables, and collects each runtime's
  event-to-semantic report. Browser-native automation supplies trusted events
  when the installed runtime exposes it; otherwise the live runtime constructs
  and dispatches standards DOM events and records the exact fields observed by
  its listeners. Checked-in cases are replayed by the source gate even when a
  browser executable is unavailable; the required live matrix gate fails with
  the missing runtime names.
- Plan 3 adds browser-layer coverage beyond the C++ input/keymap contract:
  engine-observed key events map to semantic commands, composition updates are
  suppressed until one committed UTF-8 insertion, pointer/wheel/scrollbar
  gestures produce existing semantic argument shapes, and reserved chords are
  neither mapped nor suppressed.
- Clipboard denial and unavailable APIs fall back to the test client's internal
  register and emit the typed
  **clipboard.system_paste_unavailable** status with a non-empty action label.
  The harness observes this semantic status object, never DOM presentation.
- File-drop coverage in this task is client-side capability gating only. The
  test-only local client renders and emits dropped content only when its
  snapshot capabilities contain `local_file_drop`; remote and locality-unknown
  snapshots do neither. Common-dispatch capability rejection remains owned by
  session/file-command integration and is not reimplemented by this harness.
- The product-style browser fixture is served from a dependency-free loopback
  HTTP server and opens a separate WebSocket to `HttpEditorServer`; the editor
  transport remains exactly one ordered WebSocket. The loopback origin is a
  secure context for gesture-gated clipboard APIs.
- The browser fixture owns a small JavaScript implementation of the documented
  binary protocol envelope and `ProtocolValue` tree. Checked-in C++ protocol
  fixture bytes are the anti-drift oracle for JavaScript decoding and command
  encoding.
- Remote and local-capability pages use one client implementation. Test
  credentials select host-created principals; only the local principal is
  granted `local_file_drop`. Credentials are URL query inputs supplied by the
  fixture host, never inferred or self-granted by browser code. Reconnects send
  the last applied revision from the same client state.
- The mandatory browser workflow is the `doc/spec.md` Observable browser
  workflow: open, edit, multi-selection, clipboard, undo/redo, wrap, wheel and
  scrollbar navigation, save, dirty close/reopen, read-only rejection, diff,
  recovery, external-change follow/pause/resume, capability-gated drop, theme,
  prompt, status, and accessibility surfaces.
- Key bindings, hit targets, cell runs, theme values, prompts, statuses, and
  accessible labels are rendered only from snapshot/delta API data. The client
  does not load the default keymap as product behavior or synthesize editor
  semantics and labels.
- Browser client conformance uses the existing dependency-free live-browser
  launcher policy: source tests always run, while the required live gate names
  and fails for any unavailable Chromium, Firefox, or WebKit runtime. Identical
  semantic scripts compare canonical protocol state after direct-API and
  WebSocket execution.

## Risks and Mitigations

- Browser differences: use real-browser event capture, not synthetic unit events alone.
- Circular command coverage: compare implementation against the independently accepted fixture.

## Acceptance (Definition of Done)

- Observable: every required interactive command has a browser-deliverable
  route or a documented gesture-gated flow; `Escape`, `F`, `T` opens and focuses
  server-described configuration input from every enumerated client state.
- Budgets: input-to-command translation adds no backend work.
- Gates: Chromium, Firefox, and WebKit conformance suites are green.
- Oracles: captured event fixtures; a denylist containing
  `Ctrl+Shift+KeyM`; hand-authored `Escape` leader and
  `settings.open = [Escape, KeyF, KeyT]` cases; table-driven configuration-open
  cases covering editor, panel, prompt, empty, read-only, diff, and
  distraction-free states; atomic rejection of keymaps without a global
  configuration binding; IME goldens; clipboard permission/gesture cases;
  required-command coverage; and local-accept/remote-reject file-drop
  capability cases.

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Accept required-command and browser-reserved fixtures | `data/required-commands.json`, `tests/browser/fixtures/reserved-chords.json` | exact comparison with the union of all P0 normative lists, independently maintained category/count/owner data, exact capability and Lua/keymap/palette exclusions, and browser docs/capture including `Ctrl+Shift+KeyM` rejection | I6, I18, I20 |
| 2 | Define direct primitive bindings, the `Escape` default leader, global configuration binding, context/prefix validation, and hit-target API data | `include/ssg/input.h`, `src/input.cpp`, `data/default-keymap.json`, `tests/browser/fixtures/default-keymap.json`, `tests/test_input.cpp` | independently accepted direct/leader sequence fixture including `settings.open = [Escape, KeyF, KeyT]`; complete keymap-eligible command coverage; atomic rejection of excluded, duplicate, unreachable, reserved, ambiguous-prefix, or configuration-lockout bindings; committed UTF-8 and semantic hit-target round trips; backend dependency scan | I6, I16, I17, I24 |
| 3 | Implement the test-only browser input, IME, mouse, clipboard, and file-drop conformance harness without the product browser fixture | `tests/browser/input/*`, `cmake/components/browser-input-conformance.cmake` | checked-in event/semantic cases plus a dependency-free loopback runner against already-installed Chromium, Firefox, and WebKit; IME commit, clipboard denial/internal fallback/status, reserved chord, pointer/wheel/scrollbar, and local-only file-drop cases | I6, I18 |
| 4 | Implement the thin product browser input adapter, including leader restart/reset and global configuration access, and complete browser-client oracle | `examples/browser/*`, `tests/browser/client/*`, `cmake/components/browser-client.cmake` | canonical C++ wire fixtures, direct-API/WebSocket state parity, scripted Chromium/Firefox/WebKit leader and configuration-access workflows in every enumerated state, API-sourced accessibility snapshots, and remote/local-capability cases | I6, I7, I16, I17, I18, I20, I24 |

## Rationale (optional, skippable)

Browser feasibility is a product boundary, not a later client compatibility task.
`Escape` is the initial leader because it is unmodified, terminal-friendly,
and accepted as sufficient for the current product. Direct primitive bindings
preserve modeless editing fluency, while the global settings sequence makes a
later server-owned remap recoverable.
