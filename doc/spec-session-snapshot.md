# spec-session-snapshot

## Goals
On normal SSG shutdown, preserve every editable tab with unsaved content and
restore those tabs on the next launch from the same process-starting directory.
Store the state beneath the configured session directory inside `./ssg`.
Preserve both the draft and current disk version when a saved file changed
between sessions.

## Design
Deliver this in two green commits.

The removal commit deletes the complete scratch implementation:
`ScratchStore`, `ScratchSession`, `ScratchJournal`,
`DraftAutosaveScheduler`, `DraftReopenClassifier`, their tests and fixtures,
draft-conflict notice/actions, durability badges, and every startup, idle-loop,
and shutdown hook. `NoticeView` is deleted because draft conflicts are its only
producer. `RecoveryManager` will be decoupled from scratch types during this
commit and retained because it provides closed-tab reopening and transactional
filesystem rollback. The runtime document identity moves from the
journal-named `JournalDocumentKey` to `DocumentKey`.

The implementation commit adds one `SessionSnapshot` data structure and pure
encode/decode functions. `Editor::saveSession` synchronously gathers dirty
editable document tabs in tab order and atomically replaces one snapshot file.
`createEditor` reads that file and restores its tabs before command-line target
handling. `main` captures the process starting directory before resolving any
file or directory argument and derives the snapshot path using
`kSessionDirectoryName` and `kSessionSnapshotFilename`. An empty configured
snapshot path disables persistence for isolated tests and embedders.

Each snapshot tab stores its backing kind, workspace-relative path when named,
display label, mode, UTF-8 draft content, active marker, and the exact raw disk
bytes from which a persisted saved-file buffer branched. Exact bytes are stored
instead of a hash: this removes collision reasoning and detects encoding or
line-ending changes without another abstraction.

Restore policy:
- Untitled tabs reopen as untitled dirty documents.
- A named path that had not yet been created reopens path-bound when the path is
  still absent.
- A persisted path whose current decoded text equals the draft reopens clean.
- A persisted path whose current raw bytes equal the stored baseline reopens
  with the draft applied and remains dirty.
- A missing persisted path reopens path-bound with the draft, ready to recreate
  the file.
- A path that appeared, changed, became binary, or cannot be decoded never
  receives the draft. The draft reopens as an untitled recovered copy whose
  label names the original path; the disk object remains untouched. The latest
  status message identifies the conflict.

Only dirty editable document tabs are snapshots. Read-only output, diff,
search, and tree tabs are regenerable and excluded. Restored tabs retain their
relative order; the previously active tab is reactivated when it was included,
otherwise the final restored tab is active. A command-line target is opened
after restoration and becomes active. If it names a restored path, the existing
restored tab is focused without duplicating or replacing its draft. A blank new
buffer is created only when neither restoration nor a command-line target
produced an editable tab.

The snapshot is retained after reading. A later clean shutdown atomically
replaces it, including with an empty snapshot, so a crash during the next
session cannot erase the previous recoverable state. Concurrent SSG processes
from one starting directory are deliberately last-clean-exit-wins; there are no
locks, per-process remnants, workers, journals, or compaction.

Corrupt or unsupported snapshots stop startup without modifying the snapshot.
The terminal error names the snapshot and states that moving or deleting it is
the manual recovery action. Likewise, an existing non-directory at
`kSessionDirectoryName` is a path error rather than silently disabling the
feature.

## Invariants
- SESSION-LOCATION: Production derives the snapshot from the process starting
  directory, never the resolved workspace, command-line target, user profile,
  or temporary directory. State at the path derivation function used by `main`.
- SESSION-DIRTY-ONLY: A snapshot contains every and only dirty editable
  document tab. State at `Editor::saveSession`.
- SESSION-ATOMIC: A failed write leaves the previous complete snapshot intact.
  State at the atomic replacement call in `writeSessionSnapshot`.
- SESSION-NONCLOBBER: Restoration never writes a file, and never binds draft
  content to a path whose disk bytes diverged from its baseline. State at
  `Editor::restoreSession`.
- SESSION-DIRECT: Snapshot persistence is synchronous and single-file; no
  worker, journal, lock, remnant, generation, compaction, or periodic timer may
  be added. State at `SessionSnapshot`.
- SESSION-FAIL-LOUD: Corrupt or unsupported snapshots refuse startup with the
  snapshot path in the error and remain untouched. State at
  `readSessionSnapshot`.

## Considerations
- Snapshot lengths and tab counts are bounds-checked before allocation; trailing
  bytes and duplicate active markers are invalid.
- Snapshot text is validated UTF-8 through the same document construction path
  used by live buffers.
- Storing the complete draft and baseline makes snapshot I/O proportional to
  the dirty source documents. This is intentional: excluding a large dirty tab
  would violate the recovery contract. The codec rejects lengths that cannot be
  represented safely but does not impose a policy size cap.
- Existing encoding and line-ending metadata comes from reopening unchanged
  disk files; recovered untitled/path-bound documents use normal new-document
  defaults.
- Saving or closing tabs during a session does not mutate the prior snapshot;
  clean shutdown replaces the complete file from current state.
- A snapshot write failure is printed and makes normal shutdown unsuccessful;
  it is never discarded as a successful close.
- When several saved drafts conflict, restoration publishes one aggregate
  latest-status message naming the number of recovered copies; recovered tab
  labels identify their corresponding paths.

## Risks and Mitigations
- Removing scratch types crosses Workspace, TabManager, RecoveryManager, and UI
  surfaces. Keep the removal commit behaviorally narrow, compile after deleting
  each concept, and retain closed-tab/file-operation recovery tests.
- A conflict implementation could overwrite newer disk content. The restored
  draft becomes untitled before entering Workspace, making overwrite impossible
  by construction.
- Main-only path wiring could escape component tests. Test the path derivation
  independently and test two complete Editor lifetimes against the same
  configured snapshot.

## Acceptance (Definition of Done)
- Observable: type into a new buffer, exit normally, restart from the same
  directory, and see the text in a restored tab.
- Observable: restore multiple dirty tabs in their prior order and active state.
- Observable: when a saved file changed after shutdown, its current disk bytes
  remain unchanged and the old draft appears in a recovered untitled tab.
- Budgets: one synchronous read at startup and one synchronous atomic write at
  normal shutdown; no background thread or periodic work.
- Gates: `cmake --build build`, `ctest --preset dev`, and the targeted session
  snapshot restart/conflict tests are green after each commit.
- Oracles: two-Editor-lifetime restart test; hand-case disk baseline matrix;
  byte-fixture decoder rejection cases; filesystem assertion that conflict
  restoration leaves disk unchanged; path derivation assertion from a starting
  directory distinct from the opened workspace.

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Move journal-independent document identity into `DocumentKey`, decouple closed-tab recovery, and remove durability state from tabs | `include/ssg/DocumentKey.h`, `src/DocumentKey.cpp`, `include/ssg/RecoveryManager.h`, `src/RecoveryManager.cpp`, `include/ssg/Workspace.h`, `src/Workspace.cpp`, `include/ssg/TabManager.h`, `src/TabManager.cpp`, `src/Editor.cpp`, `src/files.cpp`, `src/navigation.cpp`, `src/ExternalModificationFlow.cpp`, `tests/test_recovery.cpp`, `tests/test_tabs.cpp`, `tests/test_workspace.cpp`, `tests/test_open_equivalence.cpp`, `tests/test_name_clash.cpp`, `tests/test_external_modification.cpp`, `tests/test_file_commands.cpp`, `tests/session/test_session_navigation.cpp`, `CMakeLists.txt` | existing closed-tab, rename, delete, external-modification, file-command, and tab tests | - |
| 2 | Delete the scratch store, journal, session, autosave, classifier, fixtures, tests, and all corresponding CMake source and test targets | `include/ssg/ScratchStore.h`, `include/ssg/ScratchJournal.h`, `include/ssg/DraftAutosaveScheduler.h`, `include/ssg/DraftReopenClassifier.h`, `src/ScratchStore.cpp`, `src/ScratchJournal.cpp`, `src/ScratchSession.cpp`, `src/DraftAutosaveScheduler.cpp`, `src/DraftReopenClassifier.cpp`, `tests/test_scratch.cpp`, `tests/test_scratch_journal.cpp`, `tests/test_scratch_session.cpp`, `tests/test_draft_autosave.cpp`, `tests/test_draft_reopen.cpp`, `tests/fixtures/scratch/`, `CMakeLists.txt` | build plus CMake/ctest-name grep proves no surviving subsystem target | SESSION-DIRECT |
| 3 | Delete `NoticeView`, draft-conflict commands, and all runtime persistence hooks, then commit the green removal baseline | `include/ssg/Editor.h`, `include/ssg/GridPresenter.h`, `include/ssg/Layout.h`, `src/RendererPaint.h`, `src/Editor.cpp`, `src/EditorViews.cpp`, `src/GridPresenter.cpp`, `src/Layout.cpp`, `src/PaintChrome.cpp`, `src/files.cpp`, `src/main.cpp`, `doc/commands.md`, `tests/grid_presentation_builder.h`, `tests/test_helpers.h`, `tests/test_render.cpp`, `tests/session/test_session_files.cpp`, `tests/session/test_session_navigation.cpp`, `tests/test_end_to_end_session.cpp` | full build and dev suite plus command-ID grep | SESSION-DIRECT |
| 4 | Define the single snapshot format and strict codec | `include/ssg/SessionSnapshot.h`, `src/SessionSnapshot.cpp`, `tests/test_session_snapshot.cpp`, `tests/fixtures/session/`, `CMakeLists.txt` | round trip plus accepted, truncated, wrong-version, and trailing-byte fixtures | SESSION-ATOMIC, SESSION-FAIL-LOUD |
| 5 | Add Workspace construction paths for restored untitled, uncreated-path, saved, and recovered-copy documents | `include/ssg/Workspace.h`, `src/Workspace.cpp`, `tests/test_workspace.cpp` | hand cases for each backing kind | SESSION-NONCLOBBER |
| 6 | Gather dirty tabs, save atomically, restore in order, and reconcile the disk matrix | `include/ssg/Editor.h`, `src/Editor.cpp`, `src/files.cpp`, `tests/test_session_snapshot.cpp` | two-lifetime restart and disk matrix | SESSION-DIRTY-ONLY, SESSION-NONCLOBBER |
| 7 | Wire the process-starting-CWD path and startup precedence, then remove the completed spec and commit | `src/main.cpp`, `tests/test_startup_path.cpp`, `doc/spec-session-snapshot.md` | distinct-starting-directory path oracle and restart regression | SESSION-LOCATION |

## Rationale
The old implementation optimized for concurrent crash remnants and durable
incremental journaling but never reconstructed tabs, so its strongest guarantees
did not produce the requested feature. A single shutdown snapshot makes the
state transition visible and testable. Recovering a divergent saved draft as
untitled is intentionally less clever than an in-place conflict mode: both
versions survive and an accidental save cannot destroy the newer disk version.
