# spec-language-services

## Goals

Provide search/navigation, incremental syntax, Lua command extensibility, and LSP without compromising session ordering, browser feasibility, or optional-service independence.

## Design

Search, Tree-sitter, Lua handles/capabilities, LSP version conversion, and atomic workspace edits follow `doc/spec.md`. The Lua registry exposes every required user command except the explicit I20 exclusions.

Plan 1 keeps optional-service boundaries explicit. Search receives a
caller-owned `SearchCommandSource` that exposes immutable registered-command
descriptors plus dispatch, and `palette.execute` uses only that seam. It also
receives a caller-owned `SearchWorkspaceSource` that supplies revision-stable
workspace-relative file/content and symbol snapshots. Search therefore neither
owns nor reaches into the session registry, filesystem, Tree-sitter, or LSP.
It exports an immutable `SearchCommandSet`, typed `SearchViewState`, and
`SearchDelta` derivation/replay for later session aggregation.

Syntax parsing is an optional injected service. The core owns revision-tagged
parse requests, cancellation, plain-text fallback, immutable syntax snapshots,
and delta derivation; a host-supplied `SyntaxParser` owns the Tree-sitter
runtime, grammars, queries, and parse-tree lifetime. Constructing or using SSG
without a parser performs no Tree-sitter initialization and adds no mandatory
Tree-sitter link dependency. Grammar availability is reported per language, and
an unavailable or failed grammar produces the same deterministic plain-text
snapshot. The parser input contains the prior accepted parse plus byte edits so
Tree-sitter adapters can update trees incrementally.

This step exports immutable `SyntaxViewState` and `SyntaxDelta` values with pure
derive/replay functions for later session assembly. It also exports syntax
roles, bracket pairs/matches, comment tokens/ranges, and line indentation
metadata. It does not edit session aggregates, protocol codecs, LSP, or
rendering. The full-versus-incremental oracle uses an injected deterministic
parser fixture; this keeps the oracle unconditional while proving the exact
request/result contract a separately linked Tree-sitter adapter implements.

Normative commands owned by this feature:

- `palette.open`, `palette.close`, `palette.next`, `palette.previous`, `palette.execute`
- `goto.file`, `goto.line`, `goto.symbol`, `goto.definition`, `goto.reference`, `goto.matching_bracket`, `goto.back`, `goto.forward`
- `find.open`, `find.close`, `find.next`, `find.previous`, `find.toggle_case`, `find.toggle_whole_word`, `find.toggle_regex`, `find.toggle_selection`
- `replace.open`, `replace.current`, `replace.all`, `replace.workspace_preview`, `replace.workspace_apply`
- `search.workspace`, `search.results_next`, `search.results_previous`
- `completion.open`, `completion.next`, `completion.previous`, `completion.accept`, `completion.dismiss`, `hover.show`, `hover.dismiss`

LSP is split into separately reviewable synchronization/diagnostics, language
feature, and workspace-edit components. The synchronization component owns
injected stream framing, bounded initialize/shutdown, document version mapping,
request cancellation, UTF-8/UTF-16 boundary conversion, and bounded,
coalesced diagnostics. It exports immutable `LspSyncViewState` and
`LspSyncDelta` values with pure derive/replay functions for later session
assembly; feature tasks do not edit aggregate session or protocol codec files.
Constructing or using SSG without an injected LSP stream performs no process or
LSP initialization and adds no mandatory LSP runtime dependency. Slow, hung,
cancelled, or malformed servers cannot block the session indefinitely or
publish partial state.

Find is incremental and highlights all current-document matches with active match/count. Regex, literal, case, whole-word, and selection-limited options are typed search state. Replace-current and replace-all are atomic document transactions and one undo unit. Workspace search streams revision-tagged results into a search-results tab. Workspace replace always produces a reviewable preview; apply is one validated workspace edit with a recovery record. Zero-width regex matches must advance by one Unicode scalar to terminate.

## Invariants

I3, I5, I10, I12, I16, I20 from `doc/spec.md`.

## Considerations

- Background outputs are cancellable and revision-tagged.
- Search requests and batches carry a monotonic generation and source revision;
  cancelled, superseded, or stale-revision batches are discarded.
- Goto Anything modes are unprefixed file matching, `@` injected symbols, `:`
  one-based lines, and `#` literal workspace-text search. Regex and zero-width
  matching belong to current-file find/replace, not workspace text mode.
- Goto and search-result choices are classified as user navigation and request
  primary-caret reveal; later assembly pauses follow-edits and delegates
  minimal reveal to the view owner. Programmatic navigation remains distinct.
- Lua callbacks have instruction/time budgets and capability checks.
- LSP process launch remains an injected host adapter.

## Risks and Mitigations

- Stale output: discard at the session boundary.
- Extension failure: isolate errors and preserve transaction atomicity.

## Acceptance (Definition of Done)

- Observable: search, syntax, Lua, and LSP commands produce API deltas without requiring their services at basic editor construction.
- Budgets: no extension blocks the session indefinitely.
- Gates: service unit/integration tests and sanitizers are green.
- Oracles: search ranking goldens, full-vs-incremental parse, required-command Lua parity, scripted fake LSP, and workspace-edit fault injection.

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Implement palette, goto, current-file find/replace, and cancellable workspace search/replace | `include/ssg/search.h`, `src/search.cpp`, `tests/test_search.cpp` | independent matcher, ranking/query goldens, cancellation/stale batches, navigation transition/caret-reveal tables, zero-width termination, and preview/apply/recovery round trips | I3, I5, I10, I12, I16, I23 |
| 2 | Implement optional incremental Tree-sitter syntax and typed syntax snapshot/delta seams | `include/ssg/syntax.h`, `src/syntax.cpp`, `tests/test_syntax.cpp` | injected deterministic parser full-vs-incremental snapshots, stale cancellation, and plain-text fallback | I10, I12 |
| 3 | Implement capability-limited Lua command parity | `include/ssg/lua.h`, `src/lua.cpp`, `tests/test_lua.cpp` | manifest parity and timeout/capability faults | I20 |
| 4a | Implement injected LSP framing, lifecycle, document synchronization, cancellation, and bounded/coalesced diagnostics | `include/ssg/lsp_sync.h`, `src/lsp_sync.cpp`, `tests/fake_lsp_server.*`, `tests/fixtures/lsp/sync/`, `tests/test_lsp_sync.cpp` | scripted fake server, independent UTF-8/UTF-16 fixtures, version/stale diagnostics, cancellation/timeouts, bounds, malformed messages, and nested-consumer optional-linkability | I10, I12 |
| 4b | Implement LSP completion, hover, definition, and references | `include/ssg/lsp_features.h`, `src/lsp_features.cpp`, `tests/test_lsp_features.cpp`, plus an additive completed-response/document-snapshot seam in `lsp_sync.*` | scripted fake-server request/response cases, stale/cancelled result rejection, completion ordering/acceptance, and navigation fixtures | I10, I12 |
| 4c | Implement atomic LSP workspace edits | `include/ssg/lsp_workspace_edit.h`, `src/lsp_workspace_edit.cpp`, `tests/test_lsp_workspace_edit.cpp` | full validation, fault injection, and all-or-nothing document snapshots | I5, I10, I12 |

Plan 4b consumes responses only through `LspSyncClient`: the synchronization
layer exposes completed request IDs with raw response payloads and immutable
document snapshots containing URI, revision, server version, and exact
synchronized text. The feature controller retains request kind, URI, revision,
generation, and cancellation/supersession state, so late results cannot update
observable state. Request errors are correlated without failing the connection.
Rename remains out of Plan 4b because applying its `WorkspaceEdit` belongs to
Plan 4c.

## Rationale (optional, skippable)

These optional services share revision tagging, cancellation, and command-registry integration.
