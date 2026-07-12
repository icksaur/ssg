# find-replace

- Spec: `doc/features/language-services.md`, Plan 1
- Depends: `search-palette`, `edit-command-suite`, `undo-redo-history`
- Branch: `find-replace-task`

## Scope

Implement incremental current-file find, options/count/highlights,
replace-current/all, workspace preview/apply, and recovery integration.
The component receives caller-owned current-document/selection values and a
caller-owned workspace target; it does not reach into session or filesystem
state.  Workspace apply writes a typed recovery record through an injected
sink as part of the target's atomic apply contract.

The regex matcher is a deterministic, bounded UTF-8 matcher with an explicit
work budget and cancellation check.  Budget exhaustion is a typed error; no
backtracking regex engine may run unbounded on the session executor.

Export one immutable `FindReplaceCommandSet` for all normative `find.*` and
`replace.*` commands, plus typed `FindReplaceViewState` and `FindReplaceDelta`
derive/replay values.  Workspace preview is a distinct find-replace model.

## Files

`include/ssg/find_replace.h`, `src/find_replace.cpp`,
`tests/test_find_replace.cpp`, `cmake/components/find-replace.cmake`

Do not edit `search.*`, aggregate session state, or protocol codecs.

## Oracle

Independent matcher comparisons cover literal/regex/case/word/selection and
zero-width termination/budget exhaustion; replace and workspace
preview/apply/recover round trip, including stale and injected-failure
atomicity.

## Invariants

I3, I5, I10, I12, I16, I23.

## Done

The mandatory workflow is complete with each replace operation atomic.
