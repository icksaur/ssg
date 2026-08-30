# plan-1-complete-seam-oracles

## Goals

Make architectural simplification safe by proving that every semantic session
field replays consistently across C++ and JavaScript, terminal presentation and
hit behavior remain stable, and every required platform adapter conforms to its
public interface. Establish the shared sequencing and compatibility rules used
by all later plans.

## Design

Add independent inventory, round-trip, and parity oracles before changing
ownership. The wire-field inventory is a declaration-ordered JSON manifest
consumed by C++ and Node tests; this phase will not redesign messages. Windows
adapter repair is a separate test-first commit within this plan: first add the
cross-platform interface/compile oracle, then repair only failures exposed by
that oracle.

The plans execute in numeric order. Plan 3 establishes the semantic/grid
ownership seam but retains a deprecated compatibility projection. Plan 4
replaces that projection's implementation with the UI-tree grid solver. Plan 5
migrates repeated UI identities manually against the typed contract. Plan 6
generates that established contract and removes deprecated wire fields. Plan 7
begins only after Plans 1 through 6 are complete.

All incompatible removals use one policy. A field is marked deprecated in the
wire manifest at the current version. The next named wire version removes it
after all in-tree clients exclusively produce and consume that version, and a
negotiation test rejects the prior version. External embedders opt into the new
version explicitly; no indefinite dual decoder remains. Semantic/grid and UI
identity removals are batched into `kSemanticUiWireVersion`. Further
measurement-driven protocol removals use `kGeneratedProtocolWireVersion` only
when they require another incompatible change.

Required Windows CI runs for every plan. When that CI service is unavailable,
the corresponding plan cannot merge until its Windows compile artifact is
verified manually.

## Invariants

- ORACLE-1: Every encoded semantic snapshot field has a tested delta/replay
  disposition in both C++ and JavaScript. State at the protocol schema owner.
- ORACLE-2: A rejected stale or malformed delta cannot partially mutate retained
  state. State at the replay APIs.
- ORACLE-3: Linux and Windows platform implementations expose the same public
  typed filesystem interface. State at `platform_files.h`.
- ORACLE-4: Terminal cell output and semantic hit results remain independently
  reproducible while presentation ownership changes. State at renderer/hit
  entry points.
- ORACLE-5: Compatibility paths have a named wire version, migration condition,
  and removal test. State at the wire manifest/version negotiation boundary.

## Considerations

Current JavaScript replay omits semantic sections that browser-local behavior
reads, including keymap state. Tests must perturb values, not merely count
fields. Golden terminal cases are limited to outputs later removed or moved: prompt
placement/hits, panel/tree placement/hits, notice and external-modification
placement/actions, tab placement/actions, document selection/caret roles, and
effective focus. Windows compilation must use the real platform sources rather
than textual inspection.

## Risks and Mitigations

- An inventory-count test could pass after replacing one field with another:
  compare names and independently changed values.
- Presentation goldens can freeze taste: pin semantic placement, cell roles,
  and hits only where later deletion needs an oracle.
- Cross-compilation may be unavailable locally: keep a compile-only CI target
  and make local source conformance tests deterministic.

## Acceptance (Definition of Done)

- Observable: no user-visible behavior change.
- Budgets: oracle tests remain suitable for the fast project gate.
- Gates: `scripts/check.sh` and `scripts/check.sh push`.
- Oracles: snapshot/delta/replay round trips for every semantic field;
  cross-language fixtures; terminal render/hit hand cases; required-platform
  compile; platform API symbol conformance.

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Publish the declaration-ordered JSON semantic wire-field inventory to tests | wire manifest fixture, `include/ssg/session_snapshot.h`, `src/Protocol.cpp`, `tests/test_protocol.cpp`, `tests/web/test_reconcile.mjs` | round-trip: independently perturb every field | ORACLE-1, ORACLE-2 |
| 2 | Complete browser replay for every current semantic field | `apps/web/reconcile.mjs`, `tests/web/test_reconcile.mjs`, protocol fixtures | fixture: C++ delta replay equals JavaScript replay | ORACLE-1, ORACLE-2 |
| 3 | Pin only the enumerated terminal presentation and hit seams later plans remove | `tests/test_renderer.cpp`, `tests/test_hit_test.cpp`, shell/layout focused tests | hand cases: enumerated semantic nodes produce expected roles and hits | ORACLE-4 |
| 4 | Add the required-platform filesystem interface and compile oracle | `include/ssg/platform_files.h`, platform component manifests, CI files, focused conformance tests | compile: Linux and Windows platform targets expose identical symbols | ORACLE-3 |
| 5 | Repair Windows adapter failures exposed by Step 4 as a separate reviewed change | `src/platform/windows_files.cpp`, `src/platform/windows_watcher.cpp` | the previously failing compile oracle passes | ORACLE-3 |
| 6 | Run the full gates and remove test-only duplication | affected test/component manifests | gate | ORACLE-1, ORACLE-3, ORACLE-4, ORACLE-5 |

## Rationale

The later plans delete parallel representations and change public seams. A
small exhaustive oracle layer is cheaper than debugging client divergence after
those deletions.
