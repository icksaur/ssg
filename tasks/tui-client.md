# tui-client

- Spec: `doc/spec.md` Observable; `doc/features/presentation-shell.md`
- Depends: `editor-session-assembly`, `input-keymap-contract`,
  `required-command-catalog`
- Branch: `tui-client-task`

## Scope

Implement a minimal in-process TUI fixture that captures terminal input,
renders cell/view models with 16 colors, and exercises every required workflow
without editor behavior in the client.

The fixture is an in-process consumer only; it does not use the WebSocket
server. Terminal adapters produce committed text, key strokes, and semantic hit
targets. The fixture resolves those events from the snapshot keymap and submits
typed semantic commands to `EditorSession`; it never mutates editor state
itself.

`EditorSession` does not expose snapshots and the assembled P0 registry requires
the embedder to bind every command. The test fixture therefore supplies a
complete handler model and assembles `SessionSnapshot` values explicitly. This
model is test scaffolding, not client behavior. Both the direct and TUI paths
dispatch through the same fixture model and compare its canonical state after
each command.

The mandatory workflow command script is a reviewed, client-neutral fixture
that downstream browser/TUI parity tests can consume rather than duplicating
the scenario in each client.

## Files

`examples/tui/`, `tests/test_tui_fixture.cpp`,
`tests/fixtures/tui/`, `cmake/components/tui-client.cmake`

## Oracle

The reviewed semantic command script produces hand-authored grid snapshots and
the same canonical fixture-model state after every command through the TUI and
direct in-process dispatch paths. Snapshots prove that rendered cells use only
indices from the authoritative 16-entry `ThemeSnapshot` palette.

## Done

The platform-neutral terminal event/grid fixture completes under CTest on Linux
and Windows. Native terminal event-loop wiring and CI runner configuration
remain thin adapters owned by the later integration/CI tasks.
