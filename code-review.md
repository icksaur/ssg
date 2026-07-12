# EditorRuntime code review

Reviewer: Claude Opus 4.8

MUST **bug / I2 / I16** — `src/runtime/files.cpp:194-198` — The four
encoding commands are success-shaped no-ops. They must invoke real encoding
operations, update snapshot state, and affect saved bytes.

MUST **bug / I2 / I16** — `src/runtime/presentation.cpp:110-112` —
`settings.set`, `settings.reset`, and `settings.reset_scope` do not call the
`SettingsModel` mutation operations. They must mutate settings from typed
command arguments and expose the result in snapshots.

MUST **bug / I2** — `src/runtime/editing.cpp:198-200` —
`replace.workspace_preview` and `replace.workspace_apply` always fail despite
the existing workspace replace operations. They must dispatch to those
feature-owned operations.

SHOULD **workspace boundary** — `src/editor_runtime.cpp:162-172` — Workspace
search enumerates default `.ssg/scratch` and `.ssg/recovery` state beneath the
CWD. Exclude runtime-owned state so search and replace cannot expose or rewrite
recovery journals.
