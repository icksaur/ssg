# required-command-catalog

- Spec: all P0 feature specs; `doc/features/browser-input.md`, Plan 1
- Depends: `foundation-harness`
- Branch: `required-command-catalog-task`

## Scope

Transcribe the exact union of normative P0 command IDs into the accepted
catalog with an explicit task owner for every ID and add a test that detects
missing, duplicate, fuzzy, or unowned IDs. `diff.*` belongs to `diff-model`;
matching-bracket/occurrence selection belongs to `selection-navigation`;
encoding/EOL commands belong to `encoding-eol`; ingress-only
`file.open_dropped_content` belongs to `file-commands` and is explicitly marked
Lua/keybinding/palette excluded.

The authoritative task-owner routing is:

- all `text.*` IDs belong to `text-input-commands`;
- all `cursor.*` and `select.*` IDs, `goto.matching_bracket`,
  `view.reveal_caret`, and `view.center_caret` belong to
  `selection-navigation`;
- `edit.undo` and `edit.redo` belong to `undo-redo-history`; every other
  `edit.*` ID belongs to `edit-command-suite`;
- all `clipboard.*` IDs belong to `clipboard-register`;
- `view.toggle_word_wrap` and every `view.scroll_*` ID belong to
  `viewport-wrap-scrollbar`;
- every `pane.*` and `panel.*` ID and `view.toggle_distraction_free` belong to
  `shell-layout`;
- all `prompt.*` and `status.*` IDs belong to `prompt-status-surface`;
- all `palette.*` and `search.*` IDs and `goto.file`, `goto.line`,
  `goto.symbol`, `goto.back`, and `goto.forward` belong to `search-palette`;
- all `find.*` and `replace.*` IDs belong to `find-replace`;
- all `completion.*` and `hover.*` IDs and `goto.definition` and
  `goto.reference` belong to `lsp-language-features`;
- `workspace.open_directory` and every `file.*` ID except the four
  encoding/EOL IDs belong to `file-commands`;
- `file.reopen_with_encoding`, `file.set_encoding`, `file.set_line_ending`,
  and `file.set_final_newline` belong to `encoding-eol`;
- all `tab.*`, `external.*`, `settings.*`, `follow_edits.*`, and `diff.*` IDs
  belong respectively to `tab-management`, `external-modification-flow`,
  `settings-model`, `follow-edits`, and `diff-model`.

`reference-editor` is an oracle and never a command owner. Cursor and selection
page movement belongs to `selection-navigation`. The catalog records required
capabilities and Lua/keymap/palette availability for every ID.
`file.open_dropped_content` is the only capability-gated ID: it requires
`local_file_drop` and is the only ID excluded from Lua, keymaps, and the
palette. Every other required ID has no required capability and is available
through all three surfaces.

## Files

`data/required-commands.json`, `tests/test_required_commands.cpp`,
`cmake/components/required-command-catalog.cmake`

## Oracle

An independently maintained expected category/count fixture and explicit owner
mapping must fail when any normative command is omitted, added, duplicated,
fuzzily matched, or unowned. It also asserts the exact capability and
Lua/keymap/palette exclusion sets.

## Done

The mandatory workflow is complete; no command implementation is added.
