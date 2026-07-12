# tui-client

- Spec: `doc/spec.md` Observable; `doc/features/presentation-shell.md`
- Depends: `editor-session-assembly`, `websocket-server`, `browser-input-conformance`
- Branch: `tui-client-task`

## Scope

Implement a minimal in-process TUI fixture that captures terminal input,
renders cell/view models with 16 colors, and exercises every required workflow
without editor behavior in the client.

## Files

`examples/tui/`, `tests/test_tui_fixture.cpp`,
`cmake/components/tui-client.cmake`

## Oracle

Scripted semantic commands produce accepted screen snapshots and the same
canonical session state as direct in-process API scripts.

## Done

The mandatory workflow is complete on Linux and Windows terminals used by CI.
