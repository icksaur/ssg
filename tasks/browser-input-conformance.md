# browser-input-conformance

- Spec: `doc/features/browser-input.md`, Plan 3
- Depends: `input-keymap-contract`
- Branch: `browser-input-conformance-task`

## Scope

Build the Chromium/Firefox/WebKit conformance harness for keyboard, IME, mouse,
wheel, scrollbar, and clipboard gesture/permission translation. Add file-drop
translation only to the host-authorized test-only local-client fixture. This
task does not create or modify the product browser client.

## Files

`tests/browser/input/`, `cmake/components/browser-input-conformance.cmake`

## Oracle

Real-browser captured events map to accepted semantic commands; IME, denied
clipboard/internal fallback, and reserved-chord cases pass equally. File drop
is absent for remote/unknown clients and accepted only with `local_file_drop`.
The harness observes clipboard denial through a typed semantic status object.
Backend common-dispatch rejection for unauthorized file drop remains owned by
session/file-command integration; this task verifies client-side capability
gating.

## Done

The mandatory workflow is complete without the product browser fixture.
