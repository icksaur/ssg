# search-palette

- Spec: `doc/features/language-services.md`, Plan 1
- Depends: `document-transactions`, `prompt-status-surface`
- Branch: `search-palette-task`

## Scope

Implement command palette state, Goto Anything file/line/symbol modes,
navigation history, and cancellable ranked workspace search results.

`SearchCommandSource` is an injected, read-only source of registered command
descriptors and the sole dispatch seam used by `palette.execute`; this component
does not reach into `EditorSession` or `CommandRegistry`. `SearchWorkspaceSource`
is an injected snapshot provider for workspace-relative file content and symbol
locations, so search remains independent of filesystem and syntax services.
Both interfaces have caller-owned lifetime.

The component exports one immutable `SearchCommandSet`, typed
`SearchViewState`, and revision-based `SearchDelta` derivation/replay. Background
requests and batches carry both a request generation and source `Revision`.
Starting a replacement request cancels the prior token, and publication rejects
cancelled, superseded, or stale-revision output.

Goto and search-result choices return a navigation transition classified as
user navigation, with an explicit request to reveal the primary caret. Session
assembly uses those flags to pause follow-edits and delegate minimal caret
reveal to the view owner. Programmatic navigation is separately classified and
does not request a follow-edits pause.

Goto Anything uses the accepted modes: unprefixed text ranks files, `@` ranks
injected symbols, `:` parses a one-based line target, and `#` ranks literal
workspace-text matches. Regex and find/replace mutation remain owned by the
dependent `find-replace` task.

## Files

`include/ssg/search.h`, `src/search.cpp`, `tests/fixtures/search/`,
`tests/test_search.cpp`, `cmake/components/search-palette.cmake`

## Oracle

Accepted ranking/query goldens, cancellation/stale-result cases, and
back/forward navigation transition tables. Navigation tables also assert the
user-navigation classification and caret-reveal request.

## Invariants

I3, I10, I12, I16, I20, and I23 from `doc/spec.md`.

## Done

The mandatory workflow is complete without find/replace mutation.
