# file-commands

- Spec: `doc/features/recovery-file-lifecycle.md`, Plan 4 and command-handler integration
- Depends: `document-transactions`, `encoding-eol`, `recovery-actions`, `prompt-status-surface`, `core-websocket-slice`
- Branch: `file-commands-task`

## Scope

Implement directory open/replace and file new/open/recent/save/save-all/save-as/
reload/rename/delete/new-directory commands, untitled identity, path prompts,
and host-authorized local file drop as untitled content. Encoding/EOL command
sets belong to `encoding-eol`.
Export the immutable `FileCommandsCommandSet`. The implementation consumes the
Plan 3 recovery primitives and owns Plan 4 plus the feature spec's
command-handler integration. It upholds I5, I9, I16, I19, and I21.
`file.open_dropped_content` validates `InvocationPrincipal.local_file_drop`,
rejects Lua origin, applies normal decode/binary handling, and treats suggested
names as bounded display labels without path authority.

## Files

`include/ssg/workspace.h`, `include/ssg/file_commands.h`,
`src/workspace.cpp`, `src/file_commands.cpp`,
`tests/test_workspace.cpp`, `tests/test_file_commands.cpp`,
`cmake/components/file-commands.cmake`

## Oracle

Temporary-directory truth, byte-exact saves, duplicate-open prevention,
untitled identity transitions, path validation, and compensating-command
scripts on Linux/Windows. Capability fixtures accept host-granted local file
drops and reject Lua, remote, unknown, or client-asserted locality without
document/tab allocation.
Path fixtures reject absolute paths, `..` traversal, and symlinks escaping the
canonical CWD before mutation. Duplicate-open identity is the normalized
workspace-relative path, not inode identity. A failed first save preserves the
untitled ID and dirty state. Recent-file fixtures prove bounded MRU ordering.
Save-all attempts every dirty document, reports every failure, and leaves each
failed document dirty while committing successful saves.

## Done

The mandatory workflow is complete without tab ordering or watcher events.
