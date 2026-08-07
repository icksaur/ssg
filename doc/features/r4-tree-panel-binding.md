# spec-r4-tree-panel-binding

Status: done (R4 commit 2)

## Goals

One source of truth for the correspondence between a shell **panel provider
label** (`"files"`, `"git"`, `"symbols"`) and its **tree provider** (a
`TreeProviderId` + `TreeProviderKind`). Today that correspondence is encoded
twice, in two runtime translation units, and the "activate the tree provider for
the current panel" operation is written twice with two different policies. After
this change the mapping lives in one place the tree owns, and neither runtime TU
re-spells it.

## Design

The panel↔tree correspondence is genuine cross-domain knowledge that currently
lives nowhere and is duplicated:

- `src/runtime/presentation.cpp` `syncTreeProviderToPanel(Impl&)` maps the active
  panel label to a `TreeProviderId`, activates it, and — for `git`/`symbols` —
  LAZILY CREATES the provider (via `tree.replaceProvider` with a fresh
  `nextTreeRevision`) when it is not yet present, then activates. Returns bool.
  Called from `shellCommand` on `panel.show_files`, `panel.show_git_status`,
  `panel.next_provider`, `panel.previous_provider` (4 sites).
- `src/runtime/navigation.cpp` `panelProviderTreeId(label)` maps the same label
  to the same `TreeProviderId`, and `treeCommand`'s prologue best-effort
  activates it (no create, failure ignored) before running a tree command.

The duplicated fact is the label→(id, kind) table. `TreeProviderId` and
`TreeProviderKind` are `TreeModel`'s vocabulary; the panel label is a plain
string key both runtime files already hold (`shell.activePanelProvider()`).
`editor_runtime_internal.h` already includes `TreeModel.h`, so `TreeModel` is a
reachable home for both call sites.

Mechanism (chosen, per review): the label→binding table is DATA in the runtime
composition seam, and `TreeModel` gains a TYPED activate-or-create that never
sees a shell label.

`TreeModel` owns arbitrary tree providers; it must not learn `ShellState`'s
presentation vocabulary (`"files"`/`"git"`/`"symbols"`). So:

- The typed binding is tree vocabulary, declared in `TreeModel.h`:

      struct TreeProviderBinding { TreeProviderId id; TreeProviderKind kind; };

- `TreeModel` gains the activate-or-create, taking the typed binding — NO label:

      // Activate the provider named by `binding`. When it does not exist yet and
      // its kind is git/symbols, create it empty at `revisionIfCreated` and
      // activate it (the panel can be shown before the provider has content). A
      // Filesystem binding is NEVER created here -- it is seeded at construction,
      // so a missing one is a genuine failure. Returns false when activation
      // fails and nothing was created.
      bool activateOrCreate(const TreeProviderBinding& binding,
                            TreeRevision revisionIfCreated);

- The label→binding mapping is an internal value-conversion in the runtime seam
  (`editor_runtime_internal.h`, which both TUs already include) — an immutable
  binding value is data, not ceremony:

      [[nodiscard]] std::optional<TreeProviderBinding>
          panelProviderBinding(std::string_view panelLabel);

`syncTreeProviderToPanel` collapses to: look up the binding for
`shell.activePanelProvider()`; return false if none; else
`tree.activateOrCreate(*binding, TreeRevision{nextTreeRevision++})`. The
create-on-activate sequence moves onto `TreeModel` next to
`replaceProvider`/`activateProvider`; the shell↔tree label adapter stays in the
runtime seam where shell and tree vocabularies legitimately meet.

`treeCommand`'s prologue uses the SAME `panelProviderBinding` map but keeps its
existing policy — a best-effort plain `activateProvider(binding->id)` with NO
create (see Considerations: this is mandatory, not optional).

Rejected alternative: a `TreeModel::activatePanelProvider(std::string_view
label)` taking the shell label — it couples `TreeModel` to `ShellState`'s
presentation vocabulary, which the tree has no business knowing. Also rejected: a
free `panelTreeProvider()` in a public header (homeless free function carrying
domain behavior) or a new `PanelTreeBinding` object (owns no state `TreeModel`
doesn't; ceremony, the R4 anti-pattern).

## Invariants

- Behavior-preserving. In particular the `nextTreeRevision` source stays Impl's
  counter (passed in), NOT TreeModel's private `revision_` — unifying those two
  revision sources is a separate concern, out of scope here.
- Single source: after this change the `files/git/symbols` → provider table
  exists in exactly one place; no runtime TU re-spells it.
- No ceremony: the new surface on `TreeModel` earns its place by owning the
  provider-lifetime logic that was inline in a runtime handler, not by wrapping
  `Impl&`.
- One identifiable change per commit; `bash scripts/check.sh` green.

## Considerations

- **The two call sites have different policies, and that difference is
  LOAD-BEARING — `treeCommand` must NOT gain create-on-miss.**
  `syncTreeProviderToPanel` creates-then-activates; `treeCommand`'s prologue only
  activates. Tree commands are dispatchable while the panel is HIDDEN, or before
  `panel.show_git_status` has ever run, where the current behavior RETAINS
  whatever provider is active. If `treeCommand` used create-on-miss it would
  instead switch to a freshly-created empty git/symbols provider in exactly those
  cases — a behavior change. So `treeCommand` keeps plain
  `activateProvider(binding->id)` (map shared, policy unchanged); unifying the two
  call sites onto `activateOrCreate` is explicitly rejected. This is mandatory,
  not a conservative default.
- **Symbols panel.** `panelProviderBinding("symbols")` returns kind Symbols; the
  create path mirrors the existing git branch. Confirm the `symbols` panel label
  is actually reachable (it is listed in the map today) — if the shell never
  offers it, the binding is harmless but the create branch is dead; keep it for
  parity with the current code, which already handles it.
- **Filesystem is never created here** — it is seeded at construction
  (`EditorRuntime.cpp:1723`). `activatePanelProvider` must NOT create a
  filesystem provider; a missing one is a genuine failure (return false), exactly
  as `syncTreeProviderToPanel` returns false for an unknown label today.

## Risks and Mitigations

- Risk: `treeCommand` accidentally gains create-on-miss and switches to an empty
  provider when a tree command fires with the panel hidden. Mitigation: it keeps
  plain `activateProvider` (no create); a direct-tree-command-while-panel-hidden
  test pins that the active provider is retained.
- Risk: the empty-provider revision differs from today. Mitigation: pass
  `nextTreeRevision++` exactly as the current code does; assert via the existing
  panel/tree snapshot tests that no golden regenerates.

## Acceptance (Definition of Done)

- Observable: the `files/git/symbols` → tree-provider table appears once
  (`panelProviderBinding` in the runtime seam); `presentation.cpp` and
  `navigation.cpp` no longer spell it. `syncTreeProviderToPanel` and
  `panelProviderTreeId` are gone; the create-on-activate logic lives on
  `TreeModel::activateOrCreate`.
- Budgets: n/a.
- Gates: `bash scripts/check.sh` green (101 tests, 0 warnings), no golden
  regeneration.
- Oracles:
  - binding-table oracle (integration, not a unit test): `panelProviderBinding`
    lives in the runtime seam (`editor_runtime_internal.h`), which the unit tests
    do not include. Its oracle is the runtime navigation/presentation integration
    suite, which drives `panel.show_files`/`panel.show_git_status`/
    `panel.next_provider` and asserts the tree switches to the matching provider
    — a stronger end-to-end check than a table unit test, and it already exists.
  - activate-or-create test (on `TreeModel`, covering BOTH creatable kinds):
    - git-on-miss: with only a filesystem provider, `activateOrCreate({git,
      Git}, rev)` creates+activates a git provider at `rev`, returns true, and
      the active provider is the git one at `rev`.
    - symbols-on-miss: likewise `activateOrCreate({symbols, Symbols}, rev)`
      creates+activates a symbols provider at `rev`, returns true, active
      provider is symbols at `rev`.
    - filesystem-never-created: with no filesystem provider,
      `activateOrCreate({filesystem, Filesystem}, rev)` returns false and creates
      nothing.
    - already-present: `activateOrCreate` of an existing provider activates it
      and does NOT bump its revision (no spurious replace).
  - tree-command-retains-provider (structural, not a distinguishing test): the
    "panel label is git/symbols but that provider is absent" state is UNREACHABLE
    via the public API — the shell is built with providers {files,git,symbols} at
    index 0 (files, whose filesystem provider is seeded at construction), and
    every command that advances the index to git/symbols
    (`panel.show_git_status`, `panel.next_provider`, `panel.previous_provider`)
    also syncs+creates that provider. So create-on-miss vs no-create in
    `treeCommand` behave identically in every reachable state, which is exactly
    why keeping the old no-create policy is behavior-preserving. The guarantee is
    structural (`treeCommand` calls plain `activateProvider`, never
    `activateOrCreate`); the existing tree-command suite (running with the
    filesystem provider active) confirms no regression.
  - existing panel/tree behavior: the shell-command panel tests and the
    tree-command tests stay green unchanged.

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Add `TreeProviderBinding` + `TreeModel::activateOrCreate(binding, revIfCreated)` (activate; create git/symbols on miss; Filesystem never created; existing provider not re-revisioned) | `include/ssg/TreeModel.h`, `src/TreeModel.cpp` | activate-or-create test (git + symbols + filesystem-false + already-present), fails first | behavior-preserving; no-ceremony |
| 2 | Add `panelProviderBinding(std::string_view)` value-mapping in the runtime seam (the one table) | `src/runtime/editor_runtime_internal.h` (+ defn) | binding-table test | single-source |
| 3 | Replace `syncTreeProviderToPanel` with binding+`activateOrCreate` at its 4 sites; delete the helper | `src/runtime/presentation.cpp` | shell-command panel tests green | single-source |
| 4 | Replace `panelProviderTreeId` in `treeCommand` with `panelProviderBinding`, keeping plain `activateProvider` (NO create); delete the helper | `src/runtime/navigation.cpp` | tree-command-retains-provider-when-hidden + tree-command tests | behavior-preserving (mandatory no-create) |
| 5 | Gate + commit as ONE "consolidate panel↔tree binding" change | — | full suite green | one change per commit |

## Rationale (optional)

This is the kind-2 target the R4 spec reserved: not a ceremony controller, but a
genuine split-logic consolidation. The correspondence between panels and tree
providers is a fact about the product that two runtime files currently each
assert independently — the classic "add a provider, update it in two places or
they drift" hazard. Moving it onto `TreeModel` (which already owns provider
identity, kind, and lifetime) gives it the one home cpp-values wants, and folds
the create-on-activate sequence next to `replaceProvider`/`activateProvider`
where provider lifetime already lives.
