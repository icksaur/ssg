# spec-syntax-parse-convenience

Status: done (R3 part 2)

## Goals

A caller that parses synchronously names ONE operation instead of hand-driving
`request() -> run() -> accept()` in the right order. `SyntaxModel::parse(revision,
language, text, edits)` does the three steps inline and returns why it was
refused (if it was) and whether it fell back to plain text. The existing
three-call API stays, unchanged, for the deferred/off-thread seam.

## Design

`SyntaxModel` today exposes a three-call temporal protocol:
- `request(...)` first rejects a non-advancing revision (returns
  `StaleRevision`), and only then cancels any pending request, validates
  size/edits, and mints a self-contained `SyntaxParseRequest` (carries a cancel
  flag). The staleness guard precedes the cancel, so a stale call leaves an
  in-flight request untouched.
- `run(request) const` executes the parse. It is `const` and takes only the
  request, so it can run on a worker thread while the model is mutated on the
  main thread. This is the seam a future async/LSP source needs
  (doc/spec-syntax-and-diffs.md, "a future async, app-owned LSP transport").
- `accept(request, output)` validates identity/revision/cancellation and swaps
  in the new view-state.

Every shipped driver runs the three inline on one thread
(`EditorRuntime::Impl::refreshSyntax`, `EditorRuntime.cpp`), as do most tests.
For that case the ordering is pure ceremony a caller can get wrong (accept the
wrong request, skip the `accepted()` check, forget `cancelPending`).

Add a synchronous convenience that OWNS the sequence:

    SyntaxParseResult SyntaxModel::parse(
        Revision revision, LanguageId language, std::string text,
        std::vector<SyntaxEdit> edits = {});

It calls `request`; if refused, returns the refusal; otherwise `run` then
`accept` inline and maps the outcome. On one thread with a matching revision and
identity, `accept` cannot report StaleRevision or UnknownRequest. It CAN still
report `Cancelled` — a parser may call `request.cancel()` from inside `run()`
(cancellation is public on `SyntaxParseRequest` and needs no cross-thread
interleaving) — or `MalformedOutput` if the parser returns a mismatched-revision
output. The result therefore carries `acceptError` rather than hiding these.

Result type (new, small):

    struct SyntaxParseResult {
        SyntaxRequestError requestError = SyntaxRequestError::None;
        SyntaxAcceptError  acceptError  = SyntaxAcceptError::None;
        bool usedFallback = false;  // parsed to plain text (no grammar / parse failed)
        [[nodiscard]] bool accepted() const noexcept {
            return requestError == SyntaxRequestError::None &&
                   acceptError == SyntaxAcceptError::None;
        }
    };

Mechanism chosen: a convenience method delegating to the existing three, rather
than reimplementing them, so there is exactly one copy of the validation and
swap logic. Rejected alternative: REPLACING the three-call API with a single
`parse()` — that would delete the const-`run()` off-thread seam the syntax spec
reserves, trading a real capability for a smaller surface.

## Invariants

- The off-thread seam survives: `run(const SyntaxParseRequest&) const` and the
  public `request`/`accept`/`cancelPending` remain, with identical semantics.
  `parse()` is additive.
- `parse()` introduces no second copy of the request/accept validation or the
  view-state swap — it delegates.
- Behavior-preserving for existing callers: the three-call sites that migrate to
  `parse()` produce byte-identical view-states and the same accept outcomes.

## Considerations

- `parse()` moves `text`/`edits` into `request`; on refusal the request never
  formed, so nothing partially applied — matches the current contract.
- `parse()` needs no `cancelPending()` of its own: when its `request()` call
  passes the staleness guard it cancels any prior pending request itself; when it
  fails the guard there is deliberately nothing to cancel.
- A parser that self-cancels (calls `request.cancel()` during `run()`) yields
  `acceptError == Cancelled` and leaves the view-state unchanged — the same
  outcome the three-call path produces.
- Callers that specifically exercise the seam (staleness across two live
  requests, mid-flight cancellation, malformed-output rejection) STAY on the
  explicit three-call API; `parse()` is for the inline "just parse this" case.

## Risks and Mitigations

- Risk: migrating a caller changes an observable outcome. Mitigation: the
  existing syntax suite (test_syntax, test_treesitter_syntax, render/e2e
  goldens) is the oracle; it must stay green with zero golden regeneration.
- Risk: `parse()` tempts a future async caller to use it off-thread. Mitigation:
  its doc-comment states it is synchronous and drives the model on the calling
  thread; the off-thread path is the explicit three-call API.

## Acceptance (Definition of Done)

- Observable: `EditorRuntime::Impl::refreshSyntax` reads as one `model.parse(...)`
  call instead of the request/run/accept block.
- Budgets: n/a (no perf-sensitive change; same work, same thread).
- Gates: `bash scripts/check.sh` green (101 tests, 0 warnings).
- Oracles:
  - parse-equals-manual: a characterization test asserting `parse(...)` leaves
    the model in the same `viewState()` and returns the same fallback/refusal
    outcome as the hand-driven `request/run/accept` for: a grammar hit, a
    no-grammar fallback, a stale revision (refused), and an oversized document
    (refused). Not independent (it delegates to the same three calls) but pins
    behavior-preservation; written before the method and failing until it exists.
  - self-cancelling-parser: with a parser double that calls `request.cancel()`
    during `parse`, assert the result reports `acceptError == Cancelled`,
    `accepted() == false`, and the model's `viewState()` is unchanged.
  - seam-intact: the existing cancellation/staleness tests keep using and passing
    against the three-call API unchanged.

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Add `SyntaxParseResult` + `SyntaxModel::parse()` declaration | `include/ssg/SyntaxModel.h` | (compile) | seam-additive |
| 2 | Write parse-equals-manual + self-cancelling-parser oracle tests (fail: no `parse` yet) | `tests/test_syntax.cpp` | ref: parse vs hand-driven; self-cancel -> Cancelled + unchanged view | behavior-preserving |
| 3 | Implement `parse()` delegating to request/run/accept | `src/SyntaxModel.cpp` | step-2 test green | no duplicated validation |
| 4 | Migrate the inline driver to `parse()` | `src/EditorRuntime.cpp` | full syntax suite + goldens green, no regeneration | behavior-preserving |
| 5 | Migrate inline-only tests; leave seam tests on 3-call API | `tests/test_syntax.cpp`, `tests/test_treesitter_syntax.cpp` | seam-intact | off-thread seam survives |

## Rationale (optional)

The three-call protocol is not an accident to be tidied away; it is the shape
that lets the expensive parse move off the main thread later. The smell the
least-surprise review flagged is that the COMMON case pays for the RARE case's
flexibility at every call site. A convenience that owns the sequence fixes the
ergonomics for the common case and leaves the capability intact — the
pit-of-success move, not the surface-reduction move.
