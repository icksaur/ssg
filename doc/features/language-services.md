# spec-language-services

## Goals

Provide search/navigation, incremental syntax, Lua command extensibility, and LSP without compromising session ordering, browser feasibility, or optional-service independence.

## Design

Search, Tree-sitter, Lua handles/capabilities, LSP version conversion, and atomic workspace edits follow `doc/spec.md`. The Lua registry exposes every required user command except the explicit I20 exclusions.

Normative commands owned by this feature:

- `palette.open`, `palette.close`, `palette.next`, `palette.previous`, `palette.execute`
- `goto.file`, `goto.line`, `goto.symbol`, `goto.definition`, `goto.reference`, `goto.matching_bracket`, `goto.back`, `goto.forward`
- `find.open`, `find.close`, `find.next`, `find.previous`, `find.toggle_case`, `find.toggle_whole_word`, `find.toggle_regex`, `find.toggle_selection`
- `replace.open`, `replace.current`, `replace.all`, `replace.workspace_preview`, `replace.workspace_apply`
- `search.workspace`, `search.results_next`, `search.results_previous`
- `completion.open`, `completion.next`, `completion.previous`, `completion.accept`, `completion.dismiss`, `hover.show`, `hover.dismiss`

Find is incremental and highlights all current-document matches with active match/count. Regex, literal, case, whole-word, and selection-limited options are typed search state. Replace-current and replace-all are atomic document transactions and one undo unit. Workspace search streams revision-tagged results into a search-results tab. Workspace replace always produces a reviewable preview; apply is one validated workspace edit with a recovery record. Zero-width regex matches must advance by one Unicode scalar to terminate.

## Invariants

I3, I5, I10, I12, I16, I20 from `doc/spec.md`.

## Considerations

- Background outputs are cancellable and revision-tagged.
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
| 1 | Implement palette, goto, current-file find/replace, and cancellable workspace search/replace | `include/ssg/search.h`, `src/search.cpp`, `tests/test_search.cpp` | independent matcher, ranking goldens, zero-width termination, and preview/apply/recovery round trips | I3, I5, I10 |
| 2 | Implement incremental Tree-sitter syntax | `include/ssg/syntax.h`, `src/syntax.cpp`, `tests/test_syntax.cpp` | full parse vs incremental parse | I10, I12 |
| 3 | Implement capability-limited Lua command parity | `include/ssg/lua.h`, `src/lua.cpp`, `tests/test_lua.cpp` | manifest parity and timeout/capability faults | I20 |
| 4 | Implement injected LSP and atomic workspace edits | `include/ssg/lsp.h`, `src/lsp.cpp`, `tests/test_lsp.cpp` | independent position fixtures and fake server | I5, I12 |

## Rationale (optional, skippable)

These optional services share revision tagging, cancellation, and command-registry integration.
