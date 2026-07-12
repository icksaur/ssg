# follow-edits

- Spec: `doc/features/workspace-live-diffs.md`, Plan 4
- Depends: `diff-model`, `session-state`, `prompt-status-surface`, `external-modification-flow`
- Branch: `follow-edits-task`

## Scope

Implement shared follow/pause/resume targets, dirty conflicts, active diff tabs,
target validity, and dimension-specific per-client scroll offsets.

## Files

`include/ssg/follow_edits.h`, `src/follow_edits.cpp`,
`tests/fixtures/follow_edits/`, `tests/test_follow_edits.cpp`,
`cmake/components/follow-edits.cmake`

## Oracle

Independent transition tables cover edit storms, conflicts, cross-client pause,
queue replacement, rename/delete/revert, resume, and differing viewports.

## Done

The mandatory workflow is complete without client-specific behavior.
