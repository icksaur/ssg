# spec-recovery-file-lifecycle

## Goals

Open directories and files, save/close/reopen tabs immediately without dialogs, preserve dirty and untitled contents across restart, expose external modifications, and reverse major state-losing file actions.

## Design

The workspace and recovery mechanisms are those in `doc/spec.md`. Recovery namespaces use workspace hashes, generated session IDs, operating-system locks, stable file identities, and `UntitledDocumentId`. Atomic save uses same-directory temporary replacement. Close, reload, overwrite, rename, delete, and workspace replacement create bounded compensating records before mutation.

Plan 3 owns only the transport-neutral compensating-record primitive and its
filesystem/document restoration operations. File-tab, prompt, encoding/EOL,
save, directory-lifecycle, and command-handler integration remain owned by the
later `file-commands` and Plan 4 tasks. Plan 3 does not expose tab UI state.

`RecoveryActions` is constructed with an explicit recovery root and immutable
finite count and byte budgets. Records are evicted oldest-first only after the
newest record and every required recovery artifact have been installed
successfully. An action whose record cannot fit after eligible eviction is
rejected before mutation. Close and reload records contain the complete prior
`JournalDocument`; overwrite records contain the prior destination bytes and
whether it existed; rename records preserve both source identity/content and
any prior destination; delete records preserve the removed tree; workspace
replacement records preserve the prior workspace tree. Recovery artifacts live
under the supplied root; composition may place that root beneath the current
scratch session, but Plan 3 enforces its own budget and cleanup.

Every major-action operation follows one failure contract: install its complete
record and recovery artifacts first, perform the mutation second, and publish
the compensating record only after the mutation succeeds. Failure while
preparing the record leaves canonical document/filesystem state unchanged.
Failure during mutation rolls back from the prepared record; if rollback itself
fails, the prepared record remains available and the operation reports both the
action and rollback failure. A compensating command is retryable until it
succeeds and removes only its own record and artifacts after restoration.
Injected-failure tests cover preparation, each mutation step, restoration, and
retry.

Dirty close first submits the complete document to `ScratchStore` and waits for
that accepted generation to become durable within a caller-supplied finite
timeout. Timeout or failed durability rejects close without changing document
state. This synchronous service boundary is non-modal: it requests no user
decision, while later command/status assembly reports the actionable failure.
Plan 3's workspace-replacement, rename, and delete operations are the reversible
storage primitives only; Plan 4 owns `workspace.open_directory` and directory
lifecycle policy.

Plan 1 owns the shared platform primitive used by later recovery work:

- syntactic workspace-relative path validation is a pure operation parameterized
  by Linux or Windows syntax; canonicalization, symlink traversal, and CWD
  authority enforcement remain Plan 4 responsibilities;
- Windows legacy paths are limited to 259 UTF-16 code units (excluding the
  terminating NUL), while extended policy permits at most 32,766 UTF-16 code
  units; both policies reject components beyond 255 UTF-16 code units;
- `FileIdentity` normalizes Linux device/inode and Windows volume/file ID and is
  stable across close/reopen and rename while distinguishing different files;
- `ExclusiveFileLock` is the sole move-only, RAII OS advisory-lock primitive;
  Plan 2 session locking consumes it and owns workspace/session namespace,
  remnant-discovery, and newest-restorable policy rather than another OS lock;
- scratch-session callers provide an explicit scratch root and an
  already-canonical absolute workspace path. Its lowercase SHA-256 workspace
  key is computed from the path's native UTF-8 representation. Session IDs use
  a fixed-width UTC creation timestamp plus 128 random bits, making lexical
  order the total newest-first order. The namespace is
  `<root>/workspaces/<workspace-hash>/sessions/<session-id>/`, containing
  files named **session.lock**, **journal.bin**, and an optional **restored**
  marker;
- an unlocked remnant is restorable only when it is not marked restored and
  replaying its journal yields at least one document. Newest-restorable
  selection holds the remnant's existing `ExclusiveFileLock` in a move-only
  claim. Successful import atomically writes the restored marker; a crash
  before that point leaves the remnant retryable, while cleanup and quota
  remain the later scratch-composition component's policy;
- owner-only permission operations deny other principals read/write access;
- cache-root lookup returns the OS user cache location with a validated
  application component; and
- atomic replacement writes a same-directory temporary file and leaves the
  destination containing either complete old bytes or complete new bytes.

The public seam is `platform_files.h`: typed path validation and errors,
`FileIdentity`, move-only `ExclusiveFileLock`, owner-only permissions,
user-cache-root lookup, and byte-oriented atomic replacement. Operational
failures throw actionable standard exceptions; lock contention alone is the
empty result of `try_lock_file`.

Scratch journal framing is portable and fixed: all multi-byte fields use
little-endian encoding, and each framed payload is protected by CRC-32C.
Fixtures are byte-identical on Linux and Windows. The format defines every
on-disk record kind, including a base/checkpoint record. Replay begins with the
newest checksum-valid checkpoint and applies later checksum-valid append records;
a corrupt or truncated tail is ignored after the last complete valid record.
The later compaction component decides when to emit a checkpoint but adds no
record kind or journal encoding.

Scratch composition owns a move-only `ScratchStore` created from an explicit
scratch root, an already-canonical absolute workspace path, and immutable
`ScratchStoreConfig`. The configuration supplies finite byte and age quotas, a
compaction threshold, and a durability target that defaults to 100 ms. A test
storage seam may replace journal append and atomic replacement operations; its
caller-owned lifetime must exceed the store.

Construction creates and owns the current `ScratchSession`, claims the newest
restorable remnant, replays it, atomically installs one checkpoint in the new
session, and only then marks the remnant restored. Failure before the marker
leaves the remnant retryable. The imported recovery set is available to the
caller. Accepted document updates and removals update that set and enter one
ordered background queue. The queue is the sole writer to the current journal.
Shutdown rejects new updates, drains accepted work, and joins the writer.

Compaction serializes against appends on that same queue. It atomically replaces
journal.bin with exactly one checkpoint encoding the accepted recovery set at
the compaction generation; subsequent records append after the replacement.
Replay before and after compaction therefore produces the same recovery set.
A failed append or replacement preserves the last replayable journal, records
an actionable failure, and does not report the failed generation durable.

`ScratchDurabilityState` exposes durable, pending, or failed state, accepted and
durable generations, whether pending work has exceeded the configured target,
and an actionable failure string. This is typed service state consumed later by
session/footer assembly, not a footer or out-of-band presentation channel.

Quota and explicit purge operations inspect restored remnants under the scratch
root. Automatic age eviction removes eligible remnants older than the configured
age, then byte eviction removes eligible remnants oldest-first by fixed-width
session ID until the root is within budget. The current session, another live
session, and every unrestored remnant are ineligible even when the quota cannot
otherwise be met. `purge_workspace` removes eligible restored remnants only for
the current workspace; `purge_all` does so for every workspace. These are typed
maintenance operations, not user-visible registry commands, and need no
compensating record because they delete only redundant remnants already imported
and marked restored.

`UntitledDocumentId` is a generated unique identifier defined by
`scratch_journal.h`, persisted in journal records, and stable across restart.
The user-visible `Untitled N` number is derived by the later tab layer and is not
persisted by the journal-format component. Saved document journal keys use
workspace-relative path identity rather than filesystem inode identity. A
document that has never been named is labelled `[new buffer]`
(`ssg::kNewBufferLabel`); the runtime opens one at startup when no file was
opened, so the editor is always typeable.

File-management rules, all owned here:

- **Name clash.** A name-taking operation fails if the destination already
  exists, except that saving a document over its own current path may overwrite.
  Enforced by the filesystem (`createFileExclusively`,
  `renamePathNoClobber`), never by an `exists()` check, so a file created
  between the check and the write cannot be destroyed.
- **Delete archives first.** `file.delete` copies the file into
  `.ssg/archive/<utc-timestamp>-<counter>/<workspace-relative-path>` and returns
  only once that copy is durable, *then* removes the original. If the archive
  cannot be written the delete fails and the file is untouched: a delete never
  reduces the number of copies below one. The archive is deliberately separate
  from the recovery store, which is a bounded evicting undo ring and so cannot
  be the only surviving copy. Entries older than
  `ssg::kFileArchiveRetention` (14 days) are pruned at workspace open; entries
  whose name cannot be read, or that are dated in the future, are retained and
  counted in the returned prune report rather than removed. A prune that
  genuinely fails raises a warning status and never blocks opening the
  workspace.
- **Deleting closes the tab.** The document has no backing bytes, so its tabs
  are dropped without the close lifecycle (there is nothing to flush, and the
  workspace entry is already gone).
- **Live diff tabs.** A command whose descriptor sets
  `mutatesActiveDocumentFile` is refused while a live diff tab is active, since
  such a tab is a computed view of two revisions and has no file. `file.new` and
  `file.new_directory` create something new and are allowed.
- **Path entry.** A command whose descriptor sets `pathPrompt`, dispatched with
  no path, opens the `PromptKind::Path` prompt naming itself; submitting
  re-dispatches that command with the typed value. `file.save` on an unnamed
  buffer opens `file.save_as`'s prompt, because naming a buffer is what save-as
  does.

Nothing under `.ssg/` is intended to be committed; it is gitignored wholesale.

Normative commands owned by this feature:

- `workspace.open_directory`
- `file.new`, `file.open`, `file.open_recent`, `file.open_dropped_content`, `file.save`, `file.save_all`, `file.save_as`, `file.reload`, `file.rename`, `file.delete`, `file.new_directory`
- `file.reopen_with_encoding`, `file.set_encoding`, `file.set_line_ending`, `file.set_final_newline`
- `tab.close`, `tab.close_others`, `tab.close_all`, `tab.reopen_closed`, `tab.next`, `tab.previous`, `tab.activate`, `tab.move_left`, `tab.move_right`

The external-modification flow owns `external.reload`,
`external.keep_buffer`, and `external.open_diff`. It uses the reload primitive
here to install the complete prior `JournalDocument` before replacing a dirty
buffer, as specified by `doc/features/workspace-live-diffs.md`.

Commands requiring a path use the non-modal `PromptSurface` contract. `file.open_dropped_content` is an ingress-only, Lua-excluded command available only when authenticated host policy grants its `InvocationPrincipal` the `local_file_drop` capability. It applies the normal decode/binary pipeline and opens bytes as untitled documents with bounded sanitized display labels. Labels gain no path authority. Remote and locality-unknown clients cannot invoke it. Encoding, BOM, and line-ending behavior follows `doc/spec.md`.

The file-command component exports one immutable `FileCommandsCommandSet`.
Common command dispatch enforces required capabilities and Lua exclusion; the
dropped-content handler also validates the principal so direct typed use cannot
bypass the ingress contract. `file.open_recent` uses a workspace-owned,
process-memory MRU list of at most 32 normalized workspace-relative paths;
successful open and save add an entry, duplicates move to the front, and
missing entries are removed when selected. Later session persistence may
serialize this typed state. `file.save_all` attempts every dirty saved document,
reports all failures, and retains dirty state only for failed saves.

Every workspace-relative path is rejected before mutation when it is absolute,
contains traversal, or resolves through a symlink outside the canonical CWD.
Duplicate editable-document detection uses normalized workspace-relative path
identity rather than inode identity. An untitled document retains its generated
`UntitledDocumentId` and dirty state until atomic replacement succeeds.

The encoding/EOL component is a pure library seam. It does not read settings,
documents, tabs, or files. Callers pass the selected encoding and output policy
explicitly. Automatic detection recognizes valid UTF-8, UTF-8 BOM, and
BOM-marked UTF-16LE/BE. Windows-1252, ISO-8859-1, and BOM-less UTF-16 are
manual reopen choices only. Invalid BOM-less UTF-8 is refused rather than
replaced; decoding either single-byte encoding always succeeds.

Decoded text is valid UTF-8 with all line terminators normalized to LF. Separate
per-line terminator metadata records LF, CRLF, CR, or no terminator, allowing
mixed endings and final-newline presence to round trip byte-exactly. CR-only is
a first-class detected ending. NUL/binary classification remains the
file-command pipeline's responsibility.

UTF-16 output always includes its endian BOM. UTF-8 preserves whether its BOM
was present; no command toggles that flag. Windows-1252 and ISO-8859-1 encoding
refuse atomically when any Unicode scalar is unrepresentable and report the
offending UTF-8 byte offset; substitution is never allowed.

`file.reopen_with_encoding` asks the recovery layer to re-decode the original
file bytes and create its compensating record. `file.set_encoding` changes only
the next-save encoding after representability validation.
`file.set_line_ending` is the explicit normalization command and replaces every
stored terminator with LF, CRLF, or CR. `file.set_final_newline` selects
preserve-as-detected, ensure-present, or ensure-absent. These commands' handlers
and recovery effects belong to the later file-command layer; this component
exports only their immutable descriptors and pure view/delta derivation.

### Tab management

The tab-management component is the sole owner of `tab.close`,
`tab.close_others`, `tab.close_all`, `tab.reopen_closed`, `tab.next`,
`tab.previous`, `tab.activate`, `tab.move_left`, and `tab.move_right`.
File commands create and identify documents but do not own tab ordering or these
commands. The component exports one immutable `TabManagementCommandSet`, a
typed `TabViewState`, and pure `TabDelta` derivation and replay functions for
later session assembly.

Tabs have strong tab IDs and typed kinds: document, live diff, read-only output,
search results, and tree view. Every kind participates in activation, cyclic
next/previous navigation, close, and reorder. Document tabs additionally carry
their `FileDocumentId` and `JournalDocumentKey`; duplicate saved path identity
or duplicate untitled ID activates the existing tab instead of allocating
another. Saved identity is the normalized workspace-relative path, never inode
identity. Each untitled identity is unique.

Tab state exposes kind, label, mode, dirty state, and recovery status. The tab
layer derives the smallest available positive `Untitled N` label for an
untitled document without a caller-supplied label. A closed tab retains its
label in its recently-closed entry, so reopening preserves the number; a newly
created tab may reuse a number not used by an open tab.

Closing delegates document removal and dirty-document durability to an injected
document-lifecycle boundary; tab management performs no filesystem I/O. A dirty
close is accepted only when that boundary reports an installed durable recovery
record within the caller-supplied finite positive timeout. Timeout, durability
failure, or a success-shaped dirty close without a recovery record leaves tab
topology unchanged and returns an actionable error. Clean and non-document
closes use the same boundary so lifecycle mutation and topology have one
failure-atomic seam.

Closing an active tab selects its immediate right neighbor, or its immediate
left neighbor when no right neighbor remains. Closing a non-active tab preserves
the active tab. `close_others` and `close_all` are deterministic best-effort
operations in original left-to-right order: each accepted close is removed and
recorded, each failed close remains, and all failures are returned. If the
active tab survives it remains active; otherwise the first surviving tab at or
to the right of its original position is selected, falling back to the last
survivor.

Recently closed entries are a LIFO stack bounded to 32 entries, evicting the
oldest entry after each accepted close. `tab.reopen_closed` restores the newest
entry through the same lifecycle boundary, inserts it at its original index
clamped to the current tab count, activates it, and removes the entry only after
successful restoration. Failed restoration is retryable and changes no tab
state. If the same content identity is already open, reopen activates that tab
and consumes the stale recently-closed entry without restoring a duplicate.
Reopen restores content, identity, mode, badges, and tab position but not undo
history.

## Invariants

I4, I5, I9, I10, I16, I19, I21 from `doc/spec.md`.

## Considerations

- Linux and Windows path, permission, locking, cache-root, newline, and replacement semantics differ behind adapters.
- Journals target durability within 100 ms; footer state exposes pending/failure.
- Journal encoding, append, flush, and replay are synchronous. The later
  compaction/quota component owns asynchronous scheduling and durability status.
- Journal append uses a journal-owned durable-append primitive. Its parent
  directory must already exist. It flushes the journal file and the
  platform-required creation metadata: Linux flushes the parent directory on
  first create, while Windows uses write-through creation plus
  `FlushFileBuffers`. Atomic replacement is reserved for
  checkpoint/compaction snapshots.
- Recovery restores content and topology, not undo history.
- Journal topology is the recoverable open-document set with each document's
  identity, mode, and dirty state. Tab/split geometry belongs to session-state
  persistence.
- Multiple live sessions never share writable journal files.
- Untitled-to-saved identity changes only after a successful atomic save.

## Risks and Mitigations

- Crash corruption: checksummed append records and atomic compaction.
- Sensitive growth: user-only permissions and byte/age quotas.
- Windows path traps: validate reserved names, characters, trailing dots/spaces, and long-path policy before mutation.

## Acceptance (Definition of Done)

- Observable: clean/dirty/untitled tabs survive the specified close/crash scripts; every major action exposes and executes its compensating command.
- Budgets: accepted dirty state reaches durable journal storage within 100 ms p99 on the benchmark filesystem.
- Gates: Linux and Windows file/recovery suites are green.
- Oracles: fault-injected journal fixtures, concurrent-session tests, temporary-directory truth, and compensating-command round trips.

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Implement platform file identity, locking, paths, permissions, cache roots, and atomic replacement | `include/ssg/platform_files.h`, `src/platform/*files.cpp`, `tests/test_platform_files.cpp` | platform-independent Linux/Windows path decision table on both platforms; native temporary-directory identity/rename, lock/contention/release, owner-only permission, cache-root, and complete-old-or-new replacement cases | I4, I21 |
| 2a | Implement every on-disk journal record kind, durable append, document recovery-set encoding, untitled IDs, and replay | `include/ssg/scratch_journal.h`, `src/scratch_journal.cpp`, `tests/fixtures/scratch/journal/*`, `tests/test_scratch_journal.cpp` | byte-level corruption/truncation/untitled/restart round trips | I10, I19, I21 |
| 2b | Implement workspace/session namespaces, process locks, remnant discovery, and newest-restorable policy | `include/ssg/scratch_session.h`, `src/scratch_session.cpp`, `tests/test_scratch_session.cpp` | concurrent live/crashed-session fixtures | I10, I19, I21 |
| 2c | Compose scratch recovery with checkpoint scheduling, compaction, quota, and durability status | `include/ssg/scratch.h`, `src/scratch.cpp`, `tests/test_scratch.cpp` | compaction/quota/durability-lag fixtures | I10, I19, I21 |
| 3 | Implement bounded compensating records and restoration primitives for close, reload, overwrite, rename, delete, and workspace replacement | `include/ssg/recovery.h`, `src/recovery.cpp`, `tests/test_recovery.cpp`, `cmake/components/recovery-actions.cmake` | canonical document/filesystem ground truth after major-action plus compensation, with injected failures proving record-before-mutation ordering, failure atomicity, restoration retry, and oldest-first count/byte bounds | I5, I19 |
| 4 | Implement directory lifecycle and external-modification status actions | `include/ssg/workspace.h`, `src/workspace.cpp`, `tests/test_workspace.cpp` | filesystem ground truth and fault injection | I9, I16, I21 |
| 4a | Implement directory lifecycle, untitled and saved-document identity, path prompts, the immutable file command set, and new/open/recent/save/save-all/save-as/reload/rename/delete/new-directory/drop handlers | `include/ssg/workspace.h`, `include/ssg/file_commands.h`, `src/workspace.cpp`, `src/file_commands.cpp`, `tests/test_workspace.cpp`, `tests/test_file_commands.cpp`, `cmake/components/file-commands.cmake` | temporary-directory truth; byte-exact save and decode round trips; normalized-path duplicate prevention; successful and failed untitled identity transitions; bounded recent MRU; absolute/traversal/symlink escape rejection before mutation; best-effort save-all; compensating-command scripts; authenticated local-drop acceptance and Lua/remote/unknown/client-asserted-locality rejection without allocation | I5, I9, I16, I19, I21 |
| 4b | Implement typed tab state, badges, activation, cyclic navigation, ordering, immediate close variants, and bounded reopen | `include/ssg/tabs.h`, `src/tabs.cpp`, `tests/test_tabs.cpp`, `cmake/components/tab-management.cmake` | hand-authored transition tables for duplicate identities, dirty-close durability failure, active selection, best-effort batch close, LIFO reopen, ordering, and oldest-first bounded eviction | I2, I15, I16, I19 |

## Rationale (optional, skippable)

File lifecycle and recovery are one contract because non-modal close is safe only when recovery is already durable.
