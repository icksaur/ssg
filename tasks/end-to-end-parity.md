# end-to-end-parity

- Spec: `doc/spec.md` Acceptance
- Depends: `tui-client`, `browser-client`
- Branch: `end-to-end-parity-task`

## Scope

Assemble full Linux/Windows scenarios covering files, recovery, editing,
settings, search, services, watcher/diff/follow, and cross-client interruption.

## Files

`tests/fixtures/end_to_end/`, `tests/test_end_to_end.cpp`,
`cmake/components/end-to-end-parity.cmake`

## Oracle

TUI, browser/WebSocket, and direct API runs reach identical canonical semantic
state after every scripted command; geometry differs only by viewport.

## Done

The mandatory workflow is complete on both required platforms and all P0
observable requirements are represented.
