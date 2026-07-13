# Spec: focus and input routing

## Goals

Give the editor a single, library-owned **focus** so that at any moment exactly
one surface receives keyboard input, and every client routes keys the same way.
Today the TUI decides where a key goes with an ad-hoc `panel_visible` heuristic:
some keys reach the editor, some reach the bar, and hiding the bar silently
re-routes everything to the editor. There is no focus concept, so behavior is
inconsistent and cannot express the palette, fuzzy finder, search, or footer
actions.

## Design

### Focus is one piece of session state

The `EditorSession` owns a single focus value:

```
enum class FocusTarget { editor, panel, prompt };
```

- **editor** - the active pane; keys edit text and move the caret.
- **panel** - the left bar/tree; keys navigate and activate tree nodes.
- **prompt** - the active prompt surface (command palette, fuzzy file finder,
  find/replace, path entry, settings, and any other `PromptKind`); keys go to
  the prompt.  Settings/configuration input (I24) is `FocusTarget::prompt`, not a
  separate target.

`FocusTarget` is the single source of truth for focus.  The existing
`ShellState.panel_focused` becomes **derived** (`panel_focused == (focus ==
panel)`) rather than an independent field, and `focus == prompt` holds exactly
when a `PromptSurface` is active.  Focus is exposed to clients in its own small
snapshot section (`FocusViewState { FocusTarget target; }`), not folded into
`ShellViewState` (which is derived layout geometry), so a focus change produces a
one-enum delta rather than re-diffing the whole shell view.  The footer's
actionable status is not a focus target: `status.next`/`status.previous`/
`status.invoke_action` are global (`*`-context) commands, and search opens a
prompt (focus `prompt`).

### Input context follows focus

Key bindings carry a `context` string (`KeyBinding.context`): a binding applies
when its context is `*` (global) or equals the current `FocusTarget` name
(`editor`, `panel`, `prompt`).  `validate_keymap` rejects a binding whose context
is neither `*` nor a `FocusTarget` name with `unknown_context` (see
`doc/spec-keymap.md`).  A client
resolves a keystroke against the published keymap **using the snapshot's focus as
the context**; resolution is client-local so typing and command dispatch never
round-trip (consistent with `doc/spec-keymap.md`, which owns the keymap and
resolution model).  The same physical key can map to different
commands per focus - Down is `cursor.line_down` in `editor`, `tree.select_next`
in `panel`, and a prompt-list move in `prompt`.

### Leader is client-resolved, server-presented

Resolution is latency-sensitive; presentation must be consistent.  So the split
is: the client resolves keys and owns the transient pending multi-key (leader)
sequence locally, but the library owns how leader mode is *presented*.

- Pending leader state is client-local input-capture state derived from the
  server-published keymap (as in `doc/spec-keymap.md`): bindings in one context are
  prefix-free, so no timeout or server round-trip is needed to resolve a chord.
- When the client enters, extends, or clears leader mode, it reports the current
  pending sequence to the library.  For the in-process terminal client this is an
  ingress **parameter** of the per-client snapshot call
  (`snapshot(client_id, dimensions, leader_pending)`), exactly like per-client
  viewport dimensions - one report per leader transition, never per typed
  character, and not a registered command.  The shell view state is materialized
  per snapshot call, so the leader hint it carries is per-client and never leaks
  into another client's render even though `ShellViewState` is otherwise shared
  session-shaped state.  The equivalent browser wire message is deferred with the
  low-latency input model below.
- The library forms the hint text from the reported `KeySequence` using its own
  key-name source (human key names, e.g. `leader: Escape`) and places it in the
  shell's status area using a theme role for color; the client draws the
  server-described cells.  Layout and color stay server-owned (I7, I17, I22); the
  client uses the server-described placement and role and invents neither.
- A library-initiated focus or keymap change (including `settings.open`) that
  invalidates a pending chord requires the client to report an empty sequence on
  its next snapshot call, so a stale hint cannot outlive its context.
- Because leader state is per-client input capture, each attached client shows
  its own leader hint.

This is the affordance distant clients need: a client reports an input-capture
state and the library owns its presentation.  The pattern generalizes to IME
preedit and other capture states.  The full perceived-low-latency input model
(optimistic local echo, keystroke coalescing, reconciliation) is deferred and
builds on this seam rather than replacing it.

### Printable input is context-resolved by the client

Printable/committed text is resolved locally like any other key.  Each context
has a **text sink**: `text.insert` in `editor`, a prompt text-input command in
`prompt`, and none in `panel` (printable keys there are bound navigation or
ignored).  Committed text that is not part of a pending chord dispatches to the
current context's text sink.

### The Escape leader and cancel

Escape is the leader prefix.  Following `doc/spec-keymap.md`, context bindings are
**prefix-free**, so resolution is unambiguous without a timeout: `Escape` begins
a pending sequence, and the next keys either complete a binding or clear it.
Escape has no implicit cancel/dismiss meaning; cancel and dismiss are ordinary
commands bound in their context (e.g. an explicit `[Escape, Escape]` binding
cancels a prompt).  A `*` chord (e.g. `Escape b`) resolves in every focus; a
focus-context binding resolves only in that focus, with `*` taking precedence so
the `settings.open` escape hatch (I24) is never shadowed.  A non-matching
continuation clears the pending sequence and is not suppressed.

### Focus transitions

Focus is changed only by commands, so it is scriptable and testable.  The
interrupted-focus memory is a **stack** so prompts nest correctly.

| Trigger | Effect on focus |
|---|---|
| `panel.focus` (panel visible) | focus = panel |
| `editor.focus` (new command, step 1) or focusing a pane | focus = editor |
| `panel.toggle` hides the panel while focus == panel | focus = editor |
| Any prompt opens (`palette.open`, `find.open`, `goto.file`, `settings.open`, a command-argument prompt, ...) | push current focus; focus = prompt |
| A prompt opens while focus == prompt (nested) | push prompt; focus = prompt |
| Prompt submits or cancels | pop the stack; focus = popped target if its surface still exists, else editor |
| `tree.activate` opens a file while focus == panel | focus = editor (opening a document focuses it) |
| The focused surface disappears (panel hidden, pane closed) with an empty stack | focus = editor |

Focus never targets a hidden or absent surface: every transition that could land
on a missing surface falls back to editor.

### Client responsibility

Clients stay thin: decode platform input into keystrokes/committed text, resolve
them locally against the published keymap using the snapshot's focus as context,
dispatch the resolved command, and draw a focus indicator on the focused region.
When leader mode is entered, extended, or cleared, the client reports the pending
sequence to the library as per-client presentation state and renders the
server-produced leader hint.  A client contains no routing rules beyond
"context = snapshot focus" and invents no UI; the TUI replaces its `panel_visible`
branching and hard-coded chords with keymap-driven local resolution.

### Rendering

The renderer marks the focused region using the active/inactive role split
already present for the panel, generalized to each surface.  The terminal cursor
is placed in the focused surface: the caret in `editor`, the selected tree row in
`panel`, the prompt input position in `prompt`.  When a client's reported leader
sequence is non-empty, the library places a leader hint in that client's status
area in a distinct theme role.

## Invariants

- N1 (single focus): exactly one `FocusTarget` is focused; it is authoritative
  `EditorSession` state carried on the snapshot, and `panel_focused`/prompt-active
  are derived from it.
- N2 (context routing): every key - printable or not - is resolved by the client
  locally in the current focus context through the published keymap; the client
  uses the snapshot focus as the context and adds no independent routing.
- N3 (global reachability): `*`-context leader chords - including the
  `settings.open` escape hatch (I24) and the focus-change commands - resolve in
  every focus, and `*` takes precedence over a focus binding for the same
  sequence.
- N4 (no orphan focus): focus never targets a hidden or absent surface; a
  transition that would do so falls back to editor.
- N5 (prompt capture and stacked restore): opening a prompt pushes the current
  focus and focuses the prompt; closing pops and restores it (or editor if that
  surface is gone).
- N6 (server-presented leader): the pending leader sequence is client-local input
  capture, but its presentation is library-owned - the client reports it as
  per-client snapshot ingress and the library places a theme-colored leader hint;
  the client uses the server-described placement and role and invents neither.
  This does not require a round trip before the hint may appear: a future
  low-latency client may render the same server-described role optimistically.

## Considerations

- `FocusTarget` subsumes `ShellState.panel_focused`; that field is removed or
  made a derived accessor so the two can never disagree.
- Leader resolution stays client-local per `doc/spec-keymap.md`; this spec adds
  only the presentation seam (per-client reported pending sequence -> library
  leader hint).  The full distant-client perceived-latency input model
  (optimistic echo, coalescing, reconciliation) is explicitly deferred.
- The per-client leader report is presentation input like viewport dimensions,
  not a registered command, so it adds no command-catalog or Lua surface.
- Focus is per session, consistent with shared shell state; every attached client
  observes the same focus, but each renders its own leader hint.
- Multiple panes are one `editor` focus; which pane is active remains existing
  active-pane state.

## Risks and mitigations

- Hidden re-routing regressions: cover focus transitions with an independent
  transition table, not only end-to-end checks.
- Leader visibility: test that entering leader mode produces a non-empty reported
  sequence and a rendered hint, and that completing or abandoning the chord clears
  it.
- Nested prompts: test push/pop restores the correct prior focus.

## Acceptance (Definition of Done)

- Observable: at all times exactly one region is focused and visibly indicated;
  keyboard input affects only that region.  `ESC b` focuses the bar and every key
  then drives the bar; opening the palette captures typing; a nested prompt
  restores to its parent prompt.  Pressing the leader key shows a theme-colored
  `leader:` hint in the status area until the chord completes or is abandoned, and
  the hint's placement and color come from the library, not the client.
- The TUI contains no `panel_visible`-style routing and no hard-coded chord
  table; it resolves keys locally against the published keymap and reports leader
  state for presentation.
- Gates: `ctest --preset dev` green.
- Oracles: an independent focus-transition table (including tree-open, nested
  prompt push/pop, and surface-disappears rows) compared step-by-step against the
  library; a leader-hint test proving a reported non-empty sequence renders a
  status hint whose text reflects the pending keys and an empty sequence renders
  none; a per-client isolation test where one client is in leader mode and a
  second client's snapshot renders no hint; when local keymap resolution lands,
  context-resolution tests asserting one key yields the **exact expected command**
  per context and that `*` chords resolve in every focus with precedence over
  focus bindings.

## Plan

Steps 1 and 3 (FocusTarget session state and focus-based TUI routing) have
landed.  The remaining work:

| Step | Work | Files | Oracle |
|---|---|---|---|
| A1 | Add reported leader sequence as per-client snapshot ingress (a `snapshot()` parameter threaded to the shell view); format the hint from the key sequence via a server key-name source | `include/ssg/editor_runtime.h`, `include/ssg/ui_layout.h`, `src/ui_layout.cpp`, `src/runtime/snapshot.cpp`, `src/editor_runtime.cpp`, `tests/test_ui_layout.cpp` | non-empty sequence yields a status hint whose text reflects the keys; two clients, one in leader, the other's snapshot renders no hint (per-client isolation) |
| A2 | Render the theme-colored `leader:` hint in the status area | `src/render.cpp`, `tests/test_render.cpp` | render places the hint text in the status region in the leader theme role |
| B | TUI derives the pending sequence from the published keymap and reports it via the snapshot parameter; replace hard-coded chords with keymap-driven local resolution | `apps/ssg_main.cpp`, `apps/ssg_terminal.{h,cpp}`, `tests/test_ssg_app.cpp` | `test_ssg_app` resolution table; PTY leader-hint demo |
| C (done in `doc/spec-keymap.md`, M6) | Assign keymap contexts (`*`/`editor`/`panel`/`prompt`), per-context text routing, and `validate_keymap` context checks (`unknown_context`) | `include/ssg/input.h`, `src/input.cpp`, `tests/test_input.cpp` | exact-command-per-context resolution; `*`-precedence; unknown-context rejected — see `doc/spec-keymap.md` |
