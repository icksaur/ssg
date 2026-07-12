# input-keymap-contract

- Spec: `doc/features/browser-input.md`, Plans 1–2
- Depends: `required-command-catalog`, `prompt-status-surface`, `settings-model`, `core-websocket-slice`
- Branch: `input-keymap-contract-task`

## Scope

Define authoritative keymap data, semantic input command arguments, hit-target
metadata, IME committed-text contract, and reserved-chord validation without
running browsers.

## Files

`include/ssg/input.h`, `src/input.cpp`, `data/default-keymap.json`,
`tests/browser/fixtures/reserved-chords.json`, `tests/test_input.cpp`,
`cmake/components/input-keymap-contract.cmake`

## Oracle

Exact coverage of the 159 required commands marked `keymap:true`, explicit
exclusion of `file.open_dropped_content`, duplicate/unreachable binding
rejection, reserved fixture checks, committed UTF-8 and semantic hit-target
round trips, and backend dependency scans. Semantic input data reuses the
existing text-input, selection, and viewport argument types.

## Done

The mandatory workflow is complete without browser event capture code.
