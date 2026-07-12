# spec-recovery-file-lifecycle

## Goals

Open directories and files, save/close/reopen tabs immediately without dialogs, preserve dirty and untitled contents across restart, expose external modifications, and reverse major state-losing file actions.

## Design

The workspace and recovery mechanisms are those in `doc/spec.md`. Recovery namespaces use workspace hashes, generated session IDs, operating-system locks, stable file identities, and `UntitledDocumentId`. Atomic save uses same-directory temporary replacement. Close, reload, overwrite, rename, delete, and workspace replacement create bounded compensating records before mutation.

Normative commands owned by this feature:

- `workspace.open_directory`
- `file.new`, `file.open`, `file.open_recent`, `file.open_dropped_content`, `file.save`, `file.save_all`, `file.save_as`, `file.reload`, `file.rename`, `file.delete`, `file.new_directory`
- `file.reopen_with_encoding`, `file.set_encoding`, `file.set_line_ending`, `file.set_final_newline`
- `tab.close`, `tab.close_others`, `tab.close_all`, `tab.reopen_closed`, `tab.next`, `tab.previous`, `tab.activate`, `tab.move_left`, `tab.move_right`
- `external.reload`, `external.keep_buffer`, `external.open_diff`

Commands requiring a path use the non-modal `PromptSurface` contract. `file.open_dropped_content` is an ingress-only, Lua-excluded command available only when authenticated host policy grants its `InvocationPrincipal` the `local_file_drop` capability. It applies the normal decode/binary pipeline and opens bytes as untitled documents with bounded sanitized display labels. Labels gain no path authority. Remote and locality-unknown clients cannot invoke it. Encoding, BOM, and line-ending behavior follows `doc/spec.md`.

## Invariants

I4, I5, I9, I10, I16, I19, I21 from `doc/spec.md`.

## Considerations

- Linux and Windows path, permission, locking, cache-root, newline, and replacement semantics differ behind adapters.
- Journals target durability within 100 ms; footer state exposes pending/failure.
- Recovery restores content and topology, not undo history.
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
| 1 | Implement platform file identity, locking, paths, and atomic replacement | `include/ssg/platform_files.h`, `src/platform/*files.cpp`, `tests/test_platform_files.cpp` | Linux/Windows temporary-directory cases | I4, I21 |
| 2 | Implement checksummed document/session journals and restoration | `include/ssg/scratch.h`, `src/scratch.cpp`, `tests/fixtures/scratch/*`, `tests/test_scratch.cpp` | corruption/truncation/concurrency/untitled round trips | I10, I19 |
| 3 | Implement file tabs, path prompts, encoding/EOL conversion, save, close/reopen, and recovery records | `include/ssg/recovery.h`, `src/recovery.cpp`, `tests/fixtures/encoding/*`, `tests/test_recovery.cpp` | byte-exact encoding/EOL and compensating-command round trips | I5, I19 |
| 4 | Implement directory lifecycle and external-modification status actions | `include/ssg/workspace.h`, `src/workspace.cpp`, `tests/test_workspace.cpp` | filesystem ground truth and fault injection | I9, I16, I21 |

## Rationale (optional, skippable)

File lifecycle and recovery are one contract because non-modal close is safe only when recovery is already durable.
