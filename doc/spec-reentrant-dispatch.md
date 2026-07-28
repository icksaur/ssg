# spec-reentrant-dispatch

Status: draft

## Goals

Settle whether a command handler may dispatch another command synchronously,
and make the answer safe to depend on.

Today nesting is refused and composition happens by deferral, but the deferral
mechanism sits in `EditorRuntime` while every client reaches dispatch through
`EditorSession` — so a WebSocket client's handler can queue commands that never
run, and two clients dispatching at once share one unsynchronised queue. After
this work, composition behaves identically on every path, a queued command can
never execute under another client's dispatch, and the reason nesting is
forbidden is enforced by a test rather than by a comment.

## Decision

**No. A handler may never dispatch synchronously — on any path, for any command
effect.** Composition is by deferral only.

The reason is **I3**, not the mutex.

`EditorSession::dispatch` captures `currentRevision` before running the handler
(`src/EditorSession.cpp:129`) and, on an accepted mutation, writes
`impl_->revision = currentRevision + 1` after it returns (`:186`). A nested
mutating dispatch would advance the revision to `N+1` and then have the outer
write `N+1` over it: two accepted mutations, one revision step. Clients that
replay deltas against a base revision would silently miss an edit. Making the
mutex recursive does not fix this; it only makes the loss reachable.

This was **verified experimentally, not inferred**: making the mutex recursive,
removing both nested-dispatch guards, rebuilding the library, and running one
mutating command whose handler dispatches another mutating command yields two
accepted mutations and a revision delta of **1**. The inner advanced the
revision to 2; the outer then wrote its own pre-captured `currentRevision + 1`,
which was also 2. A client replaying deltas sees one edit where two happened.

Deferral has no such problem: each deferred command is its own dispatch with its
own revision step, ordered. **One accepted mutating dispatch advances the
revision exactly once** is the property that both forbids nesting and is
satisfied by deferral.

The rule is uniform — it does not exempt observing commands, though I3 would
permit that. A rule conditional on a command's effect would mean that
reclassifying a command from `Observation` to `Mutation` silently converts
working script code into a refusal, and callers would have to know each
command's effect to know which call style is legal. One rule, no exceptions.

### What the mutex actually protects (correcting a stated assumption)

Prior notes justified the refusal as "the handler would run with session state
unlocked if we released the lock." That is not the case, and the spec should not
rest on it. `impl_->mutex` guards only `revision`, `topology`, `clients` and the
catalog pointer. `EditorRuntime::Impl` **is** the `CommandServices`
(`src/runtime/editor_runtime_internal.h:96`), so handlers already mutate
documents, workspace, shell, prompt and search through `context.services()` with
no protection from this mutex at all.

Releasing the lock around handler execution would therefore not be dangerous for
the reason usually given. It is still not proposed here, because it does not
address the revision-conservation problem, which is the actual obstacle.

## Design

**The queue moves from `EditorRuntime` into `EditorSession`, and the drain loop
moves with it.**

Ownership. The queue becomes state of the *dispatch in progress*, not a field on
the runtime. `EditorSession` already tracks which thread is inside a dispatch
(`dispatchingThread`, `activeDispatchRevision`); the queue is scoped the same
way, so a command queued by one client's handler cannot be seen, drained, or run
by another client's dispatch on another thread.

Draining. `EditorSession::dispatch` runs the command and then drains what that
command queued, to empty, before returning. Draining is part of dispatching, so
no caller can forget it — the palette bug fixed in `4458b11` came from a caller
that returned early, and a caller obligation would invite it again. This also
makes the WebSocket path (`src/HttpEditorServer.cpp:203`, which holds only an
`EditorSession&` and calls `session.dispatch` directly) behave identically to
the in-process path without knowing the mechanism exists.

Per-command policy. `EditorRuntime::dispatch` currently wraps each command with
follow-edit pause detection and prompt/picker reconciliation, and needs to keep
doing so for every deferred command, not once per outer call. Since the drain
loop is moving into the session, that policy is supplied as a **dispatch
observer installed once** when the session is built, rather than as work the
caller does around each call. The observer receives the client each command ran
as, because `shouldPauseForLocalEdit` depends on that client's origin: a
script's edit runs as the script client and must not count as the user's local
edit.

The hook is the session reporting command completion to its host, not a general
extension point; it is required because the drain moved into the session and
per-command reconciliation is observable by the next command in a chain (see the
investigation below).

Mechanism chosen: observer-installed-at-build over returning the executed chain
to the caller. Returning the chain keeps the runtime in control but restores the
caller obligation — every caller must remember to iterate it, which is the
failure this design exists to remove.

Refusal. The refusal stays (it is the decision), but its message must name the
alternative rather than only the prohibition.

`runTransaction` (`src/EditorRuntime.cpp:700`) is a no-op passthrough with 43
call sites and is **out of scope**: it composes *operations*, not commands, and
nothing in this spec changes it.

## Invariants

- **I2 — Single behavior path.** In-process and WebSocket clients execute the
  same command implementation. A handler that queues a command must have it run
  on both paths; today it runs on neither unless the caller is `EditorRuntime`.
- **I3 — Authoritative revisions.** Accepted state changes are totally ordered
  and a client never applies a delta to a different base revision. This is the
  invariant that forbids synchronous nesting.
- **I5 — Atomic edits.** A failed command changes no document, and every
  accepted transaction belongs to exactly one undo unit. A handler that fails
  must not have its queued commands performed.
- **I10 — Bounded extension failure.** A script cannot block the session
  indefinitely: the queue stays bounded, and exceeding it is refused rather than
  spun on.
- **I1 / I12 — Headless core.** The mechanism lives in the session, usable with
  no runtime, renderer or transport.

## Considerations

- **Concurrent dispatch on one session is real and tested**, not hypothetical:
  `concurrentTuiAndWebsocketClientsShareFollowInterruption`
  (`tests/test_end_to_end.cpp`) attaches an in-process client and a WebSocket
  client to a single session. The current queue is a plain `std::vector` on
  `EditorRuntime::Impl` pushed under the session lock and drained outside it, so
  that configuration is a data race and, worse, lets one client's dispatch
  execute commands another client queued.
- **The WebSocket gap is pre-existing, not a regression.**
  `pendingPaletteTarget` and `pendingPromptCommand` had the same shape before
  they were unified into one queue; unification made it one fact instead of
  three. It is currently unreachable in shipped code only because the WebSocket
  tests build a bare `EditorSessionBuilder` session whose catalog has no
  queueing commands — an accident of test construction, not a guarantee.
- **Ordering across a nested chain.** A drained command may queue more. The
  order must remain the order requested, breadth-wise, and the existing bound
  (64) is what stops a script that queues itself forever.
- **Failure policy is already decided and must be preserved**: a handler that
  fails performs none of its requests; the first queued failure is reported,
  named, and the remainder abandoned.
- **The observer must not become a second dispatch path.** It observes; it may
  not itself dispatch, or the rule this spec sets is violated by its own
  mechanism. This is enforced, not merely stated: the observer runs while the
  dispatch that triggered it is still the active one, so an observer that
  dispatches is refused by the same rule as a handler that does, and step 5's
  oracle asserts it.
- **`EditorSession::revision()` answers from the in-progress dispatch** when
  called on the dispatching thread. That must survive the move, since a handler
  building a deferred command reads it.

## Risks and Mitigations

- **Risk: the observer changes `EditorRuntime`'s dispatch structure**, which is
  heavily used UI code (palette, prompt, file picker).
  *Mitigation*: the parity and revision oracles below run before the move; the
  palette and prompt paths already have regression tests
  (`aScriptCommandRunFromThePaletteAlsoRunsWhatItAsksFor`,
  `submittingAPathPromptRedispatchesTheCommandThatOpenedIt`); drive the real
  binary over a pty for palette and prompt before committing.
- **Risk: moving the drain into the session changes when reconciliation runs.**
  *Mitigation*: the observer is invoked per command, preserving today's
  per-command reconciliation rather than batching it.
- **Risk: a script still cannot learn whether its command succeeded**, because
  the decision makes that permanent, and `ssg.command` reports acceptance only.
  *Mitigation*: failures are reported against the invoking dispatch and name the
  failing command, so nothing is silently swallowed. Documented in
  `doc/config.md`. A future Lua state-read API must arrive with an answer to
  this; it is out of scope here.
- **Risk: scope creep into the WebSocket transport.** Making the WebSocket path
  drain is in scope; making it use `EditorRuntime` is not -- but that is now a
  named follow-up with a reason, not an unexamined exclusion: a WebSocket client
  currently bypasses all runtime per-command policy, which is a wider I2 gap
  than deferral and wants its own change.

## Investigated: routing the WebSocket server through EditorRuntime

Asked whether handing `HttpEditorServer` an `EditorRuntime&` instead of an
`EditorSession&` would remove the need for the observer. **It does not**, and
the reasoning is worth recording because it is not obvious.

What the server needs is small: it uses exactly four session members --
`catalog()`, `dispatch()`, `attach()`, `detach()` -- and `EditorRuntime` has
all four with identical signatures. Build layering permits it too:
`ssg_http_server` is a separate static library that already links all of `ssg`,
so depending on the runtime adds no link dependency and does not weaken I12.

But the observer is not removable by re-routing, because:

- The queue must move into `EditorSession` for scoping and thread-safety
  (defect 2), and **the drain has to live where the queue lives** -- otherwise
  the invariant is split across two layers and "the caller must remember to
  drain" returns, which is exactly the failure this design removes.
- Per-command reconciliation is load-bearing, not cosmetic. The three
  `reconcile*` steps are convergent -- each is a function of current state and
  idempotent -- so running them once after a chain yields the same FINAL state.
  What changes is what a *deferred* command observes: today `palette.execute`
  cancels its prompt and the target then runs against reconciled state, seeing
  focus already off the prompt. Batching reconciliation to the end of the chain
  would let the target observe `shell.focus() == Prompt` with no active prompt.
- So the session's drain must call back into its host once per command
  regardless of which object the transport holds.

Re-routing is still worth doing, for a **larger reason than deferral**: because
`HttpEditorServer` calls `session.dispatch` directly, a WebSocket client today
receives none of the runtime's per-command policy at all -- no find/prompt/
picker reconciliation and no follow-edit pause -- not merely no drain. That is
a broader I2 gap than the one this spec set out to close. It is latent rather
than live only because nothing in `apps/` wires the server to a runtime session
yet; the WebSocket tests build bare `EditorSessionBuilder` sessions.

Decision: keep the per-command hook (it is structurally required), and treat
re-routing as its own change. The hook is better understood not as an
"observer" but as the session telling its host that a command completed --
a single-purpose interface, which is why it is acceptable rather than a
general-purpose extension point.

## Acceptance (Definition of Done)

- Observable: a script command bound to a key composes built-ins from a
  keystroke, from the palette, and over a WebSocket connection, with the same
  resulting revision sequence on each path. Palette and prompt verified against
  the real binary over a pty.
- Budgets: no added allocation on the non-deferring path (the overwhelmingly
  common case is a handler that queues nothing); queue bound stays 64.
- Gates: `bash scripts/check.sh` green — build with 0 warnings, all tests
  passing.
- Oracles:
  - revision conservation -> `revisionAdvancesExactlyOncePerAcceptedMutation`
  - path parity (I2) -> `queuedCommandsRunIdenticallyInProcessAndOverWebsocket`
  - no cross-client leakage -> `commandsQueuedByOneClientNeverRunUnderAnother`
  - refusal names the alternative -> `aHandlerThatDispatchesIsToldToDeferInstead`
  - the observer cannot dispatch -> `aDispatchObserverThatDispatchesIsRefused`

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Write the revision-conservation oracle: independently count accepted mutating dispatches over a chain (outer + deferred) and assert the session revision advanced exactly that many times. Must pass on today's deferral and fail under nesting. | `tests/test_command_dispatch.cpp` | `revisionAdvancesExactlyOncePerAcceptedMutation`; perturbation: make the mutex recursive and let a handler dispatch — the count must diverge | I3 |
| 2 | Write the cross-client leakage oracle: two clients dispatching concurrently on one session, one queueing; assert no command queued by one ever executes under the other's dispatch. Must FAIL before step 4. | `tests/test_command_dispatch.cpp` | `commandsQueuedByOneClientNeverRunUnderAnother` (sanitiser build) | I2, I3 |
| 3 | Write the path-parity oracle: same command sequence in-process and over WebSocket, asserting identical revision sequence and final snapshot. Must FAIL before step 5 for a queueing command. | `tests/test_end_to_end.cpp` | `queuedCommandsRunIdenticallyInProcessAndOverWebsocket` | I2 |
| 4 | Move the queue into `EditorSession`, scoped to the in-progress dispatch alongside `dispatchingThread`; move the drain loop into `EditorSession::dispatch`; delete `EditorRuntime::deferDispatch`'s storage. Preserve: order, the 64 bound, failed-handler discards its queue, first failure reported and named. | `include/ssg/EditorSession.h`, `src/EditorSession.cpp`, `include/ssg/EditorRuntime.h`, `src/EditorRuntime.cpp`, `src/runtime/editor_runtime_internal.h`, and the two current queue producers `src/runtime/navigation.cpp` (`palette.execute`, pushes `runtime.deferredCommands` directly) and `src/runtime/presentation.cpp` (`prompt.submit`, same) | steps 1-2 pass | I2, I3, I5, I10 |
| 5 | Add the dispatch observer installed at session build; move follow-edit pause and prompt/picker reconciliation onto it, receiving the client each command ran as. | `include/ssg/EditorSessionBuilder.h`, `src/EditorSessionBuilder.cpp`, `src/EditorRuntime.cpp` | step 3 passes; `aDispatchObserverThatDispatchesIsRefused`; existing palette/prompt regression tests unchanged and green | I2, I14 |
| 6 | Make the refusal name the alternative; assert the message is actionable. | `src/EditorSession.cpp`, `src/EditorRuntime.cpp`, `tests/test_command_dispatch.cpp` | `aHandlerThatDispatchesIsToldToDeferInstead` | - |
| 7 | Record the decision and its reason in `doc/spec.md` next to I3, and close the open question in `doc/spec-lua-commands.md`'s Status. | `doc/spec.md`, `doc/spec-lua-commands.md` | doc gates green | I3 |

## Rationale (optional, skippable)

The question "may a handler dispatch synchronously?" looks like a question about
locking, and was recorded that way. Investigating it showed the lock is close to
irrelevant: it protects four fields, while the editor state handlers actually
mutate is entirely outside it. Had the lock been the obstacle, the fix would
have been mechanical — release it around handler execution.

The real obstacle is bookkeeping. A dispatch is the unit that advances the
revision, and the revision is what remote clients order deltas by (I3). Nesting
makes a dispatch produce an unbounded number of revision steps, and the current
code — which computes the new revision from a value captured before the handler
ran — would lose them. Deferral is not a workaround for a lock; it is the shape
that keeps a dispatch equal to one revision step.

That reframing also explains why the answer is "never" as a policy rather than
"not yet" as a schedule — but the claim should be stated no more strongly than
the evidence supports. The experiment proves the CURRENT implementation violates
I3 under nesting; it does not prove every conceivable synchronous design must.
A design that advanced the revision from its live value rather than a
pre-captured one would not lose steps. It would instead make one dispatch
produce an unbounded number of revision steps, so a caller could no longer treat
the revision it gets back as "mine + 1" — which is a change to what a dispatch
result means to every client, i.e. a protocol change rather than an adjustment
to this mechanism. "Never" is chosen on that basis, and would have to be
revisited by a spec that changes the revision contract, not by one that changes
locking.

The alternative considered and rejected was permitting synchronous nesting for
observing commands, which I3 would allow. It was rejected because it makes
legality depend on a command's effect: a command reclassified from
`Observation` to `Mutation` would silently break every script that called it,
with no signal at the point of change.
