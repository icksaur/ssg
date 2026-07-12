# scratch-journal-format

- Spec: `doc/features/recovery-file-lifecycle.md`, Plan 2
- Depends: `platform-file-io`
- Branch: `scratch-journal-format-task`

## Scope

Implement checksummed append records, document/topology journal encoding,
untitled IDs, restart replay, and truncated-tail recovery without process locks,
compaction, or quotas.

## Files

`include/ssg/scratch_journal.h`, `src/scratch_journal.cpp`,
`tests/fixtures/scratch/journal/`, `tests/test_scratch_journal.cpp`,
`cmake/components/scratch-journal-format.cmake`

## Oracle

Byte-level round trips plus corruption, truncation, untitled, and restart
fixtures with the last checksum-valid record as ground truth.

## Done

The mandatory workflow is complete for journal format/replay only.
