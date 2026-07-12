# end-to-end-parity

- Spec: `doc/spec.md` Acceptance
- Depends: `tui-client`, `browser-client`
- Branch: `end-to-end-parity-task`

## Scope

Assemble full Linux/Windows scenarios covering files, recovery, editing,
settings, search, services, watcher/diff/follow, and cross-client interruption.
The direct API, loopback WebSocket, TUI, and browser paths consume one reviewed,
client-neutral mandatory workflow. A concurrent scenario attaches TUI and
browser/WebSocket clients to one session, applies external edits to multiple
watched files, verifies viewport-independent shared follow state, pauses both
clients through manual navigation from either client, and resumes both.

## Files

`tests/fixtures/end_to_end/`, the shared end-to-end fixture support used by both
client fixtures, `tests/test_end_to_end.cpp`, affected TUI/browser fixture
manifests, and `cmake/components/end-to-end-parity.cmake`

## Oracle

One client-neutral canonical projection of assembled `EditorSession` snapshots
is the source of truth. Direct API, loopback WebSocket, TUI, and browser runs
consume the same workflow fixture and reach identical canonical semantic state
after every scripted command; geometry differs only by viewport. The
multi-client script additionally proves shared follow/pause/resume transitions
and independent viewport scroll offsets while both clients remain attached.

## Done

The mandatory and concurrent workflows are complete on Linux and represented
in the Windows source/cross-compile gate; native Windows CI is authoritative.
Gate 12 directly proves cross-client state parity, one-WebSocket browser
operation, and shared follow interruption. Dependency gates remain authoritative
for browser accessibility and local-drop policy, shell geometry, Lua/catalog
coverage, and exclusive 16-color output.
