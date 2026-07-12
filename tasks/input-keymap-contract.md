# input-keymap-contract

- Spec: `doc/features/browser-input.md`, Plans 1–2
- Depends: `required-command-catalog`, `prompt-status-surface`, `settings-model`, `core-websocket-slice`
- Branch: `input-keymap-contract-task`

## Scope

Define authoritative keymap data, semantic input command arguments, hit-target
metadata, IME committed-text contract, and reserved-chord validation without
running browsers.

## Files

`include/ssg/input.h`, `data/default-keymap.json`,
`tests/browser/fixtures/reserved-chords.json`, `tests/test_input.cpp`,
`cmake/components/input-keymap-contract.cmake`

## Oracle

Required-command coverage, duplicate/unreachable binding rejection, reserved
fixture checks, semantic hit-target round trips, and backend dependency scans.

## Done

The mandatory workflow is complete without browser event capture code.
