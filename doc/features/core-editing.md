# spec-core-editing

## Goals

Provide the browser-feasible headless editing core: UTF-8 typing, deletion, movement, selection, multi-cursor edits, edit/read-only/diff modes, clipboard transforms, dirty state, and bounded per-file undo/redo.

## Design

`Document` uses the private balanced piece-tree mechanism defined in `doc/spec.md`. All mutations are atomic transactions in pre-edit byte coordinates. `SelectionSet` is ordered and normalized; typing replaces selections or inserts at carets. Clipboard operations use revision-tagged client request/response values.

The normative required command IDs owned by this feature are:

- `text.insert`, `text.newline`, `text.delete_backward`, `text.delete_forward`, `text.delete_word_backward`, `text.delete_word_forward`
- `cursor.set_position`, `cursor.left`, `cursor.right`, `cursor.word_left`, `cursor.word_right`, `cursor.line_up`, `cursor.line_down`, `cursor.line_start`, `cursor.line_end`, `cursor.page_up`, `cursor.page_down`, `cursor.document_start`, `cursor.document_end`
- `select.set_range`, `select.add_range`, `select.left`, `select.right`, `select.word_left`, `select.word_right`, `select.line_up`, `select.line_down`, `select.line_start`, `select.line_end`, `select.page_up`, `select.page_down`, `select.document_start`, `select.document_end`, `select.all`, `select.add_next_occurrence`, `select.add_cursor_up`, `select.add_cursor_down`, `select.split_into_lines`, `select.to_matching_bracket`
- `edit.undo`, `edit.redo`, `edit.indent`, `edit.outdent`, `edit.duplicate_line`, `edit.move_line_up`, `edit.move_line_down`, `edit.delete_line`, `edit.join_lines`, `edit.uppercase`, `edit.lowercase`, `edit.swap_case`, `edit.sort_lines`, `edit.transpose`, `edit.toggle_comment`
- `clipboard.copy`, `clipboard.cut`, `clipboard.paste`
- `view.toggle_word_wrap`, `view.scroll_lines`, `view.scroll_pages`, `view.scroll_to_fraction`, `view.reveal_caret`, `view.center_caret`

Indentation policy, tab width, tabs-versus-spaces, automatic indentation, comment tokens, and bracket pairs are resolved typed settings consumed by commands; they are not client behavior. Contiguous typing and same-direction deletions coalesce under the project history rules, and undo/redo restore selections. Clipboard operations use the internal structured register and browser capability flow defined in `doc/spec.md`.

## Invariants

I3, I4, I5, I13, I16, I18, I19 from `doc/spec.md`.

## Considerations

- Invalid UTF-8 and NUL-containing files follow the read-only rules in `doc/spec.md`.
- Read-only and diff modes reject every mutating command atomically.
- Clipboard cut always remains usable through the internal register; system export is best effort.
- Undo history is per document and does not survive restart.

## Risks and Mitigations

- Piece-tree and multi-cursor bugs: compare randomized scripts with the independent reference editor.
- Missing ordinary commands: author the normative list above; `required-command-catalog` independently transcribes it into `data/required-commands.json` and verifies exactness.

## Acceptance (Definition of Done)

- Observable: all listed commands work through the in-process API, and read-only/diff tabs never mutate.
- Budgets: document edit budgets from `doc/spec.md`.
- Gates: project build, unit tests, and sanitizers are green.
- Oracles: hand cases establish the reference editor; randomized command scripts compare snapshots; clipboard stale-response and denied-system/fallback cases prove atomicity.

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Establish the reference editor | `tests/reference_editor.h`, `tests/reference_editor.cpp`, `tests/test_reference_editor.cpp`, `cmake/components/reference-editor.cmake` | hand-computed mutation, selection, clipboard-transform, and history command scripts covering the primitives listed below | I13 |
| 2 | Implement the private balanced piece-tree storage | `src/piece_tree.h`, `src/piece_tree.cpp`, `tests/test_piece_tree.cpp`, `cmake/components/piece-tree.cmake` | randomized insert, erase, read, and line queries against a direct `std::string` model, plus structural invariants | I4 |
| 3 | Implement document transactions and document modes over private storage | `include/ssg/document.h`, `src/document.cpp`, `tests/test_document.cpp` | reference-editor snapshots; read-only failures | I3, I5, I13 |

**Document transaction contract (Plan 3 scope).** `Document` is a move-only
owning value whose mode is fixed at construction. It accepts only well-formed
UTF-8 text without NUL; file decoding and binary/read-only fallback happen
before construction in the encoding and file-lifecycle layers. A transaction
carries its observed document revision and one or more replacements expressed
as pre-transaction byte offset, erased byte length, and inserted UTF-8 text.
Ranges and both endpoints must be valid UTF-8 boundaries. Ranges must be
distinct and non-overlapping; accepted edits are applied from highest to lowest
offset.

The typed transaction result distinguishes stale revision, read-only mode,
diff mode, empty transaction, invalid range, overlap, invalid UTF-8, UTF-8
boundary split, and revision exhaustion. Rejection changes neither text,
revision, nor dirty state. Every accepted transaction advances the document
revision exactly once and marks the document dirty. Documents begin at
revision 1 and clean. Dirty clearing is deferred to save/file-lifecycle
integration. Undo/redo storage and coalescing remain Plan 6; Plan 3 supplies
one atomic revision boundary per accepted transaction.

The canonical owning `DocumentSnapshot` contains text, revision, mode, and
dirty state. Reference-editor comparison maps each replacement to an
independent selection replacement and applies replacements highest-offset
first; reference snapshots validate text while hand-authored properties
validate revision, mode, dirty state, snapshot ownership, and every rejection.
The component manifest is
`cmake/components/document-transactions.cmake`.

**Reference editor primitives (Plan 1 scope).** The reference editor is a minimal `std::string`-based implementation that covers: text mutations (`text.insert`, `text.newline`, `text.delete_backward`, `text.delete_forward`, `text.delete_word_backward`, `text.delete_word_forward`); cursor movement (`cursor.set_position`, `cursor.left`, `cursor.right`, `cursor.word_left`, `cursor.word_right`, `cursor.line_up`, `cursor.line_down`, `cursor.line_start`, `cursor.line_end`, `cursor.document_start`, `cursor.document_end`); selection (`select.set_range`, `select.add_range`, `select.left`, `select.right`, `select.word_left`, `select.word_right`, `select.line_up`, `select.line_down`, `select.line_start`, `select.line_end`, `select.document_start`, `select.document_end`, `select.all`, `select.add_next_occurrence`, `select.add_cursor_up`, `select.add_cursor_down`, `select.split_into_lines`); clipboard (`clipboard.copy`, `clipboard.cut`, `clipboard.paste`); history (`edit.undo`, `edit.redo`); and edit transforms (`edit.indent`, `edit.outdent`, `edit.duplicate_line`, `edit.move_line_up`, `edit.move_line_down`, `edit.delete_line`, `edit.join_lines`, `edit.uppercase`, `edit.lowercase`, `edit.swap_case`, `edit.sort_lines`, `edit.transpose`, `edit.toggle_comment`). Page and scroll commands and `select.to_matching_bracket` are deferred to later tasks that own viewport and bracket state. The reference editor may use `include/ssg/types.h` and `include/ssg/config.h` (foundation domain value types, not editing code) but must reimplement all mutation, selection, and history algorithms independently; using any SSG editing, selection, or history implementation would defeat the oracle purpose of I13.
The piece tree stores byte and newline summaries in this task. Display metadata
depends on the separately scheduled Unicode cell-layout work and is added by
the viewport/layout layer rather than computed by storage. The private node
summary is deliberately centralized so later cached metadata can be attached
without changing the piece split/join representation. A direct `std::string`
model is the strongest independent oracle for the storage-only operations;
the `reference-editor` dependency establishes the independent editor oracle
that the following document-transaction task uses against this storage.

| 4 | Implement movement, caret reveal, selections, multi-cursor creation, typing, indentation, lines, comments, brackets, and transforms | `include/ssg/selection.h`, `src/selection.cpp`, `tests/test_selection.cpp` | reference command scripts, hand-computed page/bracket cases, and minimal viewport-intersection properties | I5, I13, I23 |
| 5 | Implement clipboard request/response transforms | `include/ssg/clipboard.h`, `src/clipboard.cpp`, `tests/test_clipboard.cpp` | hand cases and stale/denied fault tests | I16, I18 |
| 6 | Implement coalesced bounded per-file undo/redo with selection restoration | `include/ssg/history.h`, `src/history.cpp`, `tests/test_history.cpp` | timed coalescing and forward/undo/redo round trips | I5 |

**History contract (Plan 6 scope).** One `DocumentHistory` instance owns the
non-persistent history for one document. Callers submit an edit through history
with the pre- and post-edit selections, edit kind, and an explicitly injected
monotonic session-clock timestamp in milliseconds. History captures erased text
from the pre-edit document snapshot before forwarding the transaction to
`Document::apply`; rejected edits change neither the document nor history.
`HistoryConfig` is the construction-time snapshot of the resolved history
settings. Runtime settings integration may replace that snapshot in a later
assembly task without changing history semantics.

Undo and redo replay stored inverse or forward edits through
`Document::apply`; they never restore an old revision. Every replayed
transaction therefore advances the monotonic document revision and leaves the
document dirty, including when its bytes again equal the opening content.
Undo restores the selection before the undo unit and redo restores the
selection after it. A new accepted edit after undo clears redo. A revision
change that bypasses the owning history makes undo/redo fail atomically as
stale.

Typing, backward deletion, and forward deletion are distinct coalescing kinds.
Units coalesce only when the kind and ordered caret count match, the prior
post-edit selections exactly equal the next pre-edit selections, every
selection at the join is a caret, each edit has the declared insertion or
same-direction deletion shape adjacent to its caret, no explicit coalescing
barrier occurred, timestamps are nondecreasing, and their difference is at
most `coalesce_ms`. Newline, paste, transforms, replacement edits, navigation,
selection changes, and other non-edit commands are barriers. Multi-caret
coalescing applies these rules pairwise in normalized selection order.

The byte budget counts retained inserted and erased UTF-8 payload bytes plus
the stored pre- and post-selection objects. Fixed container metadata is not
charged. History evicts oldest undo units until within budget; a unit larger
than the entire budget is not retained, and a zero budget disables retention.
Moving a unit between undo and redo does not change its charge. Deterministic
coalescing tests inject timestamps directly. Reference-editor round trips use
non-coalescing edit kinds or timestamps beyond the window so one SSG undo unit
corresponds to one reference-editor snapshot.

**Selection/navigation contract (Plan 4 scope).** The
`selection-navigation` task owns normalized selection state, the `cursor.*` and
`select.*` command families, `goto.matching_bracket`, `view.reveal_caret`, and
`view.center_caret`; text mutation and transform commands in this plan row are
implemented by their separately scheduled tasks. Bracket pairs are resolved
settings injected into selection operations. Vertical and page movement use
display cells and preserve a desired cell across shorter lines. Every accepted
selection transition minimally reveals the primary active endpoint; explicit
centering clamps to the viewport's scroll bounds. Reference-editor scripts use
ASCII fixtures, while hand-computed page, nested-bracket, tab, combining, and
wide-grapheme cases cover behavior outside that byte-column oracle.

**Text-input command contract.** The separately scheduled
`text-input-commands` task owns exactly `text.insert`, `text.newline`,
`text.delete_backward`, `text.delete_forward`, `text.delete_word_backward`,
and `text.delete_word_forward`. It exports an immutable feature descriptor set,
following `SelectionNavigationCommandSet`, plus a pure apply function. The
function receives a `DocumentSnapshot`, normalized `SelectionSet`, resolved
indentation/line-ending settings, a command, and the insertion payload. It
returns either an actionable typed error or a pre-transaction
`EditTransaction`, the post-edit `SelectionSet`, and the resulting text needed
to resolve positions. Session assembly later applies the transaction to its
live `Document`; this feature does not register stateful
`command_registry::CommandSet` handlers.

All edits use pre-transaction offsets. Selected ranges are replaced before
caret-only behavior is considered. Coincident carets and overlapping selected
ranges produce one normalized edit, so insertion is never multiplied at one
byte offset and `Document` never receives duplicate or overlapping edits.
Adjacent ranges may be merged. If every requested deletion is already at a
document boundary, the command succeeds as an explicit no-op without a
transaction or revision change. Every non-empty accepted edit yields exactly
one transaction.

Backward and forward character deletion remove one extended grapheme cluster;
CRLF is one logical terminator. Word deletion groups ASCII letters, digits,
underscore, all non-ASCII graphemes, whitespace, and punctuation using the
same category rules as selection navigation. The independent reference-editor
comparison is restricted to ASCII/single-codepoint input with LF and automatic
indentation disabled. Hand-authored fixtures are authoritative for grapheme
clusters, configurable terminators, and indentation.

`text.newline` inserts LF, CRLF, or CR from the resolved line-ending setting.
For `mixed`, it copies the current line's existing terminator when present and
falls back to LF on the final unterminated line. With automatic indentation
enabled it copies the current line's leading whitespace display width and
re-materializes it using the resolved tabs-versus-spaces style and width.
There is no bracket-aware indentation increase in this task. Post-edit
positions are resolved against the resulting text with the resolved tab width.
Invalid selection coordinates, invalid inserted UTF-8, and non-edit document
modes fail atomically.

**Edit-command-suite contract.** The separately scheduled
`edit-command-suite` task owns exactly `edit.indent`, `edit.outdent`,
`edit.duplicate_line`, `edit.move_line_up`, `edit.move_line_down`,
`edit.delete_line`, `edit.join_lines`, `edit.uppercase`, `edit.lowercase`,
`edit.swap_case`, `edit.sort_lines`, `edit.transpose`, and
`edit.toggle_comment`. It exports one immutable
`EditCommandSuiteCommandSet` containing those descriptors and one pure apply
function. The function receives a `DocumentSnapshot`, normalized
`SelectionSet`, resolved `EditCommandSettings`, and a command. It returns
either an actionable typed error or a pre-transaction `EditTransaction`, the
post-edit `SelectionSet`, and resulting text. It does not register stateful
`command_registry::CommandSet` handlers or edit session, aggregate snapshot,
or protocol files.

`EditCommandSettings` contains the indentation style and width, display tab
width, preferred line ending, and line-comment token. Both widths are in
`[1, 16]`; selection coordinates are validated and rematerialized with the
display tab width rather than assuming it equals the indentation width.
`mixed` uses LF when a new separator is required when it is the preferred
line-ending value. The comment token must be non-empty, valid UTF-8, and
contain no line terminator. Commands reject invalid settings or selection
positions and reject read-only and diff documents atomically.

A caret touches its containing logical line. A non-empty half-open selection
touches every intersected line, except that its upper endpoint at a line start
does not add that line. Duplicate and overlapping selections collapse to one
ordered set of lines, partitioned into contiguous runs, so each line is
transformed once and untouched gaps remain unchanged. LF, CRLF, and CR are
logical terminators. Existing terminator bytes are preserved unless a command
necessarily removes or moves them; when duplication of a final unterminated
line needs a separator, the preferred line ending is used. Endpoints outside
an edit shift by the cumulative byte delta of all edits at lower offsets;
endpoints inside replaced text retain their relative byte displacement clamped
to the replacement. Insertions at an endpoint move that endpoint after the
inserted bytes. The result is normalized through `SelectionSet`.

Indent prefixes one configured indentation unit at each touched line.
Outdent removes one leading tab or up to `indent_width` leading spaces,
regardless of the configured insertion style. Duplicate copies each contiguous
touched run immediately after itself. Move-line commands move each touched run
across the adjacent line while preserving line contents and terminator style;
a run at the requested document boundary is unchanged. Delete removes complete
touched lines; deleting a final unterminated line also consumes its preceding
terminator. Join replaces each distinct touched line's terminator with one
ASCII space and leaves a final unterminated line unchanged.

Uppercase, lowercase, and swap-case transform only non-empty selections using
ASCII case mapping; non-ASCII bytes are preserved. Sort orders each contiguous
run of at least two touched lines independently, ascending by unsigned UTF-8
content bytes, stably for equal lines. Terminators remain in their original
row slots, including a final unterminated slot. A lone caret therefore does
not sort. Transpose applies only to carets and swaps the extended grapheme
cluster before the caret with the one after it; at document end it swaps the
final two clusters. It is a no-op at document start, with fewer than two
available clusters, or for non-empty selections. Cluster boundaries are the
Unicode 15.0.0 UAX #29 boundaries produced by `compute_cell_run`, shared with
text-input deletion. At document end, a trailing line terminator is skipped so
the final two content clusters are transposed while the terminator is retained.

Toggle-comment operates at the first non-horizontal-whitespace byte of each
touched line. Blank lines are unchanged. It removes the token iff every
non-blank touched line already has the token there; otherwise it inserts the
token at every non-blank touched line. Existing indentation is preserved.

Every accepted byte-changing transform emits exactly one transaction. An already
satisfied sort, a boundary move/join/transpose, an outdent with no removable
indentation, a case transform with no changed byte, and any other transform
whose resulting text is unchanged succeed as explicit no-ops without a
transaction or revision change. Hand fixtures cover every command with single
and multiple selections, LF/CRLF/CR and final-unterminated input, tabs and
spaces, invalid settings/selections, non-edit modes, and no-op atomicity.

## Rationale (optional, skippable)

These commands share one mutation and selection algebra; separate specs would duplicate the same oracle and boundary.
