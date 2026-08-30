# plan-7-enforce-build-dependency-direction

## Goals

Make the intended authority layers executable in the build so reverse
dependencies fail immediately instead of relying on convention.

## Design

After prior plans remove cycles, split the broad library target into meaningful
targets for semantic core, protocol codecs, platform services, optional grid
presentation, and transports/clients. Each target owns real sources and exposes
only the headers its dependents require. Add dependency checks for forbidden
includes without creating wrapper-only libraries.

This plan begins only after Plans 1 through 6 are complete. Semantic snapshot
values remain the public core value family; protocol codecs consume those
values but do not introduce wire types into semantic behavior.

## Invariants

- LAYER-1: Semantic core has no dependency on protocol, transport, grid,
  terminal, browser, or platform-specific implementation headers. State at
  core target definition.
- LAYER-2: Protocol depends on public semantic values but semantic code cannot
  depend on codecs or transport. State at protocol target definition.
- LAYER-3: Presentation and platform adapters depend inward through typed
  interfaces and cannot own product behavior. State at adapter target APIs.
- LAYER-4: Clients depend only on semantic/protocol or presentation interfaces
  they consume. State at client target definitions.

## Considerations

First derive the actual dependency graph after earlier deletions. Shared utility
code belongs at the lowest layer whose semantics own it, not in a catch-all
target. Public installation/export and Linux/Windows builds must continue to
work. Target count is not a goal; enforce only boundaries that prohibit real
reverse dependencies.

## Risks and Mitigations

- Premature target splitting can create cyclic static libraries: move ownership
  first, split last.
- Excessive targets increase build complexity: require each target to own
  sources and forbid a documented dependency.
- Public link compatibility may change: preserve a convenience aggregate
  target only as an external linkage facade, not an internal dependency escape.

## Acceptance (Definition of Done)

- Observable: no editor behavior change; supported embedders retain a clear
  linkage target.
- Budgets: incremental and clean build times do not materially regress.
- Gates: `scripts/check.sh`, `scripts/check.sh push`, and required-platform
  builds.
- Oracles: target dependency graph assertions; forbidden-include negative
  checks; installed/public consumer compile; Linux and Windows builds.

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Record the post-refactor source/header dependency graph | CMake component manifests and dependency-check tooling | graph has no unresolved cycle for proposed layers | LAYER-1, LAYER-2 |
| 2 | Extract semantic core and platform service targets | CMake components, source ownership, public exports | core forbidden-include checks and platform builds | LAYER-1, LAYER-3 |
| 3 | Extract protocol and optional grid presentation targets | protocol/grid CMake components and callers | link graph assertions | LAYER-2, LAYER-3 |
| 4 | Rewire transports, TUI, web host, tests, and benchmarks | application/transport/test component manifests | public consumer and all test links | LAYER-4 |
| 5 | Add negative dependency checks and remove broad internal escape hatches | CMake checks, CI scripts, obsolete aggregate internals | intentional forbidden include fails | LAYER-1, LAYER-2, LAYER-4 |
| 6 | Compare build performance and keep only boundaries whose removal would permit a forbidden LAYER dependency | build scripts/documented public linkage | clean/incremental build measurements plus dependency-negative checks | LAYER-1, LAYER-4 |

## Rationale

Layering becomes durable only when the build graph prevents violations. This
plan comes last because build targets should reveal an already-correct
architecture, not simulate one around cycles.
