# spec-autosave-tabs

Status: DRAFT (spec only — do NOT implement; collaborate on open questions
first). **Layered on `doc/spec-single-file-draft.md`** — that spec is the
foundation and owns saved-file draft *content* + baselines in a central
abspath-keyed store; this spec owns only the per-workspace **session manifest**
(tab order + active tab) and **Untitled** drafts, and *consumes* saved-file draft
content from the central store rather than re-persisting it. See "Storage model"
below. Implement the single-file spec first.

## Goals

After this change, SSG behaves like Kate / Notepad++ (and modern Windows
Notepad): closing SSG — cleanly, killed, or crashed — and relaunching it in the
**same working directory** restores the prior working state:

- Every open editable tab returns: saved files, untitled buffers, tab order, and
  the active tab.
- In-progress **unsaved edits** return exactly as they were, with the dirty
  marker intact — the user never explicitly saved, yet nothing is lost.
- A file that was open and *clean* (no unsaved edits) reopens showing current
  disk content.
- Reopening is **non-destructive to disk**: restoring drafts writes nothing to
  the user's files; drafts live only in SSG's scratch area until the user saves.

Non-goals: cross-working-directory restore (each working directory has its own
independent session); restoring ephemeral non-document tabs (diff/search/tree);
multi-window/multi-client session sync; a global "recent files" list.

## Design

### Storage model — reconciled with spec-single-file-draft.md (read first)

`doc/spec-single-file-draft.md` re-homes **saved-file draft content + baselines**
into a single per-user **central store keyed by the file's canonical absolute
path** (not the workspace root). This spec does NOT re-persist that content; it
layers a per-workspace **session** on top. The division of responsibility is:

- **Central abspath-keyed store (owned by the single-file spec):** the draft
  content, dirty flag, and `{mtime, size, contentHash}` baseline for each *saved*
  file, addressed by file identity. A saved file has exactly one draft no matter
  which workspace or path opened it. Reopen classification (draft vs disk,
  conflict detection, the yellow notice) is the single-file spec's job and fires
  per file — this spec reuses it verbatim when a restored tab is a saved file.
- **Per-workspace session journal (owned by THIS spec):** the ordered
  **session manifest** (the editable-tab list + active tab) and **Untitled**
  (UUID-keyed) draft content. A session belongs to a launch / working directory,
  and an Untitled buffer has no file identity to key by, so both are inherently
  per-workspace. This journal is the append-only log this spec extends
  (manifest record, compaction, remnant-claim below).

Consequence for restore: for a **saved** tab, the manifest stores only the file's
**identity** (its canonical path / key), and the draft content + conflict
handling come from the central store via the single-file reopen path. For an
**Untitled** tab, both identity and content come from this per-workspace journal.
The two stores are addressed independently, so there is no "two files must agree
on the same content" coupling — the manifest references a file, it does not copy
its draft.

### What already exists (build on it, do not reinvent)

- `ScratchStore` (`include/ssg/ScratchStore.h`) already persists unsaved
  document content + dirty flag to an append-only binary journal. Today it is
  keyed to the **canonical workspace root** (`ScratchStore::create(scratchRoot,
  canonicalWorkspace)`); the single-file spec re-keys saved-file drafts to the
  central abspath store, leaving this per-workspace journal for the manifest and
  Untitled drafts. Records: `JournalDocument{key, mode, dirty, utf8Content}` keyed
  by `JournalDocumentKey` (Saved path OR Untitled UUID). Crash-safe replay already
  discards a torn tail (`JournalReplayResult::discardedTail`).
- `ScratchStore::recovery()` returns the recovered `JournalRecoverySet`.
- `TabState` (`include/ssg/TabManager.h:46-58`) already carries everything a
  restored tab needs: `TabId`, `TabKind`, optional `FileDocumentId`,
  `JournalDocumentKey`, `contentIdentity`, label, `DocumentMode`, dirty,
  `TabRecoveryBadge`.
- Durability is already a background concern (`ScratchStore` fsync thread;
  `ScratchDurabilityState`), surfaced per-tab as `TabRecoveryBadge`.

### The gap this spec closes

**No session manifest / no startup replay into tabs.** `recovery()` is never used
to reopen tabs; tab order and the active tab are not persisted at all. Restore
must reconstruct the tab set from persisted state. (The other former gap —
"drafts flush only on tab close" — is closed by `doc/spec-single-file-draft.md`'s
debounced open-document flush, which this spec relies on.)

### Ownership (library owns policy; app owns OS conventions — I25)

- **Library** owns: the draft-flush policy (which documents, when, debounce), the
  session manifest (the ordered tab list + active tab + per-tab identity), the
  reconcile-on-launch algorithm (draft vs disk), and all reads/writes of the
  scratch area. This is a `SessionRecovery` concern layered over `ScratchStore` +
  `TabManager`, living inside `EditorRuntime::Impl`.
- **App** owns: the wall-clock **timer** that emits a dumb, fixed-interval
  heartbeat tick (the app already owns the event loop and all timers,
  `apps/ssg_main.cpp`), and the resolution of the scratch/recovery **root paths**
  (OS convention). The library owns the debounce *decision*: on each heartbeat it
  decides which dirty documents are idle-long-enough to flush. The app never
  holds cadence policy — a heartbeat is not "flush now," it is "a moment passed."

### Session manifest (new persisted artifact)

A per-working-directory ordered record of the editable tabs, so order and the
active tab survive. Mechanism chosen: **extend the existing journal** with a new
checkpoint record kind (`JournalSessionManifest`) rather than introduce a second
file, so one append-only log with one crash-safe replay path remains the single
source of truth (no two-files-must-agree coupling). The manifest holds, in tab
order: the `JournalDocumentKey` of each editable tab and the active tab's key. It
is rewritten (append + periodic compaction, both already supported) whenever the
tab set or order changes.

Two existing seams must extend to carry the manifest, or it is silently lost
(review MUSTs):

- **Compaction is document-only.** `ScratchStore` compaction rewrites the journal
  from an in-memory `JournalRecoverySet` snapshot (`src/ScratchStore.cpp:213-229,
  361-375`), and that recovery state models documents only
  (`include/ssg/ScratchJournal.h:75-80`). The manifest must be added to the
  checkpoint/recovery state the compactor snapshots, or a compaction pass drops
  the session manifest. This is a required change to `JournalRecoverySet` (or a
  sibling checkpoint field) and the compaction path, not an additive record kind
  alone.
- **Remnant-claim is dirty-only.** A restorable prior session is claimed today by
  `ScratchSession::claimNewestRestorable()`, which only returns a remnant when
  `replay().recovery.documents` is non-empty (`src/ScratchSession.cpp:287-290`).
  A **manifest-only** session (all tabs clean, no dirty drafts — which the Goals
  require to restore) would be skipped and never restored. The claim criteria
  must also treat a present manifest as restorable.

Only `TabKind::Document` tabs persist. Diff/search/tree/read-only tabs are
derived views and are recreated on demand, never restored.

### Restore-on-launch reconcile (per persisted tab, in manifest order)

The manifest stores each editable tab's **identity** (its `JournalDocumentKey` /
canonical path), not its draft content. For each tab, in manifest order:

- **Untitled** (no path): reopen an untitled buffer from the draft content held in
  **this spec's per-workspace journal** (keyed by the tab's UUID), dirty = the
  persisted dirty flag.
- **Saved path**: hand the file identity to the **single-file reopen path**
  (`doc/spec-single-file-draft.md`), which loads the central-store draft (if any),
  compares its baseline to disk, and resolves clean / restored-dirty / conflict /
  missing-file exactly as it does for a normal file open. This spec does NOT
  re-implement draft-vs-disk logic or copy draft content; it only asks the
  single-file path to open the tab, then places it at the manifest's position.
  A saved tab with no central draft reopens clean from disk (Kate restores clean
  tabs too); a dirty central draft returns as the user's unsaved edits with the
  same conflict handling (yellow notice) the single-file spec defines.

Restore is a single atomic assembly (I5): a failed reconcile of one tab must not
leave a half-restored session; the previous persisted state stays intact until
the new tab set is committed.

**No staged-batch commit exists today.** The existing seams are per-operation
mutators (`Workspace::openFile`/`newDocument`, `TabManager::openDocument`/
`activate`) with no assemble-then-commit boundary (review MUST). This spec must
therefore design a concrete restore transaction: build the whole intended tab set
off to the side (open documents + drafts + active selection), and commit it in
one step, rolling back every partially-opened document if any tab in the batch
fails — the persisted manifest/journal is not mutated until commit succeeds. The
oracle (Acceptance) must inject a mid-restore failure and prove the on-disk
session and the running session are both unchanged.

### Working-directory identity & concurrency

- A "working directory" is the canonical workspace root (already the store key).
  Two SSG instances launched in the *same* root would share one scratch store —
  **out of scope for this spec** and must be handled safely: detect a live
  session (lock file / PID liveness) and either refuse restore or fall back to a
  fresh session, rather than corrupt the journal. Chosen minimum: a workspace
  session lock; second instance runs without session persistence and says so.
  (Open question — see Considerations.)

## Invariants

- **I25 (feature-not-mechanism boundary):** the library owns persistence policy
  and all scratch I/O; the app injects only the timer tick and the OS-resolved
  root paths. The app must not decide what/when to flush or how to reconcile.
- **I5 (atomic reconcile):** draft flush and session restore each publish a whole
  consistent state or nothing; no half-applied tab set, no partially-written
  manifest observed as authoritative (append-only + tail-discard already gives
  this for records; restore must assemble-then-commit).
- **Single-threaded session mutation:** flush and restore run on the command
  thread via dispatch; background durability stays behind the existing fsync
  thread + wake-fd, never mutating session state directly.
- **Non-destructive restore:** restoring drafts performs no write to the user's
  files; only the scratch area is written, only disk files are read.

## Considerations

- **Conflict: draft vs. changed disk file.** Owned by
  `doc/spec-single-file-draft.md` and reused verbatim — restore does not define
  its own conflict policy. That spec settled it: keep the draft as a dirty buffer,
  detect external change via the stored `{mtime, size, contentHash}` baseline
  (content hash is the authority), and surface a non-modal reserved-row notice
  offering diff / use-disk / dismiss. Restore simply routes each saved tab through
  that path, so a restored conflict looks identical to reopening the file
  directly.
- **Debounce interval & durability target.** Owned by
  `doc/spec-single-file-draft.md`: configurable seconds (default 10), dirty-only,
  eager first flush, plus tab-close and clean-exit. This spec inherits the same
  cadence for the documents it restores; it adds only manifest rewrites on tab
  open/close/reorder/activate.
- **Quota / eviction.** `ScratchStoreConfig` already caps bytes (256 MiB) and age
  (30 days) with `applyQuotas()`. Session manifests and long-lived drafts must
  fit this; eviction of an old working directory's session is acceptable and
  already modeled (`evictedSessionIds`).
- **Untitled buffer identity across restarts.** Untitled keys are UUIDs already
  persisted in the journal; restore must reuse them so a re-saved untitled buffer
  reconciles to the same draft, not a duplicate.
- **Same file in two working directories.** Each root has its own store and
  manifest, so the same absolute file can be open (with different drafts) under
  two roots. Saving writes the one disk file; the *other* root's draft then
  becomes a conflict on its next restore — same conflict path as above.
- **Files opened by absolute path outside the workspace root.** Out of scope: the
  current `Workspace` contract already rejects absolute/out-of-workspace paths
  (`src/Workspace.cpp:328-349`), so no such tab can exist to persist. Persisting
  out-of-root tabs would require a prior, separate Workspace-contract change and
  is explicitly not part of this spec.
- **"Clean quit" semantics.** Kate restores open tabs even when everything is
  saved. So the manifest persists the tab set independent of dirtiness; a clean
  tab restores from disk. Confirm this is desired (vs. only restoring when there
  are unsaved edits).
- **Interaction with `--` / file arguments on launch.** If the user launches SSG
  with an explicit file argument in a directory that also has a restorable
  session, define precedence (restore session AND open the argument? argument
  suppresses restore?). **Open question.**
- **Disabling / opting out.** A settings toggle (SettingsModel already persists
  to `.ssg/settings.json`) to turn session persistence off, and a way to discard
  a corrupt/unwanted session without hand-deleting scratch files.

## Risks and Mitigations

- **Data loss on the conflict path** — the whole point is to never lose edits.
  Mitigation: never auto-discard a draft; on any ambiguity keep the draft as a
  dirty buffer and require an explicit user resolution. Oracle: a round-trip test
  (edit → simulated kill → relaunch → identical buffer + dirty) and a
  disk-changed-under-draft test asserting both sides survive.
- **Journal growth / churn** — continuous autosave multiplies writes. Mitigation:
  debounce + compaction (both already present); a perf oracle bounding flush cost
  per edit under the gate budget.
- **Corrupt/partial manifest bricks startup** — a bad session record must never
  prevent launch. Mitigation: reuse the existing tail-discard replay; an
  unparseable manifest degrades to "no restore," never a crash. Oracle: replay of
  a truncated/garbage manifest yields an empty session, launch proceeds.
- **Cross-instance corruption** — two SSGs in one root. Mitigation: workspace
  session lock; second instance disables persistence. Oracle: two stores over one
  root do not interleave into an unreplayable journal.
- **Scope creep** — this is large. Mitigation: land autosave-of-open-drafts first
  (smallest crash-safety win, reuses the close path), then the manifest +
  restore, then conflict UX; each phase independently gated.

## Acceptance (Definition of Done)

- Observable (needs user signoff — user-visible behavior): in a scratch working
  directory, open two files, edit one without saving, create one untitled buffer
  with text, reorder tabs, `kill -9` the process, relaunch in the same directory
  → all three tabs return in order, the active tab is preserved, the unsaved
  edits and untitled text are intact and marked dirty, and no user file on disk
  was modified.
- Budgets: per-edit autosave cost stays within the existing gate perf envelope
  (no visible input latency); journal size stays under `ScratchStoreConfig`
  quota. (Set concrete numbers during planning.)
- Gates: `bash scripts/check.sh` green (all tests, 0 warnings), both
  `SSG_TREESITTER` ON and OFF.
- Oracles (each pins a behavior; write before its code):
  - Round-trip: edit → flush → simulated crash (drop the in-memory session) →
    replay → restored buffers byte-identical, dirty flags preserved.
  - Manifest order/active: persist N tabs in an order with a non-first active,
    replay → identical order and active tab.
  - Manifest survives compaction: after a compaction pass the session manifest is
    still replayable (guards the document-only compactor gap).
  - Manifest-only (clean) session is claimed: a session with tabs but no dirty
    drafts is treated as restorable, not skipped.
  - Atomic restore: an injected mid-restore failure leaves both the on-disk
    session and the running session unchanged (no half-restored tab set).
  - Conflict: draft captured, disk file changed underneath, restore → draft
    survives as dirty AND external-modification state is observable (nothing
    silently lost).
  - Missing-file: saved tab whose file was deleted → draft survives as an
    orphaned dirty buffer (or dropped-with-note per decided policy).
  - Torn manifest: truncated/garbage session record → empty restore, launch
    proceeds (no crash).
  - Non-document tabs (diff/search/tree) are not persisted.
  - Untitled identity: an untitled buffer keeps its UUID across restart; saving
    it reconciles the same draft (no duplicate).

## Plan

Ordered, independently-checkable. (Phases; each gated separately. Files listed
are the expected touch set — confirm during implementation.)

**Prerequisite (owned by `doc/spec-single-file-draft.md`, not repeated here):**
the central abspath-keyed draft store, the `{mtime, size, contentHash}` baseline,
the debounced/eager open-document flush, the reopen classification, and the
conflict notice + `draft.diff`/`draft.discard`. This spec's phases assume those
exist and build the per-workspace session layer on top.

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Session manifest: new `JournalSessionManifest` record (ordered editable-tab keys/identities + active key) in the **per-workspace** journal; ALSO extend the compactor's checkpoint/recovery snapshot to carry it (`ScratchStore` compaction is document-only, `src/ScratchStore.cpp:213-229,361-375`) AND extend `ScratchSession::claimNewestRestorable()` (`src/ScratchSession.cpp:287-290`) to claim a manifest-only (clean) session; rewritten on tab open/close/reorder/activate | include/ssg/ScratchJournal.h, src/ScratchJournal.cpp, src/ScratchStore.cpp, src/ScratchSession.cpp, src/TabManager.cpp, src/EditorRuntime.cpp | manifest order/active round-trip; manifest survives a compaction pass; manifest-only clean session is claimed & restored; torn-manifest → empty | I5 |
| 2 | Restore-on-launch reconcile as a staged transaction: assemble the whole intended tab set off to the side — Untitled tabs from the per-workspace journal, **saved tabs via the single-file reopen path** (central-store draft + baseline/conflict) — set active, commit in one step, roll back all partial opens on any failure; wire into runtime construction | src/EditorRuntime.cpp, src/runtime/files.cpp, include/ssg/EditorRuntime.h | full-session restore round-trip; mid-restore-failure leaves both on-disk and running session unchanged; missing-file; conflict routes through the single-file notice; non-document-not-persisted; untitled-identity | I5, I25, non-destructive |
| 3 | Settings toggle to enable/disable session persistence + a discard-session action; docs in doc/config.md | include/ssg/Settings.h, src/Settings.cpp, src/runtime/*, doc/config.md | disabled → no manifest written, launch is a fresh session | I25 |
| 4 | Cross-instance safety: workspace session lock; a second instance in the same root runs without session persistence and reports it | src/EditorRuntime.cpp, apps/ssg_main.cpp | two sessions over one root never produce an unreplayable manifest journal | I5 |

(The former Phases 1–2 — open-document flush and baseline identity — and the
conflict-UX phase have moved to `doc/spec-single-file-draft.md`, which this spec
now consumes.)

## Rationale (skippable)

The heavy lifting — crash-safe append-only journaling, per-tab durability badges,
quota/compaction, tail-discard replay — is already built, and the per-file draft
content, baseline, and conflict UX now live in `doc/spec-single-file-draft.md`.
The Kate/Notepad++ behavior the user wants is, on top of that, mostly **an ordered
per-workspace session manifest and a launch-time replay into tabs** — the piece
the current design lacks. Framing saved-file draft content as file-identity-keyed
(central store) and the session as workspace-keyed (this spec) keeps each a single
source of truth and avoids a two-files-must-agree coupling: the manifest
references a file, it never copies the file's draft. The genuinely hard,
decision-bearing part — the conflict policy when a draft and the disk file
disagree — was resolved in the single-file spec; restore simply routes each saved
tab through that path, so a restored conflict is identical to reopening the file
directly.
