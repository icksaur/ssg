# lua-command-host

- Spec: `doc/features/language-services.md`, Plan 3
- Depends: `session-state`, `required-command-catalog`, `core-websocket-slice`
- Branch: `lua-command-host-task`

## Scope

Implement per-session Lua state, generational handles, capability grants,
budgets, command registration/invocation, and required-command parity.

## Files

`include/ssg/lua.h`, `src/lua.cpp`, `tests/plugins/`,
`tests/test_lua.cpp`, `cmake/components/lua-command-host.cmake`

## Oracle

Required catalog minus explicit exclusions is callable; timeout, stale-handle,
capability denial, and atomic-edit faults remain isolated.

## Done

The mandatory workflow is complete without package discovery or native modules.
