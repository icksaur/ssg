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

### Key resolution and leader state are library-owned

Key resolution is editor behavior, so it lives in the library, not the client.
The client decodes platform input into logical `KeyStroke`s and committed text
(the only client responsibility) and submits each as it arrives; the library
resolves it against the keymap and the current focus, maintains the pending
multi-key sequence, and dispatches the resolved command internally.  A client
never decides which command a key maps to and never holds chord state.

The session owns a `PendingKeySequence` (a `KeySequence`, empty when idle).  On
each submitted `KeyStroke` the library, using the current focus as context:

1. Appends the stroke to the pending sequence.
2. If the sequence exactly matches a binding whose context is `*` or the current
   focus, dispatches that command and clears the pending sequence.
3. Else if the sequence is a strict prefix of some such binding, keeps it pending
   (this is "leader mode") and publishes it.
4. Else clears the pending sequence; if the sequence was a single committed-text
   stroke and the focus context has a text sink, routes it there instead.

The pending sequence is published on the snapshot so every client renders a
consistent leader hint (for example a `leader: Esc ` indicator in a distinct
theme role).  A bound inactivity timeout is a client concern only insofar as the
client may submit a synthetic "sequence timed out" signal; the authoritative
pending state and its resolution remain in the library.

### Input context follows focus

Key bindings carry a `context` string (`KeyBinding.context`): a binding applies
when its context is `*` (global) or equals the current `FocusTarget` name
(`editor`, `panel`, `prompt`).  `validate_keymap` treats a binding whose context
is neither `*` nor a `FocusTarget` name as `unreachable_binding`.  The same
physical key can resolve to different commands per focus - Down is
`cursor.line_down` in `editor`, `tree.select_next` in `panel`, and a prompt-list
move in `prompt` - because the library resolves it in the current context.

### Printable input is also context-resolved

Printable/committed text is not special-cased.  Each context binds a **text
sink** command that receives committed text as its argument: `text.insert` in
`editor`, `prompt.input` in `prompt` (a prompt text-input command owned by the
prompt feature), and no text sink in `panel` (printable keys there are either
bound navigation or ignored).  Committed text that is not part of a pending
chord routes to the current context's text sink.

### The Escape leader and cancel

Escape is both the global leader prefix and the natural cancel.  Disambiguation
is explicit and resolved by the library:

- A pending sequence beginning with Escape is matched against bindings whose
  context is `*` or the current focus.  While the pending sequence is a strict
  prefix of a longer candidate, it stays pending (published as leader state).
- If a complete `*` chord matches (e.g. `Escape b`), it fires regardless of
  focus.  A complete focus-context binding matches only in that focus.
- A lone Escape that completes (the client submits a timeout signal, or the next
  key does not extend any candidate) resolves the current context's cancel/back
  binding: in `prompt`
  that is `prompt.cancel`; in `panel` it returns focus to the editor; in
  `editor` it clears secondary selections or is a no-op.

Because bare Escape is a one-key `prompt`/`panel` binding and the leader chords
are longer `*` sequences, the same prefix-with-timeout rule the keymap already
uses resolves both without a separate mechanism.

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

Clients stay thin: decode platform input into `KeyStroke`s and committed text,
submit each to the library, read `focus` and the pending leader sequence from the
snapshot, draw a focus indicator on the focused region, and render the leader
hint when the pending sequence is non-empty.  A client never resolves a key to a
command, holds chord state, or contains routing rules; resolution is entirely the
library's.  The TUI replaces both its `panel_visible` branching and its local
`ESC`+key chord decoding with key submission.

### Rendering

The renderer marks the focused region using the active/inactive role split
already present for the panel, generalized to each surface.  The terminal cursor
is placed in the focused surface: the caret in `editor`, the selected tree row in
`panel`, the prompt input position in `prompt`.  When the pending leader sequence
is non-empty, the status area shows a leader hint (for example `leader: Esc `) in
a distinct theme role.

## Invariants

- N1 (single focus): exactly one `FocusTarget` is focused; it is authoritative
  `EditorSession` state carried in its own snapshot section, and
  `panel_focused`/prompt-active are derived from it.
- N2 (library-owned resolution): every key - printable or not - is resolved by
  the library in the current focus context; the client submits `KeyStroke`s and
  committed text and contains no keymap resolution, routing, or chord state.
- N3 (global reachability): `*`-context leader chords - including the
  `settings.open` escape hatch (I24) and the focus-change commands - resolve in
  every focus.
- N4 (no orphan focus): focus never targets a hidden or absent surface; a
  transition that would do so falls back to editor.
- N5 (prompt capture and stacked restore): opening a prompt pushes the current
  focus and focuses the prompt; closing pops and restores it (or editor if that
  surface is gone).  Printable input reaches the prompt via its context text
  sink.
- N6 (published leader state): the pending multi-key sequence is authoritative
  library state published on the snapshot; every client renders the same leader
  hint and no client holds private chord state.

## Considerations

- `FocusTarget` subsumes `ShellState.panel_focused`; that field is removed or
  made a derived accessor so the two can never disagree.
- Plan step 2 moves key resolution into the library: it resolves submitted
  `KeyStroke`s against the keymap with focus context, owns the pending sequence,
  and dispatches resolved commands.  It authors no default bindings (owned by the
  keymap work) and tests resolution with synthetic context-tagged fixtures.  The
  focus state and TUI focus-routing (steps 1 and 3) already landed against a
  client that resolves keys locally; step 2 replaces that local resolution with
  key submission, so the TUI decode-to-command logic is removed.
- Focus is per session, consistent with shared shell state; every attached client
  observes the same focus.
- Multiple panes are one `editor` focus; which pane is active remains existing
  active-pane state.

## Risks and mitigations

- Hidden re-routing regressions: cover focus transitions with an independent
  transition table, not only end-to-end checks.
- Leader/cancel ambiguity: test that a `*` chord fires while a prompt is focused,
  that bare Escape cancels the prompt, and that printable keys still reach the
  prompt text sink.
- Nested prompts: test push/pop restores the correct prior focus.

## Acceptance (Definition of Done)

- Observable: at all times exactly one region is focused and visibly indicated;
  keyboard input affects only that region.  `ESC b` focuses the bar and every key
  then drives the bar; `editor.focus` returns to editing; opening the palette
  captures typing; Escape closes it and restores the prior focus; a nested prompt
  restores to its parent prompt.  Pressing the leader key shows a `leader:`
  hint in the status area in a distinct color until the chord completes or is
  abandoned.
- The TUI contains no `panel_visible`-style routing and no local key-to-command
  resolution; it submits `KeyStroke`s and committed text and renders the
  published focus and pending-leader state.
- Gates: `ctest --preset dev` green.
- Oracles: an independent focus-transition table (including tree-open, nested
  prompt push/pop, and surface-disappears rows) compared step-by-step against the
  library; context-resolution tests asserting one key yields the **exact expected
  command** per context against an independent reference mapping (not mere
  inequality); a pending-sequence test proving a leader prefix stays pending, a
  completed chord fires and clears it, and a non-matching stroke clears it; a test
  that a `*` leader chord and `settings.open` resolve in every focus and that bare
  Escape cancels a focused prompt; a snapshot round-trip carrying the focus and
  pending-sequence state.

## Plan

| Step | Work | Files | Oracle |
|---|---|---|---|
| 1 | Add `FocusTarget` session state with the stacked transitions and an `editor.focus` command; derive `panel_focused`; expose focus on the shell view state and protocol codec | `include/ssg/ui_layout.h`, `src/ui_layout.cpp`, `src/runtime/*.cpp`, `src/protocol.cpp`, `tests/test_ui_layout.cpp`, `tests/test_protocol.cpp` | independent stacked focus-transition table; snapshot round-trip carries focus |
| 2 | Move key resolution into the library: an `input.key` command that appends a submitted `KeyStroke` to a session `PendingKeySequence`, resolves it against the keymap in the current focus context, dispatches a matched command, keeps a prefix pending, and clears on no match; publish the pending sequence on the snapshot; define keymap context semantics (`*`/`editor`/`panel`/`prompt`) and validation | `include/ssg/input.h`, `src/input.cpp`, `src/runtime/*.cpp`, `include/ssg/session_snapshot.h`, `src/protocol.cpp`, catalog fixtures, `tests/test_input.cpp` | pending-sequence resolution table; one key resolves to the exact expected command per context; global chords resolve in all; round-trip carries pending state |
| 3 | Submit keys from the TUI instead of resolving locally; render the focus indicator, focus-aware cursor, and the `leader:` status hint | `apps/ssg_main.cpp`, `apps/ssg_terminal.{h,cpp}`, `src/render.cpp`, `tests/test_ssg_app.cpp`, `tests/test_render.cpp` | `test_ssg_app` key-submission table; PTY leader-hint demo |
