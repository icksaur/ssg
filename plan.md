# spec-editor-boundaries

## Goals
SSG preserves its current behavior while making editor changes safer to implement:
cross-model maintenance has one named owner, components owned by `Editor` do not
reach back through it to mutate sibling components, and presentation consumes one
consistent immutable frame instead of inspecting the live editor aggregate.

## Design
`Editor` remains the flat session coordinator. It is not replaced with an `App`,
a service graph, or a set of one-method interfaces. The targeted mechanism is to
extract coherent operations at the places where callers currently must remember
maintenance work, then make only the state protected by those operations private.

Document removal uses the existing `discardDocumentRuntimeState` operation after
every successful workspace removal. That operation owns cancellation and removal
of all per-document runtime associations. Search submission becomes one
editor-owned operation covering cancellation, generation, search state, and tree
publication; input routing only translates the typed input into that operation.

`GitDiffIngress` stops retaining `Editor&`. It owns and drains the worker and
returns data. `Editor` applies the resulting batch because workspace, tabs,
screen, tree, syntax, and navigation are independent responsibilities whose
coordination is a session policy. Diff/follow staging remains a pure helper that
returns effects before any editor state is adopted. Do not introduce a
`ChangeTracking` aggregate until diff, follow, and external-modification state
have a lifecycle that can be owned together without exposing their internals.

Presentation receives an `EditorFrameState` captured by `Editor` while holding
the operation lock. `GridPresenter` solves geometry from that value and its
presenter-owned viewport state; it no longer locks or reads `Editor`. This is a
data boundary, not a second mutable model.

After each operation moves behind its boundary, the fields it protects become
private. Command source files may continue to include `Editor.h` and coordinate
independent product choices. The goal is not zero includes; it is that a caller
cannot partially perform an operation with hidden maintenance obligations.

The remaining work applies the same rule to three protocols found by the final
coupling review. Find/replace operations own the document identity their byte
offsets describe. External-modification actions have one validator/executor used
by typed and command entry points. Tree activation has one provider-aware
operation used by keyboard and pointer routes. Keymap adoption remains a local
helper because it owns no broader lifecycle.

## Invariants
- **Session coordination:** `Editor` coordinates independent models; `main.cpp`
  owns process composition and lifecycle. State this on `Editor` and in
  `AGENTS.md` if its existing structure contract needs clarification.
- **One-way ownership:** a component stored by `Editor` never stores an
  `Editor` pointer or reference and never mutates sibling components. State this
  on `GitDiffIngress`.
- **Complete document teardown:** once a workspace document is removed, no
  history, syntax request, language override, find association, or content-side
  map may retain its identity. State this on
  `Editor::discardDocumentRuntimeState`.
- **Atomic scan adoption:** a rejected git scan changes no diff/follow state;
  accepted effects are applied in revision order before publication. State this
  on the scan-staging helper and `GitDiffIngress` result.
- **Consistent frames:** every `EditorFrameState` is captured under one operation
  lock and is immutable to presentation. State this on `EditorFrameState` and
  its capture operation.
- **Behavior parity:** command results, focus, tab behavior, rendered output,
  watcher handling, and recovery semantics do not change during this work.
  Preserve this through the existing public contracts and focused tests.
- **Find identity:** every accepted operation that creates or recomputes find
  matches records the document identity in the same operation; closing find
  clears it. State this on the editor-owned find operation.
- **Equivalent activation:** keyboard and pointer activation of the same tree
  node execute the same provider-specific operation and produce equivalent
  navigation/focus results. State this on `activateTreeNode`.
- **Equivalent external actions:** command and typed invocation of the same
  offered external-modification action share validation and effects. State this
  on the common external-action operation.

## Considerations
- `Editor` is a legitimate aggregate root. Moving coordination out merely to
  reduce its line count would hide the product flow without reducing coupling.
- `GitDiffIngress` currently forms an ownership cycle and directly touches
  external modifications, diff/follow, tree, tabs, workspace, syntax, screen,
  selection, and document runtime maps.
- Editing commands repeatedly combine document/history mutation, selection
  repair, tab publication, syntax refresh, find reconciliation, and follow
  notification. This plan first consolidates teardown and the existing
  transaction-finalization path; a general edit transaction is warranted only
  if the remaining call sites share one contract after those changes.
- `GridPresentation` contains solved geometry and presenter state, so it is not
  the editor-frame boundary itself. `EditorFrameState` contains only the
  immutable editor-owned inputs needed to produce it.
- The previous recovery spec mixed implemented foundations with unimplemented
  checkpoint and shutdown promises. It is replaced rather than carried forward;
  any resumed recovery work requires a fresh spec against the current code.
- Header size is an outcome, not an oracle. Removing transitive includes without
  establishing ownership is not an architecture improvement.
- Existing direct model access in command files is acceptable when it keeps an
  independent product choice visible. The remaining findings are narrower:
  repeated assignments or duplicated branches that maintain one invariant are
  not visible coordination.

## Risks and Mitigations
- Moving ingress coordination into `Editor` could enlarge the coordinator.
  Keep worker mechanics in `GitDiffIngress`, scan staging independent of
  `Editor`, and only cross-model effect adoption in `Editor`.
- A frame snapshot can accidentally copy expensive text or models more than
  once. Reuse immutable/shared view values and move the captured frame into the
  presenter; compare the presentation path with the current behavior.
- Privatizing fields too early can produce pass-through accessors. Make a field
  private only after all external mutations are replaced by a coherent
  operation; do not add accessors solely to preserve old reach-through.
- Consolidating cleanup can expose stale identities that current partial cleanup
  leaves behind. Add lifecycle tests before changing call sites and make
  teardown idempotent where repeated cleanup is valid.
- Behavior-preserving movement across translation units can alter lock scope.
  Capture frames and drain/apply ingress under the same operation boundaries as
  today, and test that worker I/O never runs while the operation lock is held.
- Consolidating keyboard and pointer activation can accidentally erase
  entry-point-specific routing mechanics. Share node interpretation and action
  selection, while leaving transport-specific result wrapping at the caller.
- Moving find identity into an owned operation can update it on rejected work.
  Adopt the identity only after the find controller accepts the new state.

## Acceptance (Definition of Done)
- Observable: Existing keyboard, mouse, tab, search, git-diff, external-change,
  and rendering behavior is unchanged; no visual signoff is required.
- Budgets: Presentation does not add another full document-text copy or hold the
  operation lock during layout/rendering; git worker and filesystem I/O remain
  outside the operation lock.
- Gates: `cmake --build build` and `ctest --preset dev` are green after every
  independently landed step; `ctest --preset all` is green before merge.
- Oracles: document close, delete, and live-diff replacement tests prove all
  per-document associations are removed; workspace-search tests compare submit,
  replacement, cancellation, stale completion, and publication behavior;
  ingress tests feed ordered, stale, rejected, and watcher-only batches and
  compare published diff/follow/tree/tab effects; presentation tests compare
  frames and rendered grids before and after the boundary; existing command,
  input, session, diff, follow, external-modification, and render suites provide
  parity coverage; find tests prove every match-producing operation retains its
  document association; paired keyboard/pointer and command/typed fixtures prove
  equivalent activation and external-action behavior.

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Route every successful workspace-document removal through the canonical teardown operation, make it safe for all removal paths, and remove direct runtime-map erasure. | `include/ssg/Editor.h`, `src/Editor.cpp`, `src/files.cpp`, `src/GitDiffIngress.cpp`, `tests/test_command.cpp`, `tests/test_diff.cpp` | lifecycle tests: close, delete, read-only replacement, and live-diff replacement leave no per-document associations or syntax work | Complete document teardown, Behavior parity |
| 2 | Make search submission and cancellation one editor operation; leave routing responsible only for typed mutation dispatch. | `include/ssg/Editor.h`, `src/Editor.cpp`, `tests/test_input.cpp`, `tests/test_search.cpp` | state-transition tests: replacement, empty query, cancellation, stale completion, and current-generation publication match existing behavior | Session coordination, Behavior parity |
| 3 | Remove the `GitDiffIngress` back-reference by returning drained worker data, stage diff/follow changes without `Editor`, and adopt cross-model effects in the editor coordinator. | `include/ssg/GitDiffIngress.h`, `src/GitDiffIngress.cpp`, `include/ssg/Editor.h`, `src/Editor.cpp`, `CMakeLists.txt`, `cmake/ssg_tests.cmake`, `tests/test_git_diff_ingress.cpp`, `tests/test_diff.cpp`, `tests/test_follow_edits.cpp` | batch fixtures: stale/rejected scans leave state unchanged; accepted scans publish matching diff, follow, tree, live-tab, and navigation effects; watcher-only batches preserve external-change behavior | One-way ownership, Atomic scan adoption, Session coordination, Behavior parity |
| 4 | Introduce immutable editor frame capture and make `GridPresenter` consume it without accessing or locking `Editor`. | `include/ssg/EditorFrameState.h`, `include/ssg/Editor.h`, `include/ssg/GridPresenter.h`, `src/EditorViews.cpp`, `src/GridPresenter.cpp`, `src/TerminalClient.cpp`, `tests/test_grid_presentation_builder.cpp`, `tests/test_terminal_client.cpp`, `tests/test_render.cpp` | frame fixtures and existing grid/render tests produce equivalent document, selection, syntax, tabs, UI tree, overlays, theme, clipboard, and layout output; a controlled operation proves capture is internally consistent | Consistent frames, Behavior parity |
| 5 | Centralize post-operation reconciliation already shared by command and typed-input paths, then move protected caches and state behind the resulting operations without pass-through accessors. | `include/ssg/Editor.h`, `src/Editor.cpp`, `src/editing.cpp`, `src/files.cpp`, `src/navigation.cpp`, `src/ViewCommands.cpp`, `src/ExternalModificationFlow.cpp`, `tests/test_command_dispatch.cpp`, `tests/test_input.cpp`, `tests/test_follow_edits.cpp`, `tests/test_find_replace.cpp` | operation-boundary tests: accepted and rejected edit, file, activation, and external-change paths perform find reconciliation, external-presence refresh, and local-edit follow notification exactly once | Session coordination, Complete document teardown, Behavior parity |
| 6 | Remove obsolete includes and public mutation surfaces made unnecessary by the preceding steps; retain direct model access where it represents visible coordination rather than a maintenance obligation. | `include/ssg/Editor.h`, `include/ssg/GitDiffIngress.h`, `include/ssg/GridPresenter.h`, `src/GitDiffIngress.cpp`, `src/GridPresenter.cpp`, and command sources changed by earlier steps | a clean build and the full test gates pass after obsolete includes and access paths are removed; compiler dependency output confirms `GitDiffIngress` and `GridPresenter` no longer depend on `Editor.h` | One-way ownership, Session coordination, Behavior parity |
| 7 | Replace direct `findDocumentId` maintenance with editor-owned find operations that atomically update or clear controller state and its document association. | `include/ssg/Editor.h`, `src/Editor.cpp`, `src/editing.cpp`, `tests/test_find_replace.cpp`, `tests/session/test_session_editing.cpp` | table-driven find fixtures exercise open, open-selection, replace, query/replacement update, option toggles, close, document switch, and rejected operations; every accepted match-producing operation remains open through the next reconciliation and every stale association closes | Find identity, Session coordination, Behavior parity |
| 8 | Route typed and command external-modification actions through one validator/executor while preserving the selected-file command contract. | `include/ssg/ExternalModificationFlow.h`, `src/ExternalModificationFlow.cpp`, `include/ssg/Editor.h`, `src/Editor.cpp`, `tests/test_external_modification.cpp`, `tests/session/test_session_external_modification.cpp` | paired fixtures invoke Reload, KeepBuffer, and OpenDiff through typed and command entry points and compare acceptance, workspace bytes, pending files, diff/live-tab state, syntax revision, and focus; unavailable actions reject identically | Equivalent external actions, Session coordination, Behavior parity |
| 9 | Make keyboard and pointer tree activation call one provider-aware node operation; preserve deferred view-action wrapping only where the dispatch transport requires it. | `include/ssg/Editor.h`, `src/navigation.cpp`, `tests/session/test_session_navigation.cpp`, `tests/test_input.cpp` | paired fixtures activate directory, filesystem file, git file, and search result through keyboard and pointer routes and compare tree expansion, active tab/document, selection, focus, and final navigation target | Equivalent activation, Session coordination, Behavior parity |
| 10 | Adopt keymap mutations through one local helper that updates the keymap and its routing generation together. | `src/ViewCommands.cpp`, `tests/test_command_dispatch.cpp`, `tests/test_input.cpp` | bind and unbind tests prove the next routed key uses the new catalog and each accepted mutation advances the routing generation once; rejected mutations change neither | Behavior parity |

## Rationale
The broad `Editor.h` surface is a symptom, not the primary defect. The concrete
defects are duplicated lifecycle cleanup, a child component that reaches back
into its owner, routing code that maintains a search protocol, and presentation
that reads a live aggregate. Fixing those obligations first creates natural
private boundaries. A wholesale facade or interface-per-model rewrite would
mostly rename access while adding ownership depth, contrary to the project's
flat composition goals.

The order is intentionally incremental. The teardown and search changes are
small proofs that operation boundaries reduce obligations at call sites. The
ingress change removes the strongest dependency cycle. Immutable frame capture
then removes the widest read-only dependency. Only after those seams exist does
privatization become useful rather than cosmetic.
