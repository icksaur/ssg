# spec-central-session-recovery

## Goals
SSG stores session recovery outside edited workspaces, removes state that no
longer represents unsaved work, and restores recent dirty tabs after normal
exit, terminal loss, SSH disconnect, logout, or process failure. Recovery never
changes a newer disk file and never depends on unsafe work inside a signal
handler.

## Design
Session snapshots move beneath the configured user-state root. A deterministic
key derived from the canonical process-starting directory preserves the current
“launch from the same directory” identity without creating workspace files.
The snapshot format carries that canonical identity so a key collision or
misplaced file is rejected rather than restored into another workspace.

The existing atomic whole-session snapshot remains the persistence mechanism.
The editor captures a consistent immutable snapshot while holding its operation
lock, then releases the lock before encoding and filesystem I/O. It checkpoints
a changed dirty-session generation after `kSessionCheckpointDelay`, and
`kSessionCheckpointInterval` forces progress during continuous editing. One
serialized persistence queue orders writes and deletions by generation, so an
older write cannot resurrect state removed by a later save, close, or discard.
Normal shutdown joins pending persistence and performs a final synchronous
checkpoint. A termination notification requests the same final checkpoint on a
best-effort basis; platform shutdown deadlines and fatal or uncatchable
termination recover from the latest completed periodic checkpoint. Signal
handlers never perform persistence work. A clean session removes its snapshot
and then prunes only empty SSG-owned directories.

Existing project-local snapshots are a migration input. The codec continues to
read identity-free `kLegacySessionSnapshotVersion` data and writes
`kSessionSnapshotVersion` data carrying the canonical identity. Startup always
prefers a valid central snapshot. When none exists, it validates and imports
`./.ssg/session.snapshot`. A valid central snapshot retires any validated legacy
snapshot so clean central deletion cannot cause a later re-import. Unknown or
corrupt legacy files remain untouched when central state already takes
precedence. Deletion occurs only after equivalent central data is durably
written, and later cleanup prunes the legacy directory only when empty. Corrupt
data selected for import remains untouched and produces the existing actionable
startup error.

The PID-scoped recovery tree remains separate process scratch. It is created
exclusively beneath a private user-owned temporary parent with a unique
non-predictable name, without following existing links. An owning lifetime
removes the verified tree after normal shutdown. Independent later housekeeping
removes only unlocked, positively identified stale SSG roots; it never infers
staleness from PID reuse alone.

## Invariants
- SESSION-IDENTITY: recovery state is selected by the canonical
  process-starting directory and validates that identity before restoring.
  State at the session-path and snapshot-read APIs.
- SESSION-NONCLOBBER: restoring a draft never overwrites disk content and never
  binds it to a path whose decoded text differs from the draft when bytes have
  diverged from its recorded baseline. Byte changes that decode to the draft
  restore clean, preserving the existing reconciliation rule. State at
  `Editor::restoreSession`.
- SESSION-ATOMIC: interruption leaves either the prior complete checkpoint or
  the new complete checkpoint. State at `writeSessionSnapshot`.
- SESSION-SIGNAL-SAFE: signal handlers only notify the main event loop; encoding,
  allocation, locking, and filesystem I/O happen in ordinary runtime code.
  State at the platform termination interface.
- SESSION-CLEAN: no snapshot exists when no dirty editable tab requires
  recovery; cleanup removes only files and empty directories owned by SSG.
  State at the snapshot persistence API.
- SESSION-PRIVATE: central session and process-scratch directories are
  owner-only. State at their creation sites.
- RECOVERY-BOUNDED: under healthy storage, checkpoint publication progresses
  during continuous editing and an abnormal exit recovers the latest completed
  checkpoint. State at the editor checkpoint scheduler.

## Considerations
- Preserve tab order, active-tab selection, backing kinds, baselines, encoding
  conflict handling, and strict codec validation.
- A failed checkpoint leaves the prior snapshot recoverable and reports the
  failure without marking the dirty generation persisted; later scheduling
  retries it.
- A checkpoint racing further edits records its captured generation; the newer
  generation remains pending.
- Persistence operations publish in generation order. Shutdown joins the worker
  before deciding whether a final write or deletion is required.
- Multiple processes using one session identity retain the existing
  last-completed-writer behavior; the snapshot remains atomic but sessions are
  not merged.
- Central path keys must be stable across runs and platforms, filesystem-safe,
  and covered by fixed-vector tests independent of the implementation.
- Normal termination notifications include hangup and logout paths. Uncatchable
  termination and machine loss recover from the latest completed checkpoint.
- Startup removes only orphan atomic-replacement files proven not to belong to
  a live writer.
- Migration never deletes unknown files from `.ssg` or any non-empty directory.
- Persistent undo, saved-file backups, file archives, and session recovery
  remain separate concepts.

## Risks and Mitigations
- Frequent whole-session writes can affect typing latency. Schedule from a dirty
  generation, bound lock-held capture work, and perform encoding and I/O outside
  the input path with one coalesced write in flight.
- A path-key collision could expose another session. Store and validate the
  canonical identity inside the snapshot before restoring.
- Migration could destroy the only recoverable copy. Delete legacy state only
  after the equivalent central snapshot is durably written.
- Cleanup could follow a hostile symlink or remove user data. Validate owned
  paths, refuse unexpected object kinds, and prune only known empty parents.
- Permission or ownership setup failures are fatal; SSG does not continue with
  central or scratch state in an untrusted directory.
- Shutdown can race a background checkpoint. Join or supersede it before the
  final synchronous persistence decision.

## Acceptance (Definition of Done)
- Observable: launching and exiting SSG does not create `.ssg` in the launch
  directory.
- Observable: dirty tabs restore when SSG is relaunched from the same directory
  after normal exit, hangup, logout, or forced process termination occurring
  after a completed checkpoint.
- Observable: saving or discarding all dirty tabs removes central recovery state.
- Observable: a pre-existing project-local snapshot imports once without losing
  drafts; unrelated `.ssg` contents remain.
- Budgets: encoding and filesystem I/O do not block the input path; capture is
  bounded and pending edits are coalesced. Persistent disk use contains only the
  latest complete snapshot, and abandoned replacement files are reclaimed when
  no live writer can own them.
- Gates: `cmake --build build`, `ctest --preset dev`, and
  `ctest --preset all` are green.
- Oracles: fixed canonical-path/key vectors pin session identity; two editor
  lifetimes pin central restoration; a hand-authored disk-baseline matrix pins
  non-clobber restoration; a controllable scheduler pins checkpoint coalescing
  dirty-generation races including continuous editing and interrupted
  publication; filesystem fixtures pin migration, both-present precedence, and
  owned-empty-directory cleanup; a simulated termination notification pins the
  final checkpoint without testing OS-emitted events.

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Add backward-compatible identity-bearing snapshots, derive the central path, pass identity through `EditorConfig`, and migrate legacy state before editor construction | `include/ssg/SessionSnapshot.h`, `src/SessionSnapshot.cpp`, `include/ssg/Editor.h`, `src/Editor.cpp`, `src/main.cpp`, `tests/test_session_snapshot.cpp`, `tests/test_startup_path.cpp`, `README.md` | legacy-version fixture, fixed-vector path key, wrong-identity fixture, and both-present migration fixture | SESSION-IDENTITY, SESSION-ATOMIC, SESSION-PRIVATE |
| 3 | Remove empty snapshots and prune only owned empty session and legacy directories | `src/SessionSnapshot.cpp`, `src/Editor.cpp`, `include/ssg/platform_files.h`, `src/platform/linux/linux_files.cpp`, `src/platform/windows/windows_files.cpp`, `tests/test_session_snapshot.cpp`, `tests/test_platform_files.cpp` | filesystem fixture with empty, non-empty, regular-file, and symlink parents | SESSION-CLEAN, SESSION-PRIVATE |
| 4 | Expose dirty-session generations and checkpoint captures independently of runtime timing | `include/ssg/Editor.h`, `src/Editor.cpp`, `tests/test_session_snapshot.cpp` | generation hand cases cover edit-during-write, save, close, and discard | RECOVERY-BOUNDED, SESSION-NONCLOBBER |
| 5 | Add serialized coalescing persistence, orphan-replacement cleanup, and final ordinary-context checkpointing for termination notifications | `include/ssg/Editor.h`, `src/Editor.cpp`, `src/main.cpp`, `include/ssg/DurableStore.h`, `src/DurableStore.cpp`, `tests/test_session_snapshot.cpp`, `tests/test_runtime_timing.cpp`, `CMakeLists.txt` | controllable scheduler covers continuous edits, failure retry, edit-during-write, deletion ordering, interrupted publication, and simulated termination | RECOVERY-BOUNDED, SESSION-SIGNAL-SAFE, SESSION-ATOMIC |
| 6 | Exclusively create a private unpredictable process-recovery root and remove the verified owned tree after editor destruction | `include/ssg/ProcessRecoveryRoot.h`, `src/ProcessRecoveryRoot.cpp`, `include/ssg/platform_files.h`, `src/platform/linux/linux_files.cpp`, `src/platform/windows/windows_files.cpp`, `src/main.cpp`, `CMakeLists.txt`, `tests/test_recovery.cpp`, `tests/test_platform_files.cpp` | planted-symlink, ownership-failure, and owned temporary-tree lifetime fixtures | SESSION-CLEAN, SESSION-PRIVATE |
| 7 | Independently add conservative stale process-root housekeeping using ownership markers and live locks | `include/ssg/ProcessRecoveryRoot.h`, `src/ProcessRecoveryRoot.cpp`, `include/ssg/platform_files.h`, `src/platform/linux/linux_files.cpp`, `src/platform/windows/windows_files.cpp`, `tests/test_recovery.cpp`, `tests/test_platform_files.cpp` | locked-live, unlocked-stale, malformed, symlink, and foreign-entry fixtures | SESSION-CLEAN, SESSION-PRIVATE |
| 8 | Promote contracts into code and user documentation, then delete this completed spec | `include/ssg/SessionSnapshot.h`, `include/ssg/Editor.h`, `include/ssg/ProcessRecoveryRoot.h`, `README.md`, `plan.md` | full build and test gates | all |

## Rationale
Vim and Emacs remove live recovery files after successful editing while retaining
crash remnants; VS Code keeps working-copy backups in centralized user state.
SSG’s current shutdown-only snapshot provides Hot Exit but cannot recover edits
lost with an SSH connection or process. Keeping the existing strict snapshot and
adding periodic atomic replacement is smaller and easier to verify than
reintroducing the removed scratch journal. Central storage avoids repository
litter, while embedded canonical identity makes a compact keyed layout safe to
validate.
