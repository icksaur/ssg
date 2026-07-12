# scratch-compaction-quota

- Spec: `doc/features/recovery-file-lifecycle.md`, Plan 2
- Depends: `scratch-journal-format`, `scratch-session-locking`
- Branch: `scratch-compaction-quota-task`

## Scope

Compose session journals with atomic compaction, byte/age quotas, durability
tracking, purge commands, and failed/pending footer state.

## Files

`include/ssg/scratch.h`, `src/scratch.cpp`,
`tests/fixtures/scratch/compaction/`, `tests/test_scratch.cpp`,
`cmake/components/scratch-compaction-quota.cmake`

## Oracle

Compaction equivalence, quota eviction order, purge, injected write failures,
and 100 ms durability-lag fixtures.

## Done

The mandatory workflow is complete without major-action recovery policy.
