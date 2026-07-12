# browser-client

- Spec: `doc/spec.md` Observable; `doc/features/browser-input.md`
- Depends: `editor-session-assembly`, `websocket-server`, `browser-input-conformance`
- Branch: `browser-client-task`

## Scope

Implement a minimal browser fixture that renders API view models and sends
semantic commands over one WebSocket, including prompts, status, clipboard,
mouse, keyboard, and accessibility labels. A separate local-capability fixture
demonstrates file drop; the ordinary remote fixture does not expose it.

## Files

`examples/browser/`, `tests/browser/client/`,
`cmake/components/browser-client.cmake`

## Oracle

Chromium/Firefox/WebKit scripted workflows, accessibility snapshots, and
canonical session-state parity with direct API scripts.

## Done

The mandatory workflow is complete with no client-side editor semantics.
