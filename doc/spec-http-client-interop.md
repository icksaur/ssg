# spec-http-client-interop

Status: draft

Scope: how a WebSocket client reaches editor behavior and observes state. The
client's own UI — what it renders, its input handling, its layout — is a
separate spec and is not decided here.

## Goals

A WebSocket client should get the same editor as the in-process one: the same
commands, with the same per-command consequences, and a snapshot/delta stream it
can trust. Today it gets a strictly weaker editor, and one that is unsafe to run
alongside a terminal client on the same session.

After this work, `HttpEditorServer` reaches the editor through `EditorRuntime`
rather than around it; commands from connection threads execute on the one
thread that owns editor state; and the transport can no longer observe a
half-mutated editor.

This is **not a priority item**. It is specified now because the gap was found
while settling re-entrant dispatch, and the analysis is perishable.

## Current state (verified in code)

- `HttpEditorServer::Impl` holds `EditorSession& session`
  (`src/HttpEditorServer.cpp:476`) and calls `session.dispatch(...)` directly
  (`:203`). It uses exactly four session members: `catalog()`, `dispatch()`,
  `attach()`, `detach()`.
- `Http::Server` accepts on one thread and spawns a **detached thread per
  connection** (`/home/carl/repo/http/http.cpp:557`). So `received()` — and the
  dispatch inside it — runs on an arbitrary connection thread, never the
  thread that owns the editor.
- `HttpEditorServer` serialises its *own* message processing with a
  `std::recursive_mutex processingMutex` held across `received()`
  (`:187`), covering both the dispatch and the `publishSession` that follows.
- `publishSession` (`:311`) calls `host.snapshot(...)` per connection, derives a
  delta against that connection's last snapshot, records it in a bounded replay
  history, and hands the encoded bytes to a per-connection outbound queue
  drained by that connection's writer thread (`writeLoop`, `:368`).

### What that means, precisely

Because `EditorSession::dispatch` holds its mutex **across handler execution**,
handler bodies are serialised against each other even across threads. So the
current arrangement is not as broken as "a transport thread mutates the editor"
suggests — but it is broken in three specific ways:

1. **A WebSocket client gets no per-command policy.** `EditorRuntime::dispatch`
   performs find/prompt/picker reconciliation, follow-edit pause detection, and
   the deferred-command drain around every command. Dispatching to the session
   directly skips all of it. This is a direct **I2** violation — the two client
   kinds do not execute the same behavior.

   The follow-up case is worse than "skipped", and the difference matters.
   `Impl::defer` admits the command because it only asks whether a dispatch is
   in progress (`session->activeDispatchRevision()`), which is true during a
   WebSocket-originated `session.dispatch` like any other. So a WebSocket
   client's `palette.execute` **queues its target successfully** and then
   nothing drains it, because that path never enters `EditorRuntime::dispatch`.
   It sits in the queue until the *terminal* user next presses a key, and is
   then drained inside that unrelated dispatch — where, because palette and
   prompt follow-ups carry no client (`std::nullopt` meaning "whoever's dispatch
   this is"), it executes **as the terminal user**, and its result is returned
   as the result of the terminal user's keystroke.

   So the observable failure is not a command that does nothing. It is one
   client's command executing later, under another client's identity, attributed
   to an action that user did not take. That is an authority defect as much as
   an ordering one, and it is the sharpest argument for this spec.
2. **Post-dispatch runtime work is unsynchronised.** `EditorRuntime::dispatch`
   runs reconciliation and the drain *after* `session->dispatch` returns and its
   lock is released. A terminal client doing that concurrently with a WebSocket
   handler body is a genuine race on runtime state (documents, shell, prompt),
   which the session mutex does not cover.
3. **`snapshot()` is read off-thread.** `publishSession` reads editor state
   through the host on a connection thread while the terminal thread may be
   mutating it.

None of this is live today: nothing in `apps/` constructs an `HttpEditorServer`,
and the WebSocket tests build bare `EditorSessionBuilder` fixtures whose handlers
are stubs touching no runtime state. It is a correctness trap waiting for the
first real deployment, not a present bug.

### Correction to `doc/spec-reentrant-dispatch.md`

That spec stated that releasing the session lock around handler execution "would
not be dangerous for the reason usually given," on the grounds that the mutex
names only four fields. That is **wrong once more than one thread dispatches**:
because the lock is held across handler execution, it serialises handler bodies
between the terminal thread and connection threads, and the four field names
understate what it does.

**Already corrected in place**, rather than deferred to this spec's step 7, so
the false premise cannot be re-derived from a committed document in the
meantime. The conclusion of that spec is unaffected — nesting is forbidden by
revision accounting, not by locking — but the lock is more load-bearing than it
claimed.

## Design

**One command thread. The transport marshals onto it and waits.**

This follows the pattern the project already uses for background work
(`doc/spec-diff-default-wiring.md`): a worker thread never touches runtime state;
it hands a value to the thread that owns that state. The difference is direction
— here the transport is the producer and needs a result back.

**The server takes an `EditorRuntime&`.** All four members it uses exist on the
runtime with identical signatures, so this is a type change at the boundary, not
a rewrite. It closes the I2 gap by construction: there is no longer a path that
reaches dispatch without the runtime's per-command work.

**A command submitted by a connection thread is executed by the command
thread.** The connection thread enqueues the command with a promise, wakes the
command thread, and blocks on the future until the result is ready. This keeps
the WebSocket request/response shape the protocol already has — one command in,
one `CommandResult` out — without the transport ever entering the editor.

Mechanism chosen: **blocking submit-and-wait** over fire-and-forget with an
asynchronous result. Fire-and-forget would let a connection thread issue a
second command before the first was applied, making a client's own commands
arrive out of order — the ordering the protocol's revision model depends on. The
connection thread is already dedicated to one client and already blocks on
socket reads, so blocking it costs nothing that is not already spent.

**Who runs the command thread.** The application's event loop already is one
(`apps/ssg_main.cpp` selects on descriptors and dispatches). The queue exposes a
wake descriptor, exactly as the git-diff worker does, so the existing `select()`
learns about pending commands with no new thread and no polling. A headless
embedder that has no loop of its own is served by a small library-side runner;
that is an implementation detail, not a second behavior path.

**Snapshot publication moves to the command thread too.** `publishSession`
currently reads state off-thread. Under this design the runtime notifies after
each accepted command, on the command thread, and the transport's only job is
encoding and enqueueing bytes — which is already thread-safe, because the
per-connection outbound queue and its writer thread exist for exactly that.

**What does not change.** The wire protocol, the delta derivation, the bounded
replay history, per-connection outbound queues and writer threads, and
authentication all stay as they are. This spec moves *where code runs*, not what
it says.

## Invariants

- **I2 — Single behavior path.** In-process and WebSocket clients execute the
  same command implementation and observe the same ordered snapshot/delta model.
  This is the invariant the current arrangement violates and this work restores.
- **I3 — Authoritative revisions.** Accepted changes are totally ordered and a
  client never applies a delta to a different base revision. Serialising all
  commands onto one thread is what makes the order well-defined with several
  clients.
- **I1 / I12 — Headless core, core independence.** `ssg_http_server` stays a
  separately linkable target; nothing HTTP-shaped enters `ssg`. Taking an
  `EditorRuntime&` adds no link dependency, since that target already links all
  of `ssg`.
- **I11 — Protocol compatibility.** Every message stays versioned, bounded, and
  validated before mutation; this changes no wire format.
- **Single-threaded session mutation** (`doc/spec-diff-default-wiring.md`). This
  spec's purpose is to make the transport honor it rather than depend on a lock
  to paper over it.

## Considerations

- **Shutdown is the hard part, not the happy path.** A connection thread blocked
  on a future must be released when the runtime stops, or shutdown hangs with
  the process apparently alive. Every pending submission must be completed with
  a typed failure at teardown, and a submission arriving after teardown must be
  refused rather than queued.
- **The queue must be bounded.** A client that pipelines commands faster than
  the editor applies them must be refused, not allowed to grow the queue without
  limit (**I10**). A bounded queue plus a blocking submit gives natural
  backpressure: a misbehaving client stalls only itself.
- **`processingMutex` becomes redundant for dispatch** once commands are
  serialised by the command thread, but it also guards attach/detach and
  connection bookkeeping. Removing it is a separate simplification; do not
  conflate the two.
- **LOCK-SCOPE RULE, mandatory: a connection thread must not hold
  `processingMutex` while waiting on a submission.** `received()` currently
  takes that mutex across the whole message, dispatch included. Keeping that
  shape while adding submit-and-wait creates a lock-order inversion: the
  connection thread waits for the command thread while holding a mutex the
  command thread's post-dispatch publication may need to take, and both stop.
  The rule has two halves and BOTH are required, because either alone is one
  refactor away from being violated:
  (a) `received()` releases `processingMutex` before submitting and reacquires
      it only if it still needs it afterwards; and
  (b) the command thread's publication path must take no transport lock other
      than `connectionsMutex` and the per-connection mutex, and must never take
      `processingMutex`.
  Step 5 must state which locks its publication path may acquire, and a
  deadlock-shaped test (a client submitting while another publishes, under a
  watchdog) must exist before the two are combined.
- **`detach` on connection close** currently runs on the connection thread
  (`:402`, `:430`) and mutates session client state. It must go through the same
  marshalling as dispatch, or close races the very thing this spec fixes.
- **Reentrancy.** The command thread must never submit-and-wait on itself; that
  would deadlock exactly as nested dispatch did. A submission made from the
  command thread runs inline. This is the same rule as
  `doc/spec-reentrant-dispatch.md`, enforced in the same way.
- **Ordering across clients is arbitrary but total.** Two clients submitting
  simultaneously get *some* order, and every client sees the same one. That is
  what I3 requires; it does not require fairness.

## Risks and Mitigations

- **Risk: deadlock between the command thread and a connection thread.**
  *Mitigation*: the command thread never blocks on a submission; the
  same-thread-runs-inline rule makes that unrepresentable rather than
  documented. Oracle: a command thread submission returns without blocking.
- **Risk: shutdown hangs** with connection threads parked on futures.
  *Mitigation*: teardown completes every pending submission with a typed
  failure, and a bounded-time shutdown test asserts it.
- **Risk: this is a large change to a component with no live users**, so
  regressions are invisible until someone deploys it.
  *Mitigation*: the parity oracle below runs the same command sequence through
  both client kinds and compares the resulting revision sequence and snapshot;
  it is what makes the change checkable at all.
- **Risk: scope creep into client UI.** Out of scope by construction; this spec
  decides only how a client reaches behavior and observes state.

## Acceptance (Definition of Done)

- Observable: a terminal client and a WebSocket client attached to one runtime
  both drive the editor, and a command that queues a follow-up (`palette.execute`)
  performs it for both. Demonstrated with a real `EditorRuntime`, not a fixture
  session.
- Budgets: no added latency on the in-process path (it must not acquire a queue
  or a lock it does not already take); WebSocket command latency bounded by one
  event-loop wake.
- Gates: `bash scripts/check.sh` green — 0 warnings, all tests passing.
- Oracles:
  - behavior parity (I2) -> `sameCommandSequenceYieldsSameRevisionsAndSnapshot`
  - follow-ups run for WebSocket clients -> `aQueuedFollowUpRunsForAWebsocketClient`
  - one total order (I3) -> `concurrentClientsProduceOneTotalRevisionOrder`
  - no off-thread editor access -> `everyCommandExecutesOnTheCommandThread`
  - bounded shutdown -> `pendingSubmissionsAreRefusedAtShutdownWithoutHanging`

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Write the parity oracle: same command sequence in-process and over WebSocket against a real `EditorRuntime`, comparing revision sequence and final snapshot. Must FAIL before step 4 for a command with a follow-up. | `tests/test_end_to_end.cpp` | `sameCommandSequenceYieldsSameRevisionsAndSnapshot`, `aQueuedFollowUpRunsForAWebsocketClient` | I2 |
| 2 | Write the thread-affinity oracle: assert every handler observes the command thread's id, from both client kinds. Must FAIL before step 3. | `tests/test_end_to_end.cpp` | `everyCommandExecutesOnTheCommandThread` | single-threaded mutation |
| 3 | Add the command queue to `EditorRuntime`: bounded submit-and-wait, a wake descriptor for the host loop, same-thread submissions run inline, teardown completes pending submissions with a typed failure. | `include/ssg/EditorRuntime.h`, `src/EditorRuntime.cpp`, `src/runtime/editor_runtime_internal.h` | step 2; `pendingSubmissionsAreRefusedAtShutdownWithoutHanging` | I3, I10 |
| 4 | Change `HttpEditorServer` to hold `EditorRuntime&`, submitting dispatch, attach and detach through the queue. **Interim contract, required because 4 and 5 land separately**: until step 5, `host.snapshot` is still read on the connection thread, so step 4 must submit that read through the queue too rather than leave it racing while the rest looks migrated. Step 5 then replaces the submitted read with a command-thread notification. | `include/ssg/HttpEditorServer.h`, `src/HttpEditorServer.cpp` | step 1; thread-affinity oracle covers snapshot reads as well as commands | I2, I11 |
| 5 | Move snapshot publication onto the command thread: the runtime notifies after each accepted command; the transport only encodes and enqueues. Declares which locks the publication path may take (per the lock-scope rule) and holds no transport lock across a submission. | `src/HttpEditorServer.cpp`, `src/EditorRuntime.cpp` | `concurrentClientsProduceOneTotalRevisionOrder`; a deadlock-shaped test under a watchdog | I2, I3 |
| 6 | Wire the wake descriptor into the application loop beside the existing git-diff and init-script descriptors. | `apps/ssg_main.cpp` | pty run: a WebSocket client drives a live terminal session | I2 |
| 7 | Record the outcome. (The `doc/spec-reentrant-dispatch.md` correction is already applied, not pending.) | `doc/spec-http-client-interop.md`, `doc/spec.md` | doc gates green | I2 |

## Out of scope

- **Client UI**: rendering, input, layout, and the client's own command surface.
  A separate spec.
- **Removing `processingMutex`**, which also guards connection bookkeeping.
- **Multi-session hosting**: this spec assumes one runtime per server, as today.
- **Authentication and transport security**, unchanged.

## Rationale (optional, skippable)

The gap was found while specifying re-entrant dispatch: asking "where does the
deferred-command queue belong?" surfaced that one client kind never drains it,
because it never goes through the code that owns it. The first instinct was to
move the queue somewhere both paths could see. That would have been treating the
symptom — the queue is not the only thing a WebSocket client misses, merely the
newest.

The honest description is that `HttpEditorServer` was written against
`EditorSession` when that was the whole editor, and `EditorRuntime` grew into
the real one around it. Everything the transport misses is a consequence of that
one fact, and pointing it at the runtime fixes the class rather than the
instance.

Marshalling is then forced rather than chosen: once the transport calls into the
runtime, and the transport is thread-per-connection while the runtime is
single-threaded, something has to move the call to the right thread. Doing that
explicitly is better than relying on a mutex that happens to be held across
handler execution — which is what the current arrangement leans on without
saying so.
