# scratch-compaction-quota

- Spec: `doc/features/recovery-file-lifecycle.md`, Plan 2
- Depends: `scratch-journal-format`, `scratch-session-locking`
- Branch: `scratch-compaction-quota-task`

## Scope

Compose session journals with atomic compaction, byte/age quotas, durability
tracking, purge commands, and failed/pending footer state.

## Contract

`ScratchStore` owns one `ScratchSession`, synchronously imports the newest
restorable remnant with marker-last ordering, and serializes asynchronous
appends and atomic checkpoint replacement on one writer queue. Immutable
configuration supplies compaction, quota, and 100 ms durability policy.
Shutdown rejects new mutations and drains accepted work.

The public durability state is durable, pending, or failed and includes
generation progress, overdue state, and an actionable failure; later session
assembly maps it to footer presentation. Quotas evict only restored remnants,
oldest session first, and never remove current, live, or unrestored sessions.
Typed `purge_workspace` and `purge_all` maintenance operations have the same
eligibility rule.

## Files

`include/ssg/scratch.h`, `src/scratch.cpp`,
`tests/fixtures/scratch/compaction/`, `tests/test_scratch.cpp`,
`cmake/components/scratch-compaction-quota.cmake`

## Oracle

Compaction equivalence, quota eviction order, purge, injected write failures,
and 100 ms durability-lag fixtures.

## Done

The mandatory workflow is complete without major-action recovery policy.
