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

Key bindings already carry a `context` string (`KeyBinding.context`).  This spec
gives context meaning: a binding applies when its context is `*` (global) or
equals the current `FocusTarget` name (`editor`, `panel`, `prompt`).
`validate_keymap` treats a binding whose context is neither `*` nor a
`FocusTarget` name as `unreachable_binding`.  A client resolves a keystroke
against the published keymap **using the snapshot's focus as the context** and
applies no independent routing.  The same physical key can map to different
commands per focus - Down is `cursor.line_down` in `editor`,
`tree.select_next` in `panel`, and a prompt-list move in `prompt`.

### Printable input is also context-resolved

Printable/committed text is not special-cased by the client.  Each context binds
a **text sink** command that receives committed text as its argument:
`text.insert` in `editor`, `prompt.input` in `prompt` (a prompt text-input
command owned by the prompt feature), and no text sink in `panel` (printable
keys there are either bound navigation or ignored).  The client therefore has one
rule for every key, printable or not: resolve it in the snapshot's focus context
and dispatch.  This keeps N2 literally true.

### The Escape leader and cancel

Escape is both the global leader prefix and the natural cancel.  Disambiguation
is explicit:

- A key sequence beginning with Escape is matched against bindings whose context
  is `*` or the current focus.  If the bytes so far are a strict prefix of a
  longer candidate, the client waits up to a bound timeout (default 250 ms) for
  the next key.
- If a complete `*` chord matches (e.g. `Escape b`), it fires regardless of
  focus.  A complete focus-context binding matches only in that focus.
- A lone Escape that completes (timeout elapses, or the next key does not extend
  any candidate) resolves the current context's cancel/back binding: in `prompt`
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

Clients stay thin: read `focus` from the snapshot, use it as the keymap context
when translating a keystroke, dispatch the resolved command, and draw a focus
indicator on the focused region.  No client contains routing rules beyond
"context = snapshot focus".  The TUI replaces its `panel_visible` branching with
this rule.

### Rendering

The renderer marks the focused region using the active/inactive role split
already present for the panel, generalized to each surface.  The terminal cursor
is placed in the focused surface: the caret in `editor`, the selected tree row in
`panel`, the prompt input position in `prompt`.

## Invariants

- N1 (single focus): exactly one `FocusTarget` is focused; it is authoritative
  `EditorSession` state carried in its own snapshot section, and
  `panel_focused`/prompt-active are derived from it.
- N2 (context routing): every key - printable or not - is resolved in the
  current focus context through the keymap; the client uses the snapshot focus as
  the context and adds no independent routing.
- N3 (global reachability): `*`-context leader chords - including the
  `settings.open` escape hatch (I24) and the focus-change commands - resolve in
  every focus.
- N4 (no orphan focus): focus never targets a hidden or absent surface; a
  transition that would do so falls back to editor.
- N5 (prompt capture and stacked restore): opening a prompt pushes the current
  focus and focuses the prompt; closing pops and restores it (or editor if that
  surface is gone).  Printable input reaches the prompt via its context text
  sink.

## Considerations

- `FocusTarget` subsumes `ShellState.panel_focused`; that field is removed or
  made a derived accessor so the two can never disagree.
- Plan step 2 defines only context *semantics* and resolution; it authors no
  default bindings (owned by the keymap work) and tests resolution with synthetic
  context-tagged fixtures.
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
  restores to its parent prompt.
- The TUI contains no `panel_visible`-style routing; it routes solely by the
  snapshot's focus.
- Gates: `ctest --preset dev` green.
- Oracles: an independent focus-transition table (including tree-open, nested
  prompt push/pop, and surface-disappears rows) compared step-by-step against the
  library; context-resolution tests asserting one key yields the **exact expected
  command** per context against an independent reference mapping (not mere
  inequality); a test that a `*` leader chord and `settings.open` resolve in
  every focus and that bare Escape cancels a focused prompt; a snapshot round-trip
  carrying the focus section.

## Plan

| Step | Work | Files | Oracle |
|---|---|---|---|
| 1 | Add `FocusTarget` session state with the stacked transitions and an `editor.focus` command; derive `panel_focused`; expose a `FocusViewState` snapshot section and protocol codec | `include/ssg/ui_layout.h`, `src/ui_layout.cpp`, `src/runtime/*.cpp`, `include/ssg/session_snapshot.h`, `src/protocol.cpp`, `data/required-commands.json` and catalog fixtures, `tests/test_ui_layout.cpp`, `tests/test_protocol.cpp` | independent stacked focus-transition table; snapshot round-trip carries focus |
| 2 | Define keymap context semantics (`*`/`editor`/`panel`/`prompt`), the per-context text sink, and resolution + validation in the shared input layer | `include/ssg/input.h`, `src/input.cpp`, `include/ssg/prompt.h` (`prompt.input`), `tests/test_input.cpp` | one key resolves to the exact expected command per context; global chords resolve in all; unreachable-context rejected |
| 3 | Route TUI input by snapshot focus; remove `panel_visible` branching; draw the focus indicator and focus-aware cursor | `apps/ssg_main.cpp`, `apps/ssg_terminal.{h,cpp}`, `src/render.cpp`, `tests/test_ssg_app.cpp`, `tests/test_render.cpp` | `test_ssg_app` routing table; PTY focus demo |
