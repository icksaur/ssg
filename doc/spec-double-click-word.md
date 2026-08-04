# Spec: double-click selects the word

Status: draft
Backlog item 10: "double-click support ... Only use case I need is selecting
words by double-clicking them."

## Summary

A left double-click in the editor selects the word under the pointer. Scope is
deliberately narrow: word selection only. No triple-click (line select), no
double-click drag-to-extend-by-word, no double-click on chrome. Those are
out of scope and, if wanted later, are separate backlog items.

The work splits into two independently testable pieces:

1. **Library**: a new `select.word_at_position` command that, given a document
   position, sets a single ranged selection spanning the maximal run of same-
   category characters (word / whitespace / punctuation) containing that
   position. Pure, deterministic, oracle-tested against documents.
2. **Client**: a pure double-click classifier plus a small piece of state in the
   app loop that, on a left editor press it recognizes as a double-click,
   dispatches `select.word_at_position` instead of the usual
   `cursor.set_position`.

The terminal mouse protocol carries no click-count, so double-click detection is
timing-based and lives in the client. The word geometry lives in the library
because only it has the document model.

## Behavior

- A **single** left click in the editor keeps today's behavior: it dispatches
  `cursor.set_position` (a caret, empty selection) and arms a drag.
- A **second** left click within the double-click window AND on the same grid
  cell fires `select.word_at_position` at that click's resolved position,
  selecting the word (or run) there. The selection's anchor is the run start,
  its active end is the run end.
- What "the word" is: the maximal run of the category of the character at the
  clicked position, where category ∈ {Word, Space, Punctuation} using the
  existing classifier (`isWordByte`: ASCII alphanumerics, `_`, and every byte
  ≥ 0x80, so UTF-8 letters are word bytes; `' ' \t \n \r` are Space; everything
  else is Punctuation). Double-clicking:
  - inside a word selects the whole word (the common case),
  - on a punctuation glyph selects the run of like punctuation,
  - on whitespace selects the whitespace run.
- Clicking at or past the document end, or in an empty document, selects nothing
  (an empty selection at the clamped position); it must never fault.
- After a double-click fires, the click tracker resets, so a third quick click is
  a fresh single click (no triple-click behavior, by design).

## Design

### Library: `select.word_at_position`

- New `SelectionCommand::SelectWordAtPosition` enum value, registered as
  `"select.word_at_position"` in `SelectionNavigationCommandSet` (the descriptor
  array size grows from 36 to 37).
- Reuses the existing `SelectionCommandArguments{ position }` payload (no new
  wire type or codec): the client passes the clicked `DocumentPosition`.
- New `apply` case: validate the position against the model (same
  `isValidPosition` guard the other positional commands use; reject
  `InvalidPosition` otherwise), compute the run, and set `selections = { Selection{
  start, end } }` (single selection, replacing any existing multi-selection, like
  `select.set_range`).
- New `TextModel` method `wordRangeAt(position) -> {DocumentPosition start,
  DocumentPosition end}` computing the maximal same-category run:
  - If the document is empty (position == documentStart() == documentEnd()) OR
    `position == documentEnd()` (a click at or past the last character), return
    `{position, position}` (empty) BEFORE any category read — `documentEnd()` has
    no character to classify, so `categoryAt` must not be called there.
  - `category = categoryAt(position)`.
  - `start`: walk `previous()` from `position` while the previous cell's
    `categoryAt` equals `category`, stopping at `documentStart()`.
  - `end`: walk `next()` from `position` while `categoryAt(cursor) == category`,
    stopping at `documentEnd()`.
  - Return `{start, end}` (`start <= end` always, since both derive from
    `position` by monotone walks in opposite directions).
  - This is deliberately a fresh, uniform helper rather than reusing the cursor-
    motion `wordLeft`/`wordRight`, which special-case categories for caret
    movement (`wordLeft` does not extend across punctuation). `wordRangeAt` must
    treat all three categories uniformly so double-clicking punctuation or
    whitespace behaves predictably. `wordRight`'s existing right-scan is already
    exactly the `end` computation; `wordRangeAt` factors the mirror-image left
    scan so both directions share the category rule.
- Word runs never cross a line break because `\n` is Space category, so a Word
  run terminates at end-of-line naturally. A whitespace run MAY span a trailing
  newline into the next line's leading whitespace; that is acceptable (the user's
  case is words; whitespace-run selection is a consistent side effect, not a
  feature to police).

### Client: double-click detection

The terminal delivers only press/drag/release with no click-count
(`apps/ssg_terminal.h` `PointerEvent`), so the app tracks click timing itself.

- A pure classifier in `apps/pointer_routing.{h,cpp}`:
  ```
  struct ClickTracker {
      std::optional<std::chrono::steady_clock::time_point> time;
      int row = 0;
      int column = 0;
  };
  // Returns true iff this press completes a double-click (within `window` of the
  // previous tracked press AND on the SAME grid cell). ALWAYS updates the tracker
  // to reflect this press. On a returned true, the tracker is reset so a
  // subsequent press starts fresh (no triple-click).
  bool register_click_is_double(ClickTracker& tracker,
                                std::chrono::steady_clock::time_point now,
                                int row, int column,
                                std::chrono::milliseconds window) noexcept;
  ```
  The pure function takes `now` as a parameter so tests inject timestamps; no
  clock is read inside it.
- Window: a fixed `kDoubleClickWindow = std::chrono::milliseconds{400}` constant
  in the app. (Not configurable now; a `dim_*`-style config knob can be a later
  backlog item — deferred, not blocking.)
- Same-position rule: equality of the **pointer grid cell** (row, column), not
  the resolved byte. A double-click is a stationary gesture; in a cell grid the
  cell is the finest pointer granularity, so "same cell" is the terminal-native
  equivalent of a GUI pixel-distance threshold. Comparing cells (rather than
  byteOffset) means clicking either cell of a double-width glyph, or any cell of
  the word, still pairs correctly, and — crucially — the library then selects the
  WHOLE word from that one cell, so landing anywhere in a word selects it. Two
  clicks on different cells are a moved pointer, not a double-click, and remain
  two singles. This keeps the classifier free of any document/word geometry
  (which the client does not have) while still selecting the whole word.
- Wiring in `apps/ssg_main.cpp`: on a **left press** whose hit region is the
  editor, after resolving `targets.document_position` (already computed today for
  `cursor.set_position`), call `register_click_is_double` with the pointer's
  `(row, column)`. If it returns true, dispatch
  `{"select.word_at_position", SelectionCommandArguments{position}}` at the
  resolved position and do NOT arm a drag or dispatch `cursor.set_position` for
  this press. Otherwise fall through to the existing single-click path unchanged.
  - Any non-left press, or a left press that resolves to no editor position,
    leaves the double-click path untaken (the tracker is only updated on left
    editor presses, so an intervening right-click or chrome click does not
    complete a double-click).

### What is NOT changed

- `route_pointer`'s existing single-click and drag behavior is untouched; the
  double-click branch is decided in the app loop before/around it, mirroring how
  the gutter-grab press is special-cased today.
- No change to the wire protocol, `PointerEvent`, or the drag machinery.

## Invariants

- **INV-single-click-unchanged**: a lone left click still dispatches
  `cursor.set_position` and arms a drag; the double-click path is entered only
  on the second qualifying press.
- **INV-word-range-valid**: `wordRangeAt` returns positions within
  `[documentStart, documentEnd]` with `start <= end`, for every position in
  every document, and never faults (empty document, position at end, line
  boundaries, multi-byte runs).
- **INV-word-line-bounded**: a Word-category run never crosses a `\n` (newline is
  Space), so double-clicking a word selects within one line.
- **INV-detector-pure**: `register_click_is_double` reads no clock and has no
  side effect beyond mutating the passed `ClickTracker`; identical inputs give
  identical outputs.
- **INV-word-selects-whole-word-from-any-cell**: dispatching
  `select.word_at_position` at any position inside a word selects that entire
  word, so the same-cell gesture rule never selects a partial word.

## Oracles

### Library (`tests/test_selection.cpp`)

Drive `select.word_at_position` through the real command apply and assert the
resulting single selection's `[anchor, active]` byte range:

- Click mid-word → selects the whole word; click on the word's first byte and on
  its last byte both select the same full word.
- Adjacent words separated by a space: clicking either selects only that word;
  clicking the space selects only the space run.
- Punctuation run (e.g. `"---"`, `"()"`): clicking selects the punctuation run,
  not the neighboring words.
- A multi-byte UTF-8 word (e.g. `"héllo"` / a CJK run) selects the whole word by
  byte range (word bytes ≥ 0x80).
- Word at start-of-document and at end-of-line: run stops at the boundary; the
  word does not cross the newline.
- Click at document end / empty document → empty selection at the clamped
  position, no fault.
- Invalid position (not matching the layout) → command rejects with
  `InvalidPosition`, selection unchanged.

### Client (`tests/test_ssg_app.cpp`)

Pure `register_click_is_double` with injected timestamps and cells:

- Two presses on the same cell within the window → second returns true.
- Two presses on the same cell but OUTSIDE the window → both false (two
  singles); tracker holds the latest.
- Two presses within the window but on DIFFERENT cells → both false.
- Three rapid presses on the same cell → true on the second, then false on
  the third (reset after firing; no triple-click).
- A first press always returns false (nothing to pair with).

An app-loop / route oracle (mirroring the existing route-pointer tests) is
REQUIRED, not optional: it is the only place that proves a recognized
double-click yields a `select.word_at_position` command at the clicked position
(and NOT `cursor.set_position`, and with no drag armed). The pure classifier
tests cannot catch a mis-wire at this seam.

## Plan

| # | Step | Files | Oracle |
|---|------|-------|--------|
| 1 | `TextModel::wordRangeAt(position)` computing the maximal same-category run (uniform for all three categories); factor the shared category scan with `wordRight` | `src/Selection.cpp` | direct range oracles: word / space / punctuation / UTF-8 / boundaries / empty |
| 2 | `SelectionCommand::SelectWordAtPosition` + `"select.word_at_position"` registration (array 36→37) + `apply` case setting a single ranged selection; reuse `SelectionCommandArguments{position}` | `src/Selection.cpp`, `include/ssg/Selection.h` | command-level oracles in `tests/test_selection.cpp`; rejects invalid position |
| 3 | Pure `register_click_is_double` + `ClickTracker` | `apps/pointer_routing.{h,cpp}`, `tests/test_ssg_app.cpp` | injected-timestamp oracles (window, same/diff position, triple-click reset, first press) |
| 4 | App-loop wiring: track left editor presses by cell; on a double-click dispatch `select.word_at_position` (position) instead of `cursor.set_position`, no drag armed | `apps/ssg_main.cpp` | REQUIRED route/app oracle: double-click emits `select.word_at_position`, single click still emits `cursor.set_position` and arms a drag |
| 5 | Register the new command in every command surface and regenerate its pinned artifacts: command metadata / `commands.md` and any command-catalog/codec parity fixture. `SelectionCommandArguments` already has a wire codec, so no NEW codec — but the command-id/metadata surfaces must be updated and their tests regenerated. Run the catalog/metadata tests and regenerate fixtures unconditionally | `src/`, `tests/` fixtures, `doc/commands.md` | command-catalog / metadata / `commands.md` tests green after regeneration |

## Considerations

- **Why server-side word geometry**: the client has no document model; only the
  library knows byte categories and line boundaries. Detecting the *gesture* is a
  client concern (timing); computing the *word* is a library concern. This split
  keeps each half pure and testable.
- **Window value**: 400 ms is a common default. Making it configurable is
  deferred; note it in the backlog if desired.
- **Same-position rule uses the grid cell**, the pointer's native granularity: a
  stationary double-click lands on one cell, and any cell of a word selects the
  whole word, so cell-equality both matches the gesture and selects the full
  word without the client needing document/word geometry.
- **No triple-click**: resetting the tracker after a double-click keeps the
  feature to exactly the requested scope.
