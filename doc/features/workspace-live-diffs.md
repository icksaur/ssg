# spec-workspace-live-diffs

## Goals

Watch the CWD on Linux and Windows, expose filesystem/Git trees and external-modification status, compute live diffs, and follow external edits consistently across clients.

## Design

Watcher normalization, seeded baselines, diff tabs, dirty conflicts, shared follow state, per-client scrolling, pause/resume, and target validity follow `doc/spec.md`. Diff correctness is defined by patch reconstruction and normalized changed-line sets, not byte-identical Git hunk boundaries.

Normative commands owned by this feature:

- `follow_edits.resume`, `follow_edits.pause`
- `diff.next_hunk`, `diff.previous_hunk`, `diff.open_file`

## Invariants

I3, I5, I9, I10, I14, I16, I19, I21 from `doc/spec.md`.

## Considerations

- inotify and `ReadDirectoryChangesW` normalize into one event vocabulary.
- Overflow triggers bounded rescan.
- SSG-originated saves do not duplicate events.
- Dirty buffers remain unchanged while disk diffs and footer actions update.

## Risks and Mitigations

- Event storms/races: debounce by stable identity and discard stale revision-tagged work.
- Follow fighting users: user navigation pauses globally before movement.

## Acceptance (Definition of Done)

- Observable: external edits update tabs/diffs and follow state identically on Linux and Windows and across clients.
- Budgets: watcher queues and rescans remain bounded.
- Gates: watcher/diff/follow tests are green on Linux and Windows.
- Oracles: temporary-directory event scripts, reconstructing diff properties, hand-authored changed-line cases, and independent follow transition tables.

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Implement tree providers and external status actions | `include/ssg/tree.h`, `src/tree.cpp`, `tests/test_tree.cpp` | temporary-directory ground truth | I9, I16 |
| 2 | Implement Linux/Windows watcher normalization | `include/ssg/watcher.h`, `src/watcher.cpp`, `src/platform/*watcher.cpp`, `tests/test_watcher.cpp` | identical normalized event scripts | I10, I21 |
| 3 | Implement Git/non-Git diff models | `include/ssg/diff.h`, `src/diff.cpp`, `tests/test_diff.cpp` | patch reconstruction and changed-line fixtures | I3, I5 |
| 4 | Implement follow/pause/resume | `include/ssg/follow_edits.h`, `src/follow_edits.cpp`, `tests/test_follow_edits.cpp` | independent transition table | I14, I19 |

## Rationale (optional, skippable)

Watcher, diff, and follow behavior form one ordered external-change pipeline.
