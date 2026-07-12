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
`tests/test_http.cpp`. The child does not create
`tasks/dependencies/http.commit`; after integration, the parent records the
merged HTTP commit on SSG `master`.

## Oracle

The platform-independent complete-write script injects partial, timeout,
closed, and error attempts deterministically. Identical native loopback frame
and connection-lifecycle scripts run on Linux and Windows. The child runs Linux
runtime gates and a source/build check with an already-installed Windows
cross-compiler when available; native Windows CI is authoritative.

## Done

The mandatory workflow is complete in both task worktrees. The parent merges
and gates HTTP first, records its merged SHA in SSG, then merges SSG.
