# browser-input-conformance

- Spec: `doc/features/browser-input.md`, Plan 3
- Depends: `input-keymap-contract`
- Branch: `browser-input-conformance-task`

## Scope

Build the Chromium/Firefox/WebKit conformance harness for keyboard, IME, mouse,
wheel, scrollbar, and clipboard gesture/permission translation. Add file-drop
translation only to the host-authorized local-client fixture.

## Files

`tests/browser/input/`, `cmake/components/browser-input-conformance.cmake`

## Oracle

Real-browser captured events map to accepted semantic commands; IME, denied
clipboard/internal fallback, and reserved-chord cases pass equally. File drop
is absent for remote/unknown clients and accepted only with `local_file_drop`.

## Done

The mandatory workflow is complete without the product browser fixture.
