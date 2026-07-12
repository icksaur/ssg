# session-state

- Spec: `doc/features/session-protocol.md`, Plan 1
- Depends: `document-transactions`
- Branch: `session-state-task`

## Scope

Implement the serialized session command executor, client identities,
revision ordering, active workspace/view topology, stale-command rejection,
immutable `InvocationPrincipal` capability context, and explicit
duplicate-rejecting `CommandRegistry`/`CommandSet` contracts.

## Files

`include/ssg/session.h`, `src/session.cpp`,
`include/ssg/command_registry.h`, `src/command_registry.cpp`,
`tests/test_session.cpp`,
`cmake/components/session-state.cmake`

## Oracle

Hand-authored state-machine scripts prove total ordering, stale rejection,
client isolation, failure atomicity, duplicate registration rejection, and
dispatch through registered handlers with identical direct/WebSocket principal
capability enforcement.

## Done

The mandatory workflow is complete without snapshot/delta serialization.
