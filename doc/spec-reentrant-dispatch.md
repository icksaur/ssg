# spec-reentrant-dispatch

Status: done

## Goals

Settle whether a command handler may dispatch another command synchronously,
and record the answer where it cannot be lost.

The answer is no, and the reason is revision accounting, not locking.
Composition already works, by deferral. This spec pins that decision with a test
so it cannot regress, makes the refusal tell a caller what to do instead, and
closes the one way the queue can currently be written unchecked.

It deliberately changes almost no code. An earlier draft proposed moving the
deferred-command queue into `EditorSession` and adding a per-command hook; that
designed around a concurrency model this project does not use. It is withdrawn
and recorded at the end.

## Decision

**No. A handler may never dispatch synchronously — on any path, for any command
effect.** Composition is by deferral only.

The reason is **I3**, not the mutex.

`EditorSession::dispatch` captures `currentRevision` before running the handler
(`src/EditorSession.cpp:129`) and, on an accepted mutation, writes
`impl_->revision = currentRevision + 1` after it returns (`:186`). A nested
mutating dispatch advances the revision to `N+1`, and the outer then writes
`N+1` over it: two accepted mutations, one revision step. A client replaying
deltas against a base revision silently misses an edit.

Verified, not inferred: making the mutex recursive, removing both
nested-dispatch guards, rebuilding the library, and running one mutating command
whose handler dispatches another yields two accepted mutations and a revision
delta of **1**. A first attempt gave a false negative because the probe linked
against a stale `libssg.a` that still contained the guards.

Deferral has no such problem: each deferred command is its own dispatch with its
own revision step, in order. **One accepted mutating dispatch advances the
revision exactly once** is the property that both forbids nesting and is
satisfied by deferral.

The rule is uniform — it does not exempt observing commands, though I3 would
permit that. A rule conditional on a command's effect would mean reclassifying a
command from `Observation` to `Mutation` silently converts working script code
into a refusal, and callers would need to know each command's effect to know
which call style is legal. One rule, no exceptions.

### Two corrections to earlier reasoning

**The lock is not the obstacle.** Prior notes justified the refusal as "the
handler would run with session state unlocked if we released the lock."
`impl_->mutex` guards only `revision`, `topology`, `clients` and the catalog
pointer. `EditorRuntime::Impl` **is** the `CommandServices`
(`src/runtime/editor_runtime_internal.h:96`), so handlers already mutate
documents, workspace, shell, prompt and search through `context.services()` with
no protection from that mutex. Releasing it would not be dangerous for the
reason usually given; it simply would not fix the revision accounting.

**Concurrent dispatch is not this project's model.** An earlier draft called
concurrent dispatch on one session "real and tested." That overstated it: the
cited test (`concurrentTuiAndWebsocketClientsShareFollowInterruption`) drives a
bare `EditorSessionBuilder` fixture, not a runtime. The project's stated
invariant is the opposite — *session mutation is single-threaded end to end*
(`doc/spec-diff-default-wiring.md`) — with background threads enqueueing values
that the command thread drains, the shape `ScratchStore` and the git-diff worker
already use.

## Design

**No structural change.** The queue stays on `EditorRuntime::Impl` and the drain
stays inside `EditorRuntime::dispatch`.

Under single-threaded session mutation that placement is already correct: the
queue is only touched during a dispatch on the command thread, and
`dispatchAndDrain` leaves it empty on every exit — drained to empty on success,
cleared on failure. There is no race to remove and no cross-client leakage to
prevent, so there is nothing to move.

What remains is small:

- an oracle that pins the decision, so a future change enabling nesting fails a
  test rather than shipping;
- a refusal that names the alternative, defined once rather than duplicated;
- one entry point for queueing, because there are currently two;
- the rule and its reason written down.

**One entry point.** Queueing has two ways in today.
`EditorRuntime::deferDispatch` checks that a dispatch is in progress and
enforces the 64-command bound. Direct `runtime.deferredCommands.push_back` in
`src/runtime/navigation.cpp` (`palette.execute`) and
`src/runtime/presentation.cpp` (`prompt.submit`) checks neither. Harmless today
— each pushes one command from inside a handler — but it is two ways to do one
thing where only one is safe, and the unsafe one is the shorter. The field
becomes private and `deferDispatch` becomes its only writer.

## Invariants

- **I3 — Authoritative revisions.** Accepted state changes are totally ordered
  and a client never applies a delta to a different base revision. This forbids
  synchronous nesting.
- **I5 — Atomic edits.** A handler that fails must not have its queued commands
  performed.
- **I10 — Bounded extension failure.** A script cannot block the session: the
  queue stays bounded, and exceeding it is refused rather than spun on.
- **Single-threaded session mutation** (`doc/spec-diff-default-wiring.md`). All
  session and runtime mutation happens on one command thread; background threads
  enqueue and never touch runtime state. This spec depends on it and does not
  change it.

## Considerations

- **The refusal is enforced twice**, in `EditorSession::dispatch` and
  `EditorRuntime::dispatch`, with a duplicated message string. The session's is
  the real guard; the runtime's exists because its wrapper touches the session
  before dispatching. Keep both; define the message once.
- **The bound and the failure policy are settled** and must survive: a handler
  that fails performs none of its requests; the first queued failure is
  reported, named, and the remainder abandoned.
- **A drained command may queue more**, so the drain loops; the bound is what
  stops a script that queues itself forever.
- **Step 3 must not change palette or prompt behavior.** Both are heavily used
  UI paths with existing regression tests; routing them through `deferDispatch`
  is meant to be behavior-preserving, and the bound must not reject their single
  push.

## Out of scope — and the more important problem

`HttpEditorServer` holds an `EditorSession&` and calls `session.dispatch`
directly (`src/HttpEditorServer.cpp:203`), bypassing `EditorRuntime` entirely. A
WebSocket client therefore receives **none** of the runtime's per-command
policy: no find/prompt/picker reconciliation, no follow-edit pause, and no drain
of anything a handler queued. That is a direct **I2** violation — in-process and
WebSocket clients do not execute the same behavior — and it is far wider than
deferral, which is merely one of the things skipped.

It is latent rather than live only because nothing in `apps/` wires the server
to a runtime session; the WebSocket tests build bare fixture sessions.

This belongs in its own spec because the fix is a transport change, not a
dispatch change. The server would take the runtime instead of the session — it
uses exactly four members (`catalog`, `dispatch`, `attach`, `detach`) and
`EditorRuntime` has all four with identical signatures — and would marshal
commands onto the command thread, delivering results through the per-connection
outbound queue it already owns, rather than dispatching inline on a transport
thread. Build layering permits it: `ssg_http_server` is a separate static
library that already links all of `ssg`.

Naming it here rather than folding it in keeps this spec honest: the deferral
mechanism is not what is wrong for HTTP clients.

## Risks and Mitigations

- **Risk: a future maintainer reads the refusal as a limitation** and "fixes" it
  by making the mutex recursive.
  *Mitigation*: the revision-conservation oracle fails loudly, and the refusal
  message and `doc/spec.md` both carry the reason.
- **Risk: step 3 regresses the palette or prompt.**
  *Mitigation*: existing regression tests
  (`aScriptCommandRunFromThePaletteAlsoRunsWhatItAsksFor`,
  `submittingAPathPromptRedispatchesTheCommandThatOpenedIt`) plus a pty run of
  the real binary exercising both before committing.
- **Risk: a script still cannot learn whether its command succeeded**, and this
  decision makes that permanent.
  *Mitigation*: failures are reported against the invoking dispatch and name the
  failing command; documented in `doc/config.md`. A future Lua state-read API
  must arrive with an answer. Out of scope.

## Acceptance (Definition of Done)

- Observable: editor behavior unchanged — a script command bound to a key still
  composes built-ins from a keystroke and from the palette, verified against the
  real binary over a pty.
- Budgets: no behavior or performance change; a test, a message, and one field's
  visibility.
- Gates: `bash scripts/check.sh` green — 0 warnings, all tests passing.
- Oracles:
  - revision conservation -> `revisionAdvancesExactlyOncePerAcceptedMutation`
  - refusal names the alternative -> `aHandlerThatDispatchesIsToldToDeferInstead`

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Write the revision-conservation oracle: independently count accepted mutating dispatches across a chain (outer plus deferred) and assert the session revision advanced exactly that many times. | `tests/test_command_dispatch.cpp` | `revisionAdvancesExactlyOncePerAcceptedMutation`; perturbation: recursive mutex + both guards removed + rebuild — the count must diverge | I3 |
| 2 | Make the refusal name the alternative, defined once rather than duplicated across the two guards. | `src/EditorSession.cpp`, `src/EditorRuntime.cpp`, `include/ssg/EditorSession.h` | `aHandlerThatDispatchesIsToldToDeferInstead` | - |
| 3 | Give the queue one writer: make `deferredCommands` private to its owner and route `palette.execute` and `prompt.submit` through `deferDispatch`, so the in-dispatch check and the bound apply to every queueing path. | `src/runtime/editor_runtime_internal.h`, `src/runtime/navigation.cpp`, `src/runtime/presentation.cpp`, `src/EditorRuntime.cpp` | palette/prompt regression tests stay green; a direct push no longer compiles | I10 |
| 4 | Record the decision and its reason in `doc/spec.md` beside I3, and close the open question in `doc/spec-lua-commands.md`'s Status. | `doc/spec.md`, `doc/spec-lua-commands.md` | doc gates green | I3 |

## Withdrawn

An earlier draft proposed moving the queue into `EditorSession`, moving the
drain with it, and adding a per-command hook installed at session build so
`EditorRuntime` could keep reconciling around each drained command.

Withdrawn because neither motivating defect exists under this project's actual
concurrency model:

- The "data race on the queue" assumed concurrent dispatch from several threads.
  Session mutation is single-threaded end to end, so the queue is only ever
  touched on the command thread, and it is empty whenever a dispatch returns.
- The "WebSocket clients never drain" defect is real, but it is a symptom of the
  transport bypassing `EditorRuntime`, not of where the queue lives. Moving the
  queue would have made queued commands run for WebSocket clients while they
  still missed reconciliation and follow-edit pause — a worse outcome than
  fixing the bypass, because it would have made the layering error harder to
  see.

The withdrawn design cost a new hook interface, changes to three headers, and a
new session-owned queue type, to fix nothing that was broken. Recorded so the
same idea is not re-derived.

## Status

Implemented in `bdd6148`, `d15f42c`, `075b811` and this commit. Gate green
(93 tests, 0 warnings).

- **Step 1** `tests/test_command_dispatch.cpp` counts accepted mutating
  dispatches independently of the counter under test and asserts the revision
  advanced by exactly that many steps -- across a flat chain, a nested chain, an
  observing command, and a chain whose queued command fails. Confirmed to fail
  for the right reason under the prescribed perturbation.
- **Step 2** `EditorSession::kNestedDispatchRefusal` is the single definition,
  used by both guards, naming the alternative and the reason.
- **Step 3** the queue's storage is private to `DeferredCommandQueue` and
  `Impl::defer` is its only writer; a direct `push_back` no longer compiles.
  `palette.execute` and `prompt.submit` now report a refusal rather than
  queueing into a dispatch that will not drain. Palette and save-as verified
  against the real binary over a pty.
- **Step 4** recorded beside I3 in `doc/spec.md` and closed in
  `doc/spec-lua-commands.md`.

The out-of-scope item stands and is the next thing worth doing:
`HttpEditorServer` bypasses `EditorRuntime`, so a WebSocket client receives no
per-command reconciliation, no follow-edit pause and no drain -- a wider I2 gap
than deferral, needing its own spec.
