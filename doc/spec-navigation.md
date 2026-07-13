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

### Focus is library state

The session owns a `FocusTarget`:

```
enum class FocusTarget { editor, panel, prompt };
```

- **editor** — the active pane; keys edit text and move the caret.
- **panel** — the left bar/tree; keys navigate and activate tree nodes.
- **prompt** — the active prompt surface (command palette, fuzzy file finder,
  find/replace, path entry, settings); keys go to the prompt.

Exactly one target is focused. Focus is authoritative session state and is
exposed on the snapshot (a `focus` field on the shell view state), so clients
render a focus indicator and route input identically. The footer's actionable
status is not a persistent focus target: its actions are reached through global
status commands, and search opens a prompt (focus `prompt`).

### Input context follows focus

Key bindings already carry a `context` string (`KeyBinding.context`). This spec
gives context meaning: a binding applies when its context is `*` (global) or
equals the current `FocusTarget` name (`editor`, `panel`, `prompt`). A client
resolves a keystroke against the published keymap **using the snapshot's focus
as the context** and applies no independent routing. The same physical key can
map to different commands per focus — for example Down is `cursor.line_down` in
`editor`, `tree.select_next` in `panel`, and a prompt-list move in `prompt`.

Global (`*`) bindings — the Escape leader chords — resolve in every focus. They
are the escape hatch: panel toggle, palette open, `settings.open` (I24), and the
focus-change commands remain reachable no matter what is focused. Inside a
prompt, printable keys go to the prompt; only leader chords escape it.

### Focus transitions

Focus is changed only by commands, so it is scriptable and testable:

| Trigger | Resulting focus |
|---|---|
| `panel.focus` (panel visible) | panel |
| `panel.toggle` hides the panel | editor (if it was panel) |
| A focus-editor command (`pane.focus`/`editor.focus`) | editor |
| Any prompt opens (`palette.open`, `find.open`, `goto.file`, `settings.open`, …) | prompt |
| Prompt submits or cancels | the target focused before the prompt opened |
| The focused surface disappears (panel hidden, prompt closed, pane closed) | a still-present target, defaulting to editor |

Focus never targets a hidden or absent surface. The prompt remembers the focus
it interrupted and restores it on close.

### Client responsibility

Clients stay thin: read `focus` from the snapshot, use it as the keymap context
when translating a keystroke, dispatch the resolved command, and draw a focus
indicator on the focused region. No client contains routing rules beyond
"context = snapshot focus". The TUI replaces its `panel_visible` branching with
this rule.

### Rendering

The renderer marks the focused region (the existing `panel_active`/
`panel_inactive` role split generalizes: the focused surface uses its active
role, others the inactive role). The caret is shown only when the editor is
focused; when the panel or a prompt is focused the terminal cursor moves to that
surface's selection.

## Invariants

- N1 (single focus): exactly one `FocusTarget` is focused; it is authoritative
  session state carried on the snapshot.
- N2 (context routing): a non-global key is resolved in the current focus
  context; the client uses the snapshot focus as the keymap context and adds no
  independent routing.
- N3 (global reachability): `*`-context leader chords — including the
  `settings.open` escape hatch (I24) and the focus-change commands — resolve in
  every focus.
- N4 (no orphan focus): focus never targets a hidden or absent surface; losing
  the focused surface moves focus to a present target, defaulting to editor.
- N5 (prompt capture and restore): opening a prompt focuses it and captures
  printable input; closing restores the previously focused target.

## Considerations

- The keymap's default bindings must be assigned contexts; the specific bindings
  are owned by the keymap work (M6). This spec owns the focus model and the
  routing contract, not the binding table.
- Focus is per session, not per client, consistent with shared shell state;
  every attached client observes the same focus.
- Multiple panes are one `editor` focus; which pane is active remains the
  existing active-pane state.

## Risks and mitigations

- Hidden re-routing regressions: cover focus transitions with an independent
  transition table rather than only end-to-end checks.
- Prompt/leader ambiguity: test that a leader chord fires while a prompt is
  focused and that printable keys still reach the prompt.

## Acceptance (Definition of Done)

- Observable: at all times exactly one region is focused and visibly indicated;
  keyboard input affects only that region. `ESC b` focuses the bar and every key
  then drives the bar; a focus-editor action returns to editing; opening the
  palette captures typing; Escape closes it and restores the prior focus.
- The TUI contains no `panel_visible`-style routing; it routes solely by the
  snapshot's focus.
- Gates: `ctest --preset dev` green.
- Oracles: an independent focus-transition table compared against the library;
  context-resolution tests asserting one key yields different commands under
  `editor`/`panel`/`prompt`; a test that a global leader chord and
  `settings.open` resolve in every focus; a snapshot round-trip carrying focus.

## Plan

| Step | Work | Files | Oracle |
|---|---|---|---|
| 1 | Add `FocusTarget` session state, transition commands/rules, and expose `focus` on the shell view state and protocol | `include/ssg/ui_layout.h`, `src/ui_layout.cpp`, `src/runtime/*.cpp`, `include/ssg/session_snapshot.h`, `src/protocol.cpp`, `tests/test_ui_layout.cpp`, `tests/test_protocol.cpp` | independent focus-transition table; snapshot round-trip carries focus |
| 2 | Define keymap context semantics (`*`/`editor`/`panel`/`prompt`) and resolution in the shared input layer | `include/ssg/input.h`, `src/input.cpp`, `tests/test_input.cpp` | one key resolves to different commands per context; global chords resolve in all |
| 3 | Route TUI input by snapshot focus; remove `panel_visible` branching; draw the focus indicator and focus-aware cursor | `apps/ssg_main.cpp`, `apps/ssg_terminal.{h,cpp}`, `src/render.cpp`, `tests/test_ssg_app.cpp`, `tests/test_render.cpp` | `test_ssg_app` routing table; PTY focus demo |
