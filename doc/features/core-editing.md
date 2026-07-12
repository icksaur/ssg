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
- Missing ordinary commands: author the normative list above before `data/required-commands.json` and require exact transcription.

## Acceptance (Definition of Done)

- Observable: all listed commands work through the in-process API, and read-only/diff tabs never mutate.
- Budgets: document edit budgets from `doc/spec.md`.
- Gates: project build, unit tests, and sanitizers are green.
- Oracles: hand cases establish the reference editor; randomized command scripts compare snapshots; clipboard stale-response and denied-system/fallback cases prove atomicity; required-command fixture equals the normative list.

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Establish the reference editor and required-command fixture | `tests/reference_editor.*`, `tests/test_reference_editor.cpp`, `data/required-commands.json` | hand-computed command scripts; exact normative-list comparison | I13 |
| 2 | Implement piece-tree transactions and document modes | `include/ssg/document.h`, `src/document.cpp`, `tests/test_document.cpp`, `tests/test_piece_tree.cpp` | reference snapshots; read-only failures | I3, I5 |
| 3 | Implement movement, caret reveal, selections, multi-cursor creation, typing, indentation, lines, comments, brackets, and transforms | `include/ssg/selection.h`, `src/selection.cpp`, `tests/test_selection.cpp` | reference command scripts and viewport intersection properties | I5, I13, I23 |
| 4 | Implement clipboard request/response transforms | `include/ssg/clipboard.h`, `src/clipboard.cpp`, `tests/test_clipboard.cpp` | hand cases and stale/denied fault tests | I16, I18 |
| 5 | Implement coalesced bounded per-file undo/redo with selection restoration | `include/ssg/history.h`, `src/history.cpp`, `tests/test_history.cpp` | timed coalescing and forward/undo/redo round trips | I5 |

## Rationale (optional, skippable)

These commands share one mutation and selection algebra; separate specs would duplicate the same oracle and boundary.
