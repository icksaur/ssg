# external-modification-flow

- Spec: `doc/features/recovery-file-lifecycle.md`; `doc/features/workspace-live-diffs.md`
- Depends: `filesystem-watchers`, `file-commands`, `recovery-actions`, `prompt-status-surface`, `diff-model`
- Branch: `external-modification-flow-task`

## Scope

Own the event-to-buffer flow: clean auto-reload, dirty externally-modified
state, footer status/actions, open-diff, keep-buffer, reversible reload, and
SSG-save event correlation.

## Files

`include/ssg/external_modification.h`, `src/external_modification.cpp`,
`tests/test_external_modification.cpp`,
`cmake/components/external-modification-flow.cmake`

## Oracle

Temporary-directory event scripts cover clean/dirty/save-correlation races and
prove each external action's document/recovery/status transition.

## Done

The mandatory workflow is complete without follow navigation.
