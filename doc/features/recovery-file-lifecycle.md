# spec-recovery-file-lifecycle

## Goals

Open directories and files, save/close/reopen tabs immediately without dialogs, preserve dirty and untitled contents across restart, expose external modifications, and reverse major state-losing file actions.

## Design

The workspace and recovery mechanisms are those in `doc/spec.md`. Recovery namespaces use workspace hashes, generated session IDs, operating-system locks, stable file identities, and `UntitledDocumentId`. Atomic save uses same-directory temporary replacement. Close, reload, overwrite, rename, delete, and workspace replacement create bounded compensating records before mutation.

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

`UntitledDocumentId` is a generated unique identifier defined by
`scratch_journal.h`, persisted in journal records, and stable across restart.
The user-visible `Untitled N` number is derived by the later tab layer and is not
persisted by the journal-format component. Saved document journal keys use
workspace-relative path identity rather than filesystem inode identity.

Normative commands owned by this feature:

- `workspace.open_directory`
- `file.new`, `file.open`, `file.open_recent`, `file.open_dropped_content`, `file.save`, `file.save_all`, `file.save_as`, `file.reload`, `file.rename`, `file.delete`, `file.new_directory`
- `file.reopen_with_encoding`, `file.set_encoding`, `file.set_line_ending`, `file.set_final_newline`
- `tab.close`, `tab.close_others`, `tab.close_all`, `tab.reopen_closed`, `tab.next`, `tab.previous`, `tab.activate`, `tab.move_left`, `tab.move_right`
- `external.reload`, `external.keep_buffer`, `external.open_diff`

Commands requiring a path use the non-modal `PromptSurface` contract. `file.open_dropped_content` is an ingress-only, Lua-excluded command available only when authenticated host policy grants its `InvocationPrincipal` the `local_file_drop` capability. It applies the normal decode/binary pipeline and opens bytes as untitled documents with bounded sanitized display labels. Labels gain no path authority. Remote and locality-unknown clients cannot invoke it. Encoding, BOM, and line-ending behavior follows `doc/spec.md`.

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
| 3 | Implement file tabs, path prompts, encoding/EOL conversion, save, close/reopen, and recovery records | `include/ssg/recovery.h`, `src/recovery.cpp`, `tests/fixtures/encoding/*`, `tests/test_recovery.cpp` | byte-exact encoding/EOL and compensating-command round trips | I5, I19 |
| 4 | Implement directory lifecycle and external-modification status actions | `include/ssg/workspace.h`, `src/workspace.cpp`, `tests/test_workspace.cpp` | filesystem ground truth and fault injection | I9, I16, I21 |

## Rationale (optional, skippable)

File lifecycle and recovery are one contract because non-modal close is safe only when recovery is already durable.
