# plan-2-one-typed-interaction-ingress

## Goals

Make identical user intent produce the same authoritative transition from every
client. Clients translate native device events into typed semantic input but do
not sequence editor commands or own pointer/picker product policy.

## Design

Extend the existing typed client-input vocabulary with semantic targets,
gestures, and values. Library routing resolves those inputs through the command
catalog and interaction authority. Keyboard and pointer routes converge before
behavior executes. Browser prediction and fuzzy narrowing remain local over
published authoritative data and reconcile through normal results.

## Invariants

- INPUT-1: Product behavior and command sequencing execute only in the library.
  State at `EditorSession::input`.
- INPUT-2: Equal typed input at equal authoritative state yields equal
  transitions regardless of client. State at `ClientInput`.
- INPUT-3: Every pointer-reachable user action remains keyboard reachable
  through the authoritative keymap. State at semantic interaction registration.
- INPUT-4: Client input cannot establish identity, capability, or authorization.
  State at the host attachment/input boundary.
- INPUT-5: Local prediction and fuzzy narrowing never become authoritative and
  require no routine round trip. State at prediction reconciliation APIs.

## Considerations

Cover tab close/activate, tree select/activate, external actions, status
actions, selection gestures, multi-cursor modifiers, word selection, and
scrolling. Native coordinates stay in clients; typed input names semantic
targets and offsets. The load-bearing device conversions are
`routePointerInput`/terminal event dispatch in `apps/pointer_routing.cpp` and
`apps/ssg_main.cpp`, plus pointer, keyboard, picker, and action handlers in
`apps/web/client.mjs`. Avoid feature-specific transport messages when command
or client-input frames can carry the operation.

## Risks and Mitigations

- A generic event bag would hide unsupported combinations: use closed variants
  with typed payloads.
- Moving fuzzy query processing to the server would regress responsiveness:
  retain the full candidate publication/local narrowing contract.
- Compound operations could expose intermediate revisions: execute each typed
  input as one library transaction.

## Acceptance (Definition of Done)

- Observable: TUI and web keyboard/pointer behavior remain equivalent.
- Budgets: local typing, selection prediction, and fuzzy narrowing remain
  independent of network latency.
- Gates: `scripts/check.sh` and `scripts/check.sh push`.
- Oracles: table-driven equal-input/equal-transition tests; an inventory test
  asserting every registered pointer semantic action resolves to an
  authoritative command also present in a keyboard-reachable keymap context;
  stale revision/capability rejection; existing browser prediction settlement
  tests.

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Define closed semantic interaction variants | `include/ssg/ClientInput.h`, related input codecs and tests | table: valid and invalid variant payloads | INPUT-2, INPUT-4 |
| 2 | Route semantic interactions through library transactions | `src/EditorSession.cpp`, `src/runtime/editing.cpp`, `src/runtime/presentation.cpp`, interaction/command transition files | equal-input/equal-transition reference cases | INPUT-1, INPUT-2 |
| 3 | Convert TUI device routing to semantic input translation | `apps/pointer_routing.*`, `apps/ssg_main.cpp`, terminal tests | parity: prior command outcomes equal new input outcomes | INPUT-1, INPUT-3 |
| 4 | Convert browser interaction routing without moving prediction authority | `apps/web/client.mjs`, `apps/web/reconcile.mjs`, web tests | prediction settlement and typed-frame fixtures | INPUT-1, INPUT-5 |
| 5 | After both client migrations, delete feature-specific client command sequences and redundant message kinds made obsolete here | protocol/client routing files; depends on Steps 3 and 4 | inventory: no client behavior sequence remains | INPUT-1, INPUT-3 |

## Rationale

One typed ingress removes the most failure-prone duplication without requiring
clients to share native event or layout representations.
