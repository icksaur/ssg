# prompt-status-surface

- Spec: `doc/features/presentation-shell.md`, Plan 2
- Depends: `shell-layout`
- Branch: `prompt-status-surface-task`

## Scope

Implement non-modal one-to-three-row prompt surfaces and the bounded actionable
footer status queue with navigation/dismiss/invoke commands.

## Files

`include/ssg/prompt.h`, `include/ssg/status.h`, `src/prompt.cpp`,
`src/status.cpp`, `tests/test_prompt_status.cpp`,
`cmake/components/prompt-status-surface.cmake`

## Oracle

Geometry goldens, priority/queue transition tables, stale action rejection, and
accessible-label snapshots.

## Done

The mandatory workflow is complete without file/find-specific prompt policy.
