# scratch-journal-format

- Spec: `doc/features/recovery-file-lifecycle.md`, Plan 2
- Depends: `platform-file-io`
- Branch: `scratch-journal-format-task`

## Scope

Implement checksummed append records, every on-disk record kind (including the
base/checkpoint record), document recovery-set encoding, untitled IDs, restart
replay from the newest checkpoint plus later records, and truncated-tail
recovery without process locks, compaction policy, or quotas. The recovery set
contains document identity, mode, dirty state, and content; tab/split geometry
belongs to session-state persistence. `scratch_journal.h` owns the generated,
restart-stable `UntitledDocumentId`; display numbering is a later tab-layer
concern. This component is synchronous and owns durable append/flush; scheduling,
durability status, and the decision to compact belong to
`scratch-compaction-quota`.

## Files

`include/ssg/scratch_journal.h`, `src/scratch_journal.cpp`,
`tests/fixtures/scratch/journal/`, `tests/test_scratch_journal.cpp`,
`cmake/components/scratch-journal-format.cmake`

## Oracle

Byte-level round trips plus corruption, truncation, untitled, and restart
fixtures with the last checksum-valid record as ground truth.

## Done

The mandatory workflow is complete for journal format/replay only.
