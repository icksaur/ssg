# file-commands

- Spec: `doc/features/recovery-file-lifecycle.md`, Plans 3–4
- Depends: `document-transactions`, `encoding-eol`, `recovery-actions`, `prompt-status-surface`, `core-websocket-slice`
- Branch: `file-commands-task`

## Scope

Implement directory open/replace and file new/open/recent/save/save-all/save-as/
reload/rename/delete/new-directory commands, untitled identity, path prompts,
and host-authorized local file drop as untitled content. Encoding/EOL command
sets belong to `encoding-eol`.
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
scripts on Linux/Windows. Capability fixtures accept authenticated local file
drops and reject Lua, remote, unknown, or client-asserted locality without
document/tab allocation.

## Done

The mandatory workflow is complete without tab ordering or watcher events.
