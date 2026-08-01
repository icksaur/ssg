# spec-autosave-tabs

Status: DRAFT (spec only — do NOT implement; collaborate on open questions first)

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

### What already exists (build on it, do not reinvent)

- `ScratchStore` (`include/ssg/ScratchStore.h`) already persists unsaved
  document content + dirty flag to an append-only binary journal, keyed to the
  **canonical workspace root** (`ScratchStore::create(scratchRoot,
  canonicalWorkspace)`; scratchRoot defaults to `cwd/.ssg/scratch`,
  `src/EditorRuntime.cpp:1796`). Records: `JournalDocument{key, mode, dirty,
  utf8Content}` keyed by `JournalDocumentKey` (Saved workspace-relative path OR
  Untitled UUID). Crash-safe replay already discards a torn tail
  (`JournalReplayResult::discardedTail`).
- `ScratchStore::recovery()` returns the recovered `JournalRecoverySet`.
- `TabState` (`include/ssg/TabManager.h:46-58`) already carries everything a
  restored tab needs: `TabId`, `TabKind`, optional `FileDocumentId`,
  `JournalDocumentKey`, `contentIdentity`, label, `DocumentMode`, dirty,
  `TabRecoveryBadge`.
- Durability is already a background concern (`ScratchStore` fsync thread;
  `ScratchDurabilityState`), surfaced per-tab as `TabRecoveryBadge`.

### The two gaps this spec closes

1. **Drafts flush only on tab close** (`src/EditorRuntime.cpp:750-765`). A kill
   or crash between opening a file and closing its tab loses every edit since the
   last save/close. Autosave must flush a document's draft **while it is open**,
   debounced, so a hard kill preserves in-progress edits.

2. **No session manifest / no startup replay into tabs.** `recovery()` is never
   used to reopen tabs; tab order and the active tab are not persisted at all.
   Restore must reconstruct the tab set from persisted state.

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

For each editable tab key in the manifest:

- **Untitled** (no path): reopen an untitled buffer from the draft content, dirty
  = the persisted dirty flag.
- **Saved path, draft is dirty**: reopen the file, then apply the draft as the
  buffer content and mark dirty — the user's unsaved edits. If the on-disk file
  changed since the draft was captured (disk content != the draft's baseline),
  this is a **conflict** (see Considerations); do NOT silently discard either
  side.
- **Saved path, draft clean (or no draft)**: reopen the file from disk, not
  dirty. (Kate restores clean tabs too.)
- **Saved path, file now missing on disk**: reopen as an untitled/orphaned dirty
  buffer holding the draft content if any; otherwise drop the tab and note it.

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

- **Conflict: draft vs. changed disk file.** The draft was captured against some
  baseline; if the file on disk now differs (an external tool, git checkout, or
  another editor changed it), restoring must not clobber either. Needs a decided
  policy: keep the draft as the dirty buffer AND surface an external-modification
  state (SSG already has `ExternalModificationFlow` + `file.reload`), letting the
  user reload-and-lose-draft or keep-draft-and-overwrite-on-save. The draft must
  therefore record enough baseline identity to detect this (content hash or the
  saved mtime/size at capture). **Open question:** what baseline identity to
  store, and the default resolution.
- **Debounce interval & durability target.** Too eager = constant fsync churn on
  every keystroke; too lazy = a kill loses recent edits. `ScratchStoreConfig`
  already exposes `durabilityTarget{100ms}`. Needs a chosen idle-debounce (e.g.
  flush a document N ms after the last edit, and/or every M edits) — a policy
  decision, measured against the existing gate perf budget.
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

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Debounced draft flush for OPEN documents: a library `session.flush_drafts` dispatch (or runtime tick) that, on a dumb app heartbeat, decides which dirty open documents are idle-long-enough and writes each as a `JournalDocument` via `ScratchStore::updateDocument`; debounce state/decision lives in the library, cadence heartbeat in the app | src/EditorRuntime.cpp, src/runtime/files.cpp, include/ssg/EditorRuntime.h, apps/ssg_main.cpp | round-trip: edit → heartbeat → drop session → `recovery()` returns the edited content, dirty=true | I5, I25, single-thread |
| 2 | Baseline identity in the draft: capture a disk baseline (hash or mtime+size) with each saved-key draft so a later disk change is detectable | include/ssg/ScratchJournal.h, src/ScratchJournal.cpp, src/EditorRuntime.cpp | invariant: a draft flushed against baseline B, then disk changed, is classified conflict (not clean) | I5 |
| 3 | Session manifest: new `JournalSessionManifest` record (ordered editable-tab keys + active key); ALSO extend the compactor's checkpoint/recovery snapshot to carry it (`ScratchStore` compaction is document-only, `src/ScratchStore.cpp:213-229,361-375`) AND extend `ScratchSession::claimNewestRestorable()` (`src/ScratchSession.cpp:287-290`) to claim a manifest-only (clean) session; rewritten on tab open/close/reorder/activate | include/ssg/ScratchJournal.h, src/ScratchJournal.cpp, src/ScratchStore.cpp, src/ScratchSession.cpp, src/TabManager.cpp, src/EditorRuntime.cpp | manifest order/active round-trip; manifest survives a compaction pass; manifest-only clean session is claimed & restored; torn-manifest → empty | I5 |
| 4 | Restore-on-launch reconcile as a staged transaction: assemble the whole intended tab set (documents + drafts + active) off to the side from manifest + recovery set, classify conflict/missing per tab, commit in one step, roll back all partial opens on any failure; wire into runtime construction | src/EditorRuntime.cpp, src/runtime/files.cpp, include/ssg/EditorRuntime.h | full-session restore round-trip; mid-restore-failure leaves both on-disk and running session unchanged; missing-file; conflict; non-document-not-persisted; untitled-identity | I5, I25, non-destructive |
| 5 | Conflict UX: restored dirty tab whose disk changed surfaces external-modification state (reuse `ExternalModificationFlow`) with keep-draft / reload choice | src/ExternalModificationFlow.cpp, src/runtime/files.cpp | conflict oracle: both sides survive; user can resolve either way | non-destructive |
| 6 | Settings toggle to enable/disable session persistence + a discard-session action; docs in doc/config.md | include/ssg/Settings.h, src/Settings.cpp, src/runtime/*, doc/config.md | disabled → no manifest written, launch is a fresh session | I25 |
| 7 | Cross-instance safety: workspace session lock; a second instance in the same root runs without persistence and reports it | src/EditorRuntime.cpp, apps/ssg_main.cpp | two stores over one root never produce an unreplayable journal | I5 |

## Rationale (skippable)

The heavy lifting — crash-safe append-only journaling of unsaved content keyed by
workspace, per-tab durability badges, quota/compaction, tail-discard replay — is
already built and shipped for the close-time recovery feature. The Kate/Notepad++
behavior the user wants is mostly **wiring that machinery into two moments it does
not yet cover**: continuously while editing (not only on close), and at launch
(replaying into tabs, plus an ordered manifest the current design lacks). Framing
the work as "extend the one journal" rather than "add a session file" keeps a
single crash-safe source of truth and avoids a two-files-must-agree coupling — the
kind of implicit contract that rots. The genuinely hard, decision-bearing part is
not the persistence but the **conflict policy** when a draft and the disk file
disagree at restore; that is why it is called out as the primary risk and given
its own phase and oracle, and why several open questions are left for the user
rather than guessed.
