# spec-diff-default-wiring

Status: DRAFT. Fixes the diff-UI milestone (`doc/spec-diff.md`) shipping with
diff tabs open but rendering NO tint/coloring, and relocates the git-diff
refresh loop from the client app into the library so ANY embedding app gets
live diffs with zero opt-in code, per I25.

## Goals

After this change: opening a `LiveDiff` tab shows the actual diff overlay
(added/removed/modified row tints, phantom removed rows, word marks) exactly as
the syntax-and-diffs engine already renders for a correctly-fed document. And a
brand-new embedding app that does nothing beyond calling `EditorRuntime::create`
in a git working tree gets live git diffs automatically — no poll loop, no
`GitRepository`/`GitDiffSource` wiring, no per-app code at all. Existing
clients (`ssg_main.cpp`, the browser client) have their current hand-rolled
git-diff loops DELETED, not merely left alongside a new library path.

## Design

### Bug 1 — diff tint never renders (root cause, confirmed by direct read of
the code, not inferred)

`EditorRuntime::Impl::documentView()` (`src/runtime/snapshot.cpp:37-56`)
publishes, for the active `LiveDiff` tab, `publishedRevision =
diff.viewState().revision` — the `DiffModel`'s own monotonic scan-revision
counter. `DiffViewState::fileForDocument` (`src/DiffModel.cpp:325-337`) then
requires `document.revision == revision` (the SAME field name, two UNRELATED
counters) before it will match by identity at all. These two revisions have
never been the same kind of value — one is "how many diff scans have been
applied," the other is "how many edits has this document had" — so the
equality check was, in effect, permanently false for any document, and
`fileForDocument` silently returns `nullopt`. The renderer's diff-selection
call site (`src/Renderer.cpp:379`,
`snapshot.sections().diff.fileForDocument(snapshot.sections().document)`) then
skips its entire tint-painting block. No error, no log, no failing test caught
this because `tests/test_diff.cpp`'s `documentDiffLookupUsesIdentityAndRevision`
(line 542) asserts the CURRENT (broken) behavior as correct — a revision
mismatch is asserted to correctly return no match, encoding the bug as a
passing test.

**The fix is a CONTRACT change, not a value-alignment tweak.** There is no
value you can publish for "document revision" that will ever legitimately equal
`diff.viewState().revision` on an ongoing basis — they count different things.
The revision-equality check must be REMOVED from `fileForDocument`; a
`LiveDiff` document is selected by `diffFileIdentity` ALONE. This is safe
because of an invariant `doc/spec-diff.md` Step 5 already established and this
spec now makes load-bearing, stated PRECISELY (not as a blanket "never stale"
claim): `EditorRuntime::Impl::refreshLiveDiffDocuments`
(`src/EditorRuntime.cpp:849-887`) updates a `LiveDiff` document's content and
its `liveDiffDocuments` identity-tracking entry TOGETHER, as a single atomic
step, in the SAME command-executor-thread commit that advances the diff
revision — and (per Step 5's review-fold fix, `942c448`+`fa5fb12`) on a
PER-TAB refresh FAILURE it leaves the OLD document and its identity mapping
BOTH untouched (a create-before-remove swap order), never a state where
identity has advanced but content has not, or vice versa. So a `LiveDiff`
document is, at every observable moment, EITHER exactly current relative to
the `DiffViewState` that last successfully refreshed it, OR exactly as it was
after its last successful refresh (a stale-but-coherent pair) — content and
identity move together or not at all. There is consequently no window where
identity matches the current view but content reflects a DIFFERENT (older or
newer) view — which is precisely the property revision-equality was
(ineffectively) trying to guard, and identity-only matching preserves it
exactly, because that pairing is enforced at the WRITE site
(`refreshLiveDiffDocuments`), not at the read/select site (`fileForDocument`).
This is not a race-free claim for hypothetical FUTURE multi-writer scenarios
(e.g. concurrent multi-client editing of session state from different
threads) — it holds because this codebase's session mutation is already
single-threaded end to end (see Invariants below), a property this spec does
not change and the background-worker design (below) explicitly preserves.

**A second, related contract error must be fixed in the same change.** The
`else if` branch of `documentView()` (`src/runtime/snapshot.cpp:50-52`) ALSO
sets `diffFileIdentity` for an ordinary SAVED (non-`LiveDiff`) edit tab, using
its saved path as the identity. This is the vestige of the pre-`spec-diff.md`
design intent (inline diff overlay directly in the edit tab you are actively
typing in) that the user explicitly rejected during the `spec-diff.md` UX
interview ("we don't want the library to show diffs in every file that's
opened in git... it's okay to defer this"). Today this branch is harmless only
by the SAME bug (the revision check also always fails for it) — fixing bug 1
without also removing this branch would make ordinary edit tabs start showing
diff tints, which is a shipped regression against explicit user direction, not
a fix. **Remove the `else if` branch entirely**: `diffFileIdentity` is set ONLY
for `TabKind::LiveDiff` tabs. An ordinary edit tab never has diff-file
identity, never matches `fileForDocument`, never renders tint — by identity
absence, not by a coincidental revision mismatch.

### Bug 2 — git-diff refresh is 100% client-composed (confirmed I25 violation)

`apps/ssg_main.cpp:238-297` owns: the poll-vs-event mode selection (an env var
check), the 250ms poll interval, retry/backoff scheduling on a failed or
incomplete scan, the full-vs-path refresh decision, and the decision of when to
call `GitDiffSource::refresh`/`refreshPaths` and feed the result into
`EditorRuntime::applyGitDiffScan`. Per I25 and `doc/spec-git-diff-source.md`'s
own Design ("`GitDiffSource` — pull-driven library FEATURE/orchestrator... Host
— owns ONLY the loop: a timer for polling and/or a watcher poll thread... NO
feature logic"), every one of those decisions is FEATURE policy and belongs in
the library. Today it is duplicated per client — any embedding app (a future
GUI embedder, the browser client if it does not already share this code) would
have to reimplement the same ~60 lines to get diffs at all. This directly
contradicts this session's own established principle: "Applications are thin
user IO and responsiveness providers."

**Mechanism (this codebase already has the right precedent — reuse it, do not
invent new concurrency machinery).** `ScratchStore` (`src/ScratchStore.cpp`)
already owns an internal background `worker_` thread with a mutex/condvar-
guarded job queue: the library does slow/blocking I/O (durable scratch writes)
OFF the session-command-executor thread, and callers observe completion via
polled state (`durableGeneration_`), never a callback into session state from
the worker thread itself. Apply the SAME shape here:

- `EditorRuntime` (or a small library-owned `GitDiffRefreshWorker` alongside
  `GitDiffSource`) owns an internal worker thread when the workspace resolves
  to a git repository (`makePlatformGitRepository` succeeds). The worker thread
  runs the SAME poll/retry/mode logic currently duplicated in `ssg_main.cpp`,
  calling the (libgit2, blocking) `GitRepository`/`GitDiffSource::refresh`
  scan off the executor thread, and enqueues completed `GitDiffScan` results
  into a thread-safe queue.
- The session's OWN existing dispatch/tick path (the same place `primeDeferred`
  or the normal per-frame tick already runs on the executor thread) drains that
  queue and calls `applyGitDiffScan` — the mutation into `DiffModel`/
  `FollowEditsModel`/`TreeModel` still happens exclusively on the single
  command-executor thread, preserving every existing single-threaded-mutation
  invariant in this codebase. The worker thread NEVER touches runtime/session
  state directly, exactly like `ScratchStore`'s worker never touches
  `Workspace`/`Document` state directly.
- **CONCRETE drain + wake hook (settled, not deferred to implementation).**
  Every client already calls `EditorRuntime::snapshot(...)` unconditionally on
  every loop iteration (`apps/ssg_main.cpp:513-550`, the `refresh` lambda) —
  this is the correct, already-universal drain point: `snapshot()` drains the
  worker's queue and applies any pending `GitDiffScan` via the existing
  `applyGitDiffScan` path BEFORE assembling the returned snapshot, so any
  client that calls `snapshot()` sees current diffs, with no new API to learn.
  This alone is NOT sufficient, though: a client blocked in a blocking-read
  event loop (`ssg_main.cpp`'s `select()` with a timeout, waiting on terminal
  input) will not call `snapshot()` again until a key arrives, so a
  following-agent's fast edits would sit undrained until the human happens to
  press a key — defeating the follow-mode goal for an unattended/agent-only
  session. Per I25, the CLIENT still owns its own event-loop scheduling policy
  (spec.md I25's explicit carve-out: "does not disturb... scheduling"); the
  library's job is only to give it a narrow WAKE mechanism, exactly the shape
  `ssg_main.cpp` already uses for its SIGWINCH self-pipe (`apps/ssg_main.cpp`
  line ~183's comment references this exact existing pattern). Add
  `EditorRuntime::gitDiffWakeDescriptor()` (or an equivalent narrow,
  platform-appropriate notification primitive — a self-pipe/eventfd on POSIX)
  that the worker thread signals once when it enqueues a new scan; the client
  adds this descriptor to its EXISTING `select()`/poll set (one line, no
  policy) alongside its terminal-input fd, exactly as it already does for the
  signal self-pipe. A client with no such event loop (a synchronous in-process
  embedder that calls `dispatch`/`snapshot` on its own cadence) is unaffected —
  it already drains on every call and never blocks indefinitely on this
  library's behalf.
- Event mode (filesystem-watch-driven refresh) is folded into the SAME internal
  worker: it owns the `FilesystemWatcher` poll loop too (mirroring the existing
  watcher-ownership contract: "the session host owns the polling thread" — that
  "host" is now the library's own internal worker, not the client app, exactly
  as I25 intends: mechanism ownership is about WHO drives blocking I/O, not
  WHICH process/binary the code happens to link into).
- This is opt-OUT, not opt-in: any `EditorRuntimeConfig` that resolves to a git
  workspace gets live diffs automatically. `apps/ssg_main.cpp`'s entire
  `SSG_GIT_DIFF_MODE`/poll-timer/retry block (lines ~238-297 and its per-tick
  poll check) is DELETED, not kept as a fallback or made conditional — a client
  no longer has any code path to get this wrong or forget to wire it.
- A config knob MAY remain for tests/embedders that need to disable the
  background thread (e.g. deterministic single-threaded test harnesses that
  want to call `applyGitDiffScan` manually) — mirror how `deferEnrichment`
  already exists as an explicit, narrow opt-out flag on `EditorRuntimeConfig`
  for analogous test-determinism reasons. Default remains fully automatic.

## Invariants

- Diff content-consumer contract (`doc/features/workspace-live-diffs.md`) —
  `DiffModel` still never invokes Git; the background worker's blocking git
  reads happen entirely inside the already-injected `GitRepository` adapter.
- I5 (all-or-nothing) — the worker thread enqueues a complete `GitDiffScan`;
  the executor-thread drain still applies it through the EXISTING atomic
  `applyGitDiffScan` staged-commit path (already fixed for atomicity in
  `doc/spec-diff.md` Step 5's review fold) — no new partial-application path.
- Single-threaded session mutation (existing, implicit throughout this
  codebase, now made explicit for this feature) — `DiffModel`/
  `FollowEditsModel`/`TreeModel`/`TabManager` are mutated ONLY on the
  command-executor thread; the background worker thread's ONLY job is
  producing a `GitDiffScan` value and handing it across the queue.
- I25 — feature policy (mode selection, retry, poll interval, reconcile
  decision) is library-owned; only the raw libgit2 read (already behind
  `GitRepository`) and (if event mode needs it) the raw native filesystem
  event source (already behind `FilesystemWatcher`) are mechanism.
- Behavior preservation for NON-diff rendering — this change touches only the
  `diffFileIdentity`/revision fields of `DocumentViewState` and the diff
  refresh loop; syntax highlighting, selection, and all other document-view
  rendering paths are unaffected.

## Considerations

- The existing `tests/test_diff.cpp::documentDiffLookupUsesIdentityAndRevision`
  test asserts the bug as correct behavior and MUST be rewritten (not just
  patched to pass) to assert the new identity-only contract, including the case
  that used to be labeled "stale" (revision mismatch, identity match) — under
  the new contract this case must now MATCH, since revision is no longer part
  of the lookup key at all.
- Removing the `else if` branch in `documentView()` changes `diffFileIdentity`
  for ordinary saved-file edit tabs from "set to the saved path" to "unset."
  Audit every existing test asserting `diffFileIdentity` equals a saved path
  for a non-`LiveDiff` tab (several exist per a grep of `diffFileIdentity` in
  `tests/`) — those assertions must be updated to expect `std::nullopt`, since
  that behavior is the explicit fix, not a regression to paper over.
- The background worker's queue-drain point is SETTLED by the Design section's
  drain+wake hook above (`snapshot()` drains unconditionally; the wake
  descriptor covers clients blocked in their own event loop) — no further
  investigation needed at implementation time.
- `makePlatformGitRepository` failing (not a git repo) must result in NO
  worker thread started at all, not a thread that spins retrying forever
  against a permanently-absent repository (mirrors the existing git-diff-source
  spec's "not-a-repo is a permanent inert state, not an error loop").
- Thread lifecycle: the worker must stop cleanly on `EditorRuntime` destruction
  with no detached/leaked thread. `ScratchStore`'s shutdown discipline is the
  template for the STOP-SIGNALING pattern (a guarded stop flag + condvar
  notify + join). `FilesystemWatcher::poll` is already timeout-bounded; the
  libgit2 scan calls used here (`git_repository_open_ext`,
  `git_diff_tree_to_workdir_with_index`, related local object reads) do not
  expose a strict timeout boundary in this adapter path, so shutdown cannot
  truthfully be specified as "bounded by poll interval only." The honest
  contract is: stop is checked immediately before and immediately after each
  scan call, so the worker exits as soon as any in-flight local scan returns,
  with no extra sleep layered on top. In practice this design assumes local
  repository reads are fast (no network I/O), and lifecycle tests enforce a
  generous finite destruction bound for expected workloads.

## Risks and Mitigations

- A new background thread inside `EditorRuntime` could introduce data races if
  any drain-side code accidentally touches session state off the executor
  thread ⇒ enforce the `ScratchStore` pattern strictly: the worker thread's
  ONLY output is an immutable `GitDiffScan` value pushed onto a thread-safe
  queue; it must call NOTHING on `EditorRuntime`/`DiffModel`/`Workspace`
  directly.
- Deleting `ssg_main.cpp`'s loop entirely (rather than leaving it as a
  redundant fallback) could regress the TUI if the library-side timing differs
  ⇒ this is exactly why it must be verified end-to-end against the SAME
  temp-repo integration harness `doc/spec-git-diff-source.md` Step 6 already
  built (`tests/test_git_diff_host.cpp`), extended to prove NO application code
  is needed.
- The `documentDiffLookupUsesIdentityAndRevision` test rewrite could be done as
  a shallow "make it pass" edit rather than a genuine contract change ⇒ the
  Acceptance oracle below requires an END-TO-END render test (not just a
  `DiffModel`-level unit test) so a regression cannot hide behind a
  unit-level pass.

## Acceptance (Definition of Done)

- Observable: opening a `LiveDiff` tab in the TUI against a real git repo with
  staged add/modify/delete/rename shows the SAME tints/phantom-rows/word-marks
  the syntax-and-diffs engine already renders elsewhere; a fresh `ssg_main.cpp`
  build with ZERO diff-related code in `main()` still shows live diffs; visual
  signoff required before merge (user-visible UI fix).
- Gates: `bash scripts/check.sh` green with and without `SSG_TREESITTER`.
- Oracles:
  - render: an end-to-end runtime test opens a `LiveDiff` tab for a file with
    known hunks and asserts the published `CellGrid`/render output actually
    carries diff tints (not just that `fileForDocument` returns a value at the
    model layer) — this is the oracle that would have caught the original bug,
    which a model-only unit test did not.
  - identity-only contract: hand cases proving a `LiveDiff` document matches by
    identity regardless of the diff model's own revision counter, and that an
    ordinary saved-file edit tab NEVER carries `diffFileIdentity` (never
    matches, by construction, not by accident).
  - default-wiring: a headless temp-repo integration test drives
    `EditorRuntime::create` alone (no app-level git-diff code) in a repo with a
    staged change and asserts diffs appear via the normal snapshot path within
    a bounded time, in both poll and event configurations.
  - thread-safety/shutdown: a test creates and destroys `EditorRuntime` many
    times against a live repo with no leaked/detached thread and no race
    reported under the sanitizer build this project already runs in CI.

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Write the end-to-end render oracle FIRST (red before green) proving tints actually paint for a `LiveDiff` tab — must FAIL against current code, proving it would have caught the original bug | `tests/test_renderer_diff_overlay.cpp` or a new runtime-level render test | fails before step 2, passes after | render correctness |
| 2 | Fix `fileForDocument` to match by identity only; remove the edit-tab `diffFileIdentity` branch in `documentView()`; rewrite the now-wrong unit test | `src/DiffModel.cpp`, `src/runtime/snapshot.cpp`, `tests/test_diff.cpp`, any test asserting edit-tab `diffFileIdentity` | step 1's oracle now passes; rewritten hand cases; audit + fix every affected existing assertion | contract change, documented above |
| 3 | Library-owned background git-diff worker (mirrors `ScratchStore`'s worker/queue shape): starts automatically when `GitRepository` resolves, drains inside `EditorRuntime::snapshot()`, owns poll+event mode/retry/reconcile policy, exposes the wake descriptor | `include/ssg/EditorRuntime.h`, `src/EditorRuntime.cpp` (or new `GitDiffRefreshWorker.h/.cpp`), `cmake` | headless temp-repo test: `EditorRuntime::create` alone produces live diffs, both modes; thread lifecycle test (create/destroy loop, no leak, bounded shutdown latency) | I25; single-threaded session mutation |
| 4 | Delete `apps/ssg_main.cpp`'s entire manual git-diff loop; wire the new wake descriptor into its existing `select()` set (one line, mirroring the SIGWINCH self-pipe); confirm the browser client (if it has its own copy) is deleted too | `apps/ssg_main.cpp`, browser client sources if applicable | dual-gate green; manual smoke test against `/tmp/gittest`, including an unattended/no-keypress follow scenario | I25 |

## Rationale (skippable)

Both bugs share one root pattern: a genuinely library-owned FEATURE (diff
coherence matching, diff refresh orchestration) had its policy leak outward —
in bug 1, into an ad hoc revision-equality check that encoded a stale design
assumption instead of the actual invariant (`refreshLiveDiffDocuments`
synchronicity) that makes revision-checking unnecessary; in bug 2, into the
client application entirely. Both fixes are actually the SAME kind of move:
delete the place where policy leaked out, and make the library's own already-
correct machinery (Step 5's synchronous refresh; `ScratchStore`'s worker-thread
precedent) the single source of truth. Nothing here needs new concepts — it
needs the existing, already-built pieces connected the way I25 already
requires.
