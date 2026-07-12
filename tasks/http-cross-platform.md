# http-cross-platform

- Spec: `doc/features/session-protocol.md`, Plan 3
- Depends: `foundation-harness`
- Branch: `http-cross-platform-task`
- Exclusive: owns paired isolated SSG/HTTP worktrees

## Scope

Port the required HTTP server/WebSocket socket seam to Windows and add
deadline-aware complete writes with typed timeout/closed/error results on both
platforms. The parent creates sibling isolated task worktrees named `ssg` and
`http`; both use `http-cross-platform-task`, and relative `../http` from the SSG
worktree resolves to the isolated HTTP worktree.

## Files

The paired HTTP worktree's `http.h`, `http.cpp`, `src/platform/`, and
`tests/test_http.cpp`; SSG records the parent's merged HTTP commit in
`tasks/dependencies/http.commit`.

## Oracle

Identical Linux/Windows partial-write, deadline, close, frame, and connection
lifecycle scripts.

## Done

The mandatory workflow is complete in both task worktrees. The parent merges
and gates HTTP first, records its merged SHA in SSG, then merges SSG.
