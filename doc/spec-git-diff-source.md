# spec-git-diff-source

Status: DRAFT — architecture + source only. Defines the LIBRARY-owned component
that reads Git and feeds diffs into `DiffModel`. No UI, rendering, follow-reveal,
tab, or presentation work here (those follow in a later spec once the source is
settled).

## Goals

After this change the library can, on its own, keep `DiffModel` populated with the
current Git working-tree diff of the workspace, with no client involvement:

- Git is the ONLY source of workspace diff overlays. Every `DiffFileView` shown
  for a workspace file originates from a Git baseline-vs-working-tree comparison.
- Two refresh modes over the same computation: (1) POLLING — a periodic tick
  re-reads the changed set; (2) EVENT-DRIVEN — filesystem-watch events name the
  changed paths and the source recomputes just those, so the editor can follow an
  agent editing files quickly without a poll interval of latency.
- `.gitignore` and Git's own change detection are honored for free (build
  artifacts, ignored files never appear as diffs).
- Committing, checking out, or staging (any HEAD/index move) rebaselines: stale
  diffs are recomputed against the new baseline, and files that returned to clean
  are dropped.

Explicit non-goals (deferred to the later UI/wiring spec, NOT this one):
rendering the overlay, phantom rows, follow-reveal viewport offsets, next/prev
hunk caret, tabs, and the document-edit-revision-vs-diff-revision coherence at
`DiffViewState::fileForDocument`. This spec only establishes the source and the
revision/identity contract those later fixes depend on.

## Design

### Layers (who owns what)

- `DiffModel` (EXISTING, unchanged sink) — the diff ENGINE. Consumes content +
  identity via `updateGitFile`, computes hunks/changed-lines/word-ranges, produces
  `DiffViewState`. It never invokes Git or a shell (workspace-live-diffs.md
  contract). This spec adds no diff math; it feeds this model.
- `GitRepository` (NEW, injected adapter — the narrow mechanism) — the ONLY thing
  that touches a real repository. A library-owned interface with a platform impl,
  exactly the `FilesystemWatcher` / LSP-stream template (spec.md I25). It supplies
  raw content + identity, no feature logic:
  - enumerate the current changed set (tracked-modified, staged, untracked,
    deleted, renamed) relative to the configured baseline, honoring `.gitignore`;
  - for each changed path, the baseline blob content (absent ⇒ untracked/added),
    the working-tree content (absent ⇒ deleted), and the prior path (renames);
  - a `baselineIdentity` token that changes whenever HEAD or the index moves.
  It owns no thread and performs no diffing.
- `GitDiffSource` (NEW, library FEATURE/orchestrator) — owns the current
  changed-set, the monotonic diff `Revision` counter, and the last baseline
  identity. It is PULL-DRIVEN (owns no thread, matching the watcher contract): the
  host drives it. It turns adapter scans into `DiffModel` mutations: upsert every
  currently-changed file via `updateGitFile`, and remove entries that are no
  longer changed. Both refresh modes call the same reconcile logic.
- Host / composition root (app or session builder) — owns ONLY the loop: a
  timer for polling and/or a `FilesystemWatcher` poll thread whose normalized
  events it hands to the source, then posts the resulting batch onto the session
  command executor. No feature logic; identical ownership model to the existing
  watcher contract ("the session host owns the polling thread and posts returned
  batches to the session command executor").

### Data flow

Poll mode:
`host timer tick → source.refresh(repo.scan()) → reconcile against last set →
EditorRuntime.applyGitDiffScan(batch) → DiffModel.updateGitFile(...) + removals`.

Event mode:
`FilesystemWatcher.poll() → normalized events (paths) → source.refreshPaths(
repo.scanPaths(paths)) → reconcile just those paths → applyGitDiffScan(batch)`.

Both produce the same batch shape and go through the same runtime injection point.
Event mode is an optimization of WHICH paths to re-read, never a different diff.

### Reconcile semantics (the core of `GitDiffSource`)

A scan yields the authoritative changed set (full scan) or a targeted subset
(path scan). Against the source's last-known set:

- present in scan, changed ⇒ `updateGitFile` (upsert) with a fresh revision.
- present in a FULL scan's baseline identity but absent from its file list ⇒
  the file returned to clean / was committed ⇒ REMOVE from `DiffModel`.
- in a PATH scan, a named path absent from the returned files ⇒ that path is now
  clean ⇒ remove just that path. Unnamed paths are untouched (a path scan is not
  authoritative over the whole set).
- baseline identity changed since the last scan ⇒ a full rebaseline is required
  (a path scan alone cannot be trusted across a HEAD/index move); the source
  requests a full scan and reconciles the entire set.

Removal needs a `DiffModel` capability that does not exist yet (today it only
upserts). This spec adds a typed remove/clear on `DiffModel` (see Plan) so
returning-to-clean is expressible without rebuilding the model.

### Revision + identity contract (load-bearing)

- Every batch fed to `DiffModel` carries a strictly increasing diff `Revision`
  owned by `GitDiffSource` (DiffModel already rejects stale/equal revisions). This
  diff revision is a SEPARATE counter from document edit-revisions; the two are
  never compared as equals. (The existing `fileForDocument` bug that compares them
  is a RENDERING concern, deferred — but this spec fixes the contract it needs:
  the source guarantees a stable `DiffFileId` = the workspace-relative path, so the
  later UI selects a file's diff by IDENTITY, not by revision equality.)
- `baselineIdentity` tags every file with the baseline it was computed against.
  On any HEAD/index move the identity changes and the whole set is recomputed, so
  a consumer can reject diffs computed for an obsolete baseline (I5 atomicity: a
  publication is all-or-nothing).
- A scan the adapter reports as INCOMPLETE or FAILED (git error, mid-rebase,
  bounded-work exceeded) publishes NOTHING and is retried at a finite backoff —
  never a partial set presented as authoritative (mirrors the watcher's
  incomplete-rescan policy). Not-a-repository is a permanent inert state, not an
  error loop.

### Relationship to the existing non-Git path

`DiffModel`'s `seedNonGit` / `applyNonGitEvent` path and `ExternalModificationFlow`
stay, but their ROLE is clarified: they are the open-buffer-vs-disk CONFLICT
mechanism (a dirty buffer whose file changed underneath it), which is inherently
not a Git comparison. They are NOT a workspace diff-overlay source. The filesystem
watcher, previously imagined as a "non-Git diff producer," is repurposed here to a
CHANGE-NOTIFICATION TRIGGER for the Git source. No workspace overlay is ever
produced from the acknowledged-content baseline; the non-Git entry family is kept
strictly for open-buffer conflict (Decision 1).

### Runtime injection API

Add `EditorRuntime::applyGitDiffScan(GitDiffScan scan)` mirroring the existing
`applyExternalDiffBurst`: it stages `DiffModel::updateGitFile`/removals for the
batch and drives the follow MODEL (newest-hunk target per changed file) using the
existing burst logic. The follow-REVEAL viewport math and any rendering are out of
scope. The client never calls this; only the host loop does.

## Invariants

- I25 — the source is a library feature; only the raw repository read is a narrow
  injected adapter (`GitRepository`), and only the poll/watch thread is host-owned.
- I5 — a scan publication is all-or-nothing; partial/failed scans mutate nothing.
- I3 — revision-ordered mutation; stale/equal diff revisions are rejected.
- DiffModel content-consumer contract (workspace-live-diffs.md) — the model never
  invokes Git; all Git access is behind `GitRepository`.

## Considerations

- Baseline choice (DECIDED): baseline = HEAD — the diff is the working tree
  (staged + unstaged) against the last commit, so an agent's full divergence from
  HEAD is visible. Deeper history/range baselines may come with a later Git
  integration; keep the baseline behind `GitDiffConfig` so that is an additive
  change, not a skeleton change. The `GitDiffFile.index*` field names are renamed
  to `baseline*` (Plan step 1) to stop implying "index only".
- Adapter mechanism (DECIDED): libgit2, vendored and in-process, from the start.
  Rationale: a single way to touch a repository behind `GitRepository` — shelling
  to `git` on both Linux and Windows first and migrating to libgit2 later would be
  a rewrite, and the event-driven fast-follow path must not spawn a subprocess per
  event. Neither shell nor lib bleeds into the ssg library either way (all Git
  access is behind the one adapter), so we pick the endpoint mechanism up front.
  The `GitRepository` interface stays mechanism-agnostic so this remains a fact,
  not a pinned skeleton (a future in-tree alternative could still swap in).
- Targeted (path) scans must map watcher paths to Git status correctly, including
  renames (a rename appears as a delete+create at the filesystem layer but as a
  single rename in Git) — the adapter resolves this; the source consumes Git's
  view, not the raw filesystem pairing.
- Event debounce/coalescing is already owned by the `FilesystemWatcher`
  normalizer; the source does not re-debounce, it just consumes normalized paths.
- Bounded work: a huge changed set (e.g. a branch switch touching thousands of
  files) must respect `DiffConfig` limits and the adapter's own bound; over-limit
  ⇒ incomplete ⇒ suppressed + retried, never a partial overlay.

## Risks and Mitigations

- Subprocess latency defeating fast-follow ⇒ recommend in-process libgit2 for the
  event path; keep shell-git for poll only.
- Rebaseline storms (rapid commits by an agent) ⇒ identity-change collapses to one
  full rescan at backoff; revision ordering discards superseded work.
- Path-scan non-authoritativeness ⇒ a path scan never removes files it did not
  name; only full scans reconcile the whole set; identity change forces a full
  scan.
- Partial/failed git read presented as truth ⇒ incomplete scans publish nothing.

## Acceptance (Definition of Done)

- Observable (source-level, no UI): in a temp Git repo, editing/creating/deleting/
  staging/committing/checking-out files drives `DiffModel` to exactly the Git
  working-tree diff set, in both poll and event modes, with correct removals on
  return-to-clean and correct rebaseline on HEAD/index moves. Demonstrated by a
  headless harness that prints the resulting `DiffViewState`, not a rendered UI.
- Budgets: event-mode recompute touches only the named paths; no full scan except
  on identity change or overflow. Failed/incomplete scans allocate no partial
  state.
- Gates: `bash scripts/check.sh` green with and without `SSG_TREESITTER`. libgit2
  is vendored; the source degrades to an inert no-adapter state when no repository
  is present (not-a-repo), and that inert path is covered without a repo.
- Oracles: each below is an independent ground truth NOT going through the source.

## Plan

Ordered; each step green before the next. Codec/round-trip work lands in the step
that changes a shipped state shape.

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Add typed removal to `DiffModel` (`removeFile(id, revision)` / clear-to-set) and rename `GitDiffFile.index*`→`baseline*`; update existing Git tests | `include/ssg/DiffModel.h`, `src/DiffModel.cpp`, `tests/test_diff.cpp` | hand cases: upsert then remove yields empty view; stale/equal revision on remove rejected (I3); rename detected | I3, I5 |
| 2 | Define the `GitRepository` injected interface + `GitDiffScan`/`GitWorkingTreeScan` types (full scan + path scan + baselineIdentity + complete flag), and `GitDiffConfig` (baseline = HEAD default, bounds) | `include/ssg/GitDiffSource.h` | interface-only; compiles; no impl yet | I25 |
| 3 | Implement `GitDiffSource` reconcile (pull-driven, revision + identity contract, full/path/rebaseline/incomplete cases) against an INJECTED fake `GitRepository` | `include/ssg/GitDiffSource.h`, `src/GitDiffSource.cpp`, `tests/test_git_diff_source.cpp`, `cmake/components/git-diff-source.cmake` | reference reconcile: independently-computed changed-set vs source-driven `DiffModel` view over scripted scans (present/removed/rename/identity-change/incomplete) — fails before impl | I3, I5, I25 |
| 4 | Platform `GitRepository` impl on libgit2 (vendored, in-process) behind `makePlatformGitRepository`; not-a-repo ⇒ inert | `src/platform/git_repository.cpp`, `vendor/libgit2`, `cmake/components/git-diff-source.cmake` + `CMakeLists.txt`, `tests/test_git_repository.cpp` | temp-repo ground truth: real `git` CLI status/diff vs adapter output over create/modify/delete/stage/commit/checkout/rename/ignored | I25 |
| 5 | Runtime injection `EditorRuntime::applyGitDiffScan` (feed updateGitFile + removals + follow model via existing burst logic); no reveal/render | `include/ssg/EditorRuntime.h`, `src/EditorRuntime.cpp`, `src/runtime/editor_runtime_internal.h`, runtime tests | runtime test: a scan batch updates `DiffViewState` + follow target; stale batch rejected | I3, I5 |
| 6 | Host wiring: composition owns a poll timer and/or `FilesystemWatcher` thread that drives the source and posts batches to the executor; poll + event modes selectable | `src/EditorSessionBuilder.cpp` (or app composition), integration test | headless temp-repo harness: edits ⇒ correct `DiffViewState` in both modes (the Observable acceptance) | I25 |

## Decisions (settled)

1. Non-Git overlay path (`DiffModel::seedNonGit`/`applyNonGitEvent`) is KEPT
   strictly for `ExternalModificationFlow` open-buffer-vs-disk conflict; it is NOT
   a workspace diff-overlay source. Git is the sole workspace overlay source.
2. Baseline = HEAD (staged + unstaged vs last commit), behind `GitDiffConfig`;
   deeper history baselines are a later additive change.
3. Adapter = libgit2, vendored and in-process, from the start (single mechanism
   behind `GitRepository`).
4. Scan scope = whole-workspace changed set (the UI later decides what to show);
   narrowing to open/active files is a later optimization.

## Rationale (skippable)

The diff ENGINE (`DiffModel`), the follow MODEL, rendering, tints, and phantom
rows are all built and green, but every one has only ever been fed by tests: no
production code reads Git or drives a watcher. The missing piece is a SOURCE.
Making Git the sole source is not just a feature choice — it hands the library
`.gitignore` filtering, real baselines, rename detection, and commit/checkout
awareness for free, none of which a raw filesystem-vs-acknowledged-content baseline
can provide. The only genuinely platform-bound part is reading the repository,
which is exactly one narrow injected adapter (I25). Everything else — when to
re-read, how to reconcile, how to tag revisions, how to drive follow — is portable
feature logic that belongs in the library and would otherwise be reimplemented by
every client. Polling and event-driven refresh are two triggers over one
computation; the event path exists so the editor can track an agent's edits at
agent speed rather than poll-interval speed.
