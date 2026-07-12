# follow-edits

- Spec: `doc/features/workspace-live-diffs.md`, Plan 4
- Depends: `diff-model`, `session-state`, `prompt-status-surface`, `external-modification-flow`
- Branch: `follow-edits-task`

## Scope

Implement shared follow/pause/resume targets, dirty conflicts, active diff tabs,
target validity, and dimension-specific per-client scroll offsets. The
component emits stable target-activation intents; `editor-session-assembly`
later binds them to live-diff tabs and `TabManager`.

Consume accepted `DiffFileView` changes after external-modification routing,
including dirty-buffer conflicts. Export `FollowEditsViewState`,
`FollowEditsDelta`, the paused footer contribution/resume binding, and the
immutable `follow_edits.resume`, `follow_edits.pause` command set. Classify
semantic command handling as user navigation, programmatic navigation, or
non-navigation: only user navigation pauses, before applying the navigation.
The explicit pause command and automatic pause path must produce the same
shared mode transition.

## Files

`include/ssg/follow_edits.h`, `src/follow_edits.cpp`,
`tests/fixtures/follow_edits/`, `tests/test_follow_edits.cpp`,
`cmake/components/follow-edits.cmake`

## Oracle

Independent transition tables cover edit storms, conflicts, cross-client pause,
queue replacement, rename/delete/revert, explicit pause, user versus
programmatic/non-navigation commands, and differing viewports. Resume cases
cover newest-valid selection with older-target clearing and no-valid-target
return to following without movement.

## Done

The mandatory workflow is complete with one shared follow policy and
transitions; only library-owned scroll offsets differ to fit client viewport
dimensions.
