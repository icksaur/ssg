# spec-goto-line — Go-to-line prompt (Alt+Shift+G)

Status: draft (V1)

## Goal

Pressing **Alt+Shift+G** (or invoking `goto.line` from the palette) opens a
one-field prompt asking for a line number. Submitting a number moves the active
document's caret to the start of that line — 1-based, clamped to the document's
line range — and reveals it. This is the last of the two backlog items.

## Background — what already exists

- `goto.line` is a registered catalog command but is **inert**: it is created by
  the shared `jump()` helper in `src/runtime/navigation.cpp` alongside
  `goto.file`/`goto.symbol` as an `optionalInProcessHandler<NavigationTarget>`.
  Nothing anywhere dispatches it (no picker uses it; `goto.file`/`goto.symbol`
  have fuzzy finders, `goto.line` does not). Grep confirms zero dispatch sites in
  app, library, or tests. It is free to repurpose, and its name is exactly right.
- `PromptKind::CommandArgument` exists (`include/ssg/PromptSurface.h`) and is
  currently unused.
- The generic prompt round-trip is already wired end-to-end
  (`src/runtime/presentation.cpp` `prompt.submit`, lines 184–204): when a prompt
  carries a non-empty `PromptRequest::commandId`, submitting it **re-dispatches
  that command with `values.front()` (a `std::string`) as the payload**, via the
  deferred-dispatch drain. It rejects an empty value with a failure. No new
  submit/client code is needed.
- `cursor.set_position` (`SelectionCommand::CursorSetPosition`) moves the caret to
  a `DocumentPosition`, revealing it, focusing the editor, and recording User
  navigation — the full caret-placement contract, in one command
  (`src/runtime/editing.cpp` `bindSelection`; `src/Selection.cpp:846`). It
  **requires a fully valid `DocumentPosition`** (`isValidPosition`).
- `SelectionNavigator::resolvePosition(text, ByteOffset, tabWidth)` →
  `std::optional<DocumentPosition>` mints a valid position from a byte offset
  (`include/ssg/Selection.h:155`).

## Design

Repurpose `goto.line` as a self-contained library command. Pull it **out** of the
`jump()` helper (which stays serving `goto.file`/`goto.symbol` and their
`NavigationTarget` picker contract). Register `goto.line` with its own handler
that branches on its payload:

- **No payload** (keybinding / palette invocation): open a
  `PromptKind::CommandArgument` prompt with `commandId = "goto.line"`, a single
  input (id `"line"`, accessible label "Go to line"), then
  `reconcilePromptFocus()`. The existing `prompt.submit` path re-dispatches
  `goto.line` with the typed string — nothing else to write.

- **Non-empty `std::string` payload** (the prompt round-trip, or a direct
  programmatic line number):
  1. Require an active document; else `failure("goto.line requires an active document")`.
  2. Parse the string as a base-10 integer with `std::from_chars`. Reject
     non-numeric, trailing-garbage, or `< 1` input with
     `failure("goto.line expects a positive line number")`.
  3. **Single source of truth for line boundaries.** In ONE pass over the active
     text (`activeText()`), locate line starts by scanning for `'\n'` (line 0
     starts at offset 0; line `k+1` starts just past the `k`th newline). The line
     count is `newlines + 1` — the same definition `PieceTree::lineCount()` uses,
     but derived from the same scan that yields the start, so the count and the
     offset can never disagree. Do NOT read `PieceTree::lineCount()` separately;
     that would be a second source of truth for line boundaries.
  4. Convert 1-based → 0-based and clamp to `[0, lineCount - 1]`. (Out-of-range
     numbers clamp to the last line rather than failing — "go past the end" means
     "go to the end", the least-surprising behavior.) Take the clamped line's
     start offset from the scan in step 3.
  5. `resolvePosition(text, ByteOffset{start}, tabWidth)` → the valid caret
     `DocumentPosition` (column 1 / cell 0 of the line). A `nullopt` here is an
     internal invariant break → `failure`.
  6. **Defer** a `cursor.set_position` dispatch carrying
     `SelectionCommandArguments{position}` (`runtime.defer(std::nullopt,
     ClientCommand{"cursor.set_position", revision, arguments})`) and return
     success. The deferral drain runs it after the current handler, reusing the
     entire caret/reveal/focus/history machinery. This composes cleanly: the
     prompt already dispatched `goto.line` via deferral, and a deferred command
     may enqueue a further deferred command drained in the same loop (the
     established L3–L5 composition pattern).

Why defer `cursor.set_position` rather than move the caret inline: it is the ONE
place that owns "place the caret at a position and reveal/focus/record it."
Routing through it keeps that contract in a single owner and adds zero
duplication; re-entrant synchronous dispatch is refused by design, so deferral is
the correct seam.

### Keybinding

Add to `defaultTerminalKeymap` (`src/EditorRuntime.cpp`), editor context:

```
bind(seq({"Alt+Shift+KeyG"}), "goto.line", "editor");
```

### Scope boundaries

- **No app / client / presentation changes.** Prompt open, submit, re-dispatch,
  and caret placement all already exist. The change is confined to
  `src/runtime/navigation.cpp` (repurpose `goto.line`), one keymap line, and docs.
- `goto.file` / `goto.symbol` are untouched; their `NavigationTarget` contract and
  the `jump()` helper remain.
- `SearchCommandSet` keeps `{"goto.line", true}` (userNavigation) — jumping to a
  line is still user navigation; `cursor.set_position` records it.
- Column is always 1 (line start). Selecting a range or remembering a target
  column is out of scope.

## Testing

Library oracles (add to the runtime navigation / search test suite):

1. **No-payload opens the prompt**: dispatching `goto.line` with no payload opens
   a `CommandArgument` prompt whose `commandId == "goto.line"` and that has one
   input.
2. **Submit jumps and clamps**: with a known multi-line document, a submitted
   value of "3" places the primary caret at the start of line 3 (0-based line 2);
   a value far past the end clamps to the last line's start; "1" goes to offset 0.
3. **Bad input fails loudly**: non-numeric ("abc"), empty (rejected upstream by
   `prompt.submit`), and "0"/negative values return a failure and do not move the
   caret.
4. **Keymap resolves**: `Alt+Shift+KeyG` resolves to `goto.line` in the editor
   context (extends `curatedKeymapResolvesPerContext`).

The submit→jump oracle should drive the real deferral drain (dispatch
`prompt.submit` after opening, or dispatch `goto.line` with a string payload and
then drain) so the `cursor.set_position` composition is exercised end-to-end, not
mocked.

## Docs

- `doc/commands.md`: `goto.line` row already present; update its description/args
  to note it opens a line-number prompt.
- Mention the Alt+Shift+G binding wherever the default keymap is documented.
