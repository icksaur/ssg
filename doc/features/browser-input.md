# spec-browser-input

## Goals

Make all required interaction workflows reachable in Chromium, Firefox, and WebKit while keeping platform input capture and clipboard permission handling in clients.

## Design

The API publishes the authoritative keymap, required command metadata, semantic hit targets, clipboard requests, and the current principal's host-granted capability IDs in per-client snapshot state. Clients map raw events to semantic commands. The accepted required-command fixture is the exact union of normative command IDs in all P0 feature specs and is reviewed before registry/keymap implementation. Every entry records its owning task, required capabilities, and Lua/keymap/palette availability. `file.open_dropped_content` is the only capability-gated entry: it requires `local_file_drop` and is excluded from Lua, keymaps, and the palette; every other required entry has no required capability and is available through all three surfaces. Clipboard read requires a secure-context user gesture; unavailable requests use the internal register and produce an actionable footer status. Browser drag-and-drop is excluded for ordinary remote clients. A client renders and emits file-drop interaction only when snapshot capabilities include `local_file_drop`; the common command dispatcher independently enforces the same grant.

## Invariants

I6, I16, I17, I18, I20 from `doc/spec.md`.

## Considerations

- IME submits committed UTF-8 text, never raw composition internals.
- Browser-reserved chords cannot be required defaults.
- Mouse selection, wheel, and scrollbar gestures use the same semantic command path as keyboard navigation.
- Locality is host-granted; browser code may not infer or self-assert it.

## Risks and Mitigations

- Browser differences: use real-browser event capture, not synthetic unit events alone.
- Circular command coverage: compare implementation against the independently accepted fixture.

## Acceptance (Definition of Done)

- Observable: every required interactive command has a browser-deliverable route or a documented gesture-gated flow.
- Budgets: input-to-command translation adds no backend work.
- Gates: Chromium, Firefox, and WebKit conformance suites are green.
- Oracles: captured event fixtures, reserved-chord denylist, IME goldens, clipboard permission/gesture cases, required-command coverage, and local-accept/remote-reject file-drop capability cases.

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Accept required-command and browser-reserved fixtures | `data/required-commands.json`, `tests/browser/reserved-chords.json` | exact comparison with the union of all P0 normative lists, independently maintained category/count/owner data, exact capability and Lua/keymap/palette exclusions, and browser docs/capture | I6, I18, I20 |
| 2 | Define keymap and hit-target API data | `include/ssg/input.h`, `data/default-keymap.json`, `tests/test_input.cpp` | fixture coverage and backend dependency scan | I16, I17 |
| 3 | Implement browser input, IME, mouse, and clipboard adapters | `examples/browser/*`, `tests/browser/*` | real-browser capture and permission cases | I6, I18 |

## Rationale (optional, skippable)

Browser feasibility is a product boundary, not a later client compatibility task.
