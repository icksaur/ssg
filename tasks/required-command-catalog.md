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

## Files

`data/required-commands.json`, `tests/test_required_commands.cpp`,
`cmake/components/required-command-catalog.cmake`

## Oracle

An independently maintained expected category/count fixture and explicit owner
mapping must fail when any normative command is omitted or duplicated.

## Done

The mandatory workflow is complete; no command implementation is added.
