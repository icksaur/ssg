# plan-3-separate-semantic-state-from-grid-presentation

## Goals

Make the general session and wire contracts semantic-only. Keep reusable
terminal presentation in an optional grid adapter without exposing cells,
rectangles, viewport dimensions, or shell geometry to native-layout clients.

## Design

Split `SessionSnapshot` into an authoritative semantic frame and a separately
owned grid projection. Each grid presenter is per-view and single-owner; it
consumes immutable revision-keyed semantic snapshots and never observes a
mid-frame mutation. HTTP publishes only semantic frames. The TUI composes
`EditorSession` with the grid presenter in-process. This plan retains a
deprecated presentation bridge until Plan 4 supplies the UI-tree solver and
Plan 6 activates `kSemanticUiWireVersion`; it does not create a permanent
second path.

Layout-dependent commands remain entries in the one authoritative command
catalog. Their handlers return a closed `ViewActionRequest` instead of mutating
semantic state or hidden session geometry. `CommandResult` and
`ClientInputResult` distinguish a completed semantic dispatch from a view-owned
action; no second command-name mapping is introduced. A host resolves the
action with native layout or `GridPresenter::apply`. Presenter application
validates the full `GridBasis`, mutates only presenter state, and may return a
typed semantic `ClientInput` for authoritative submission. The presenter never
stores or calls an `EditorSession`.

`ViewActionRequest` is the closed variant of document scroll by lines, pages,
or fraction; tree scroll by lines or fraction; visual selection movement with
move/extend and line/page direction; reveal or center selection; pane
split/close/cycle/directional focus; and pointer-edge continuation before or
after the document viewport. These are the complete layout-dependent command
families migrated by this plan. Palette query, selection, and windowing remain
client-local inputs to `GridPresentationRequest`, not view actions.

`CommandResult` carries a closed outcome of `Completed`,
`ViewActionRequired`, or `Rejected`; only `ViewActionRequired` carries a
`ViewActionRequest`. `accepted()` is true for both non-rejected outcomes, while
`completed()` is true only after semantic dispatch. `ClientInputResult` mirrors
the view-owned distinction. Callers must branch on the outcome rather than
treating `accepted()` as proof that an action finished.

`GridPresenter::apply` takes only a `ViewActionRequest` and the `GridFrame`
from which the action arose. Its closed result distinguishes applied,
transition-required, and rejected outcomes and carries at most one optional
`ClientInput`; it never invokes the session. The host submits that input once
through `EditorSession::input` before considering the action complete. A
rejected submission is dropped without retrying presenter application; another
apply requires a fresh host-driven action. After the deprecated projection
bridge is removed, `GridPresenter::project` takes a moved semantic
`SessionSnapshot`, not an `EditorSession`.

Programmatic dispatch returns `ViewActionRequired` to its caller. The HTTP
input result codec carries the same typed request to the native-layout client.
`ScriptHost` accepts an optional host-supplied view-action sink: the TUI
supplies one, while a host without a layout owner reports the named
`view_action_unavailable` script error and does not claim command completion.
Client input never establishes whether a layout owner exists.
The sink is the typed callback
`GridActionResult(ViewActionRequest const&)`; the TUI implementation
captures its currently adopted `GridBasis` and delegates directly to
`GridPresenter::apply`. It does not define a script-specific action shape or
resolution path.

The deprecated `EditorSession::present()` bridge does not resolve or remember
view actions once their state moves to a presenter. It projects moved fields
from fixed defaults. A wrapper-only caller that ignores `ViewActionRequired`
therefore observes no scroll; callers requiring persistent view behavior must
own a presenter. This intentional compatibility narrowing prevents hidden
auto-application or a second presentation mutation path. The semantic HTTP
path already uses `snapshot()` and is unaffected.

Selection and focus changes resolved from layout re-enter through named
`ClientInput` variants. The session validates their semantic revision, active
document, position bounds, and attached client before mutation. Pointer edge
continuation is resolved to a concrete position at the presentation edge and
then uses the existing document gesture transition; cells and rectangles never
enter semantic input.

## Invariants

- FRAME-1: The semantic frame is complete without client geometry. State at the
  semantic snapshot type.
- FRAME-2: Grid state cannot enter the general protocol contract. State at the
  protocol snapshot codec.
- FRAME-3: Each client owns native layout and device geometry; optional library
  presentation consumes semantic state but cannot mutate product state. State
  at the grid presenter API.
- FRAME-4: Per-view presentation caches are isolated by view identity and
  invalidated by their semantic basis. State at grid presentation state.
- FRAME-5: A command has one catalog entry and one resolution path whether its
  result is semantic, client-owned, or view-owned. State at command result and
  input result types.
- FRAME-6: Presenter-resolved product changes re-enter through validated typed
  `ClientInput`; a presenter has no session mutation capability. State at the
  presenter and semantic input APIs.

## Considerations

Move shell layout, viewport, style, prompt grid projection, selection reveal,
and tree windows together. Preserve terminal behavior and hit testing. Do not
create a second semantic snapshot assembled independently from the existing
authoritative sections.

`accepted()` alone is insufficient once command resolution can yield an action.
The result type must make completed dispatch versus view-owned resolution
explicit, and every host boundary must handle both. Pure view actions do not
advance the semantic revision or set semantic delta fields. Same-revision
reprojection remains valid after resize or presenter-state changes, while
applying an action captured from an older presentation generation is rejected.
Resolved semantic input uses exact revision matching against the session's
current active document; a session advance after presenter resolution rejects
the input rather than rebasing or retrying it.

Ordinary semantic changes such as edits, find navigation, document switches,
and tree selection are revealed by presenter reconciliation against the new
semantic frame. Explicit scroll, page/visual-row movement, center, pane, and
pointer-edge operations remain view actions because their intent cannot be
inferred from semantic change alone.

## Risks and Mitigations

- Splitting ownership may duplicate snapshots: pass immutable semantic values
  or revision-keyed views into the presenter.
- Presentation currently causes some view-state updates: identify and replace
  hidden mutation with explicit typed presentation state.
- Programmatic and scripted command callers could ignore a view-owned result:
  use an explicit result outcome, migrate every host boundary, and require the
  optional host-owned script sink or surface `view_action_unavailable`.
- Presenter re-entry could loop or duplicate a transition: one apply produces
  at most one input, each action permits one submission, and rejection is
  terminal for that action.
- A stale hit could mutate a newly resized presenter: require the complete
  `GridBasis` for presenter application and leave generation unchanged on
  rejection.
- Wire removal follows the compatibility policy in Plan 1. This plan marks the
  bridge deprecated; Plan 6 removes it after all in-tree consumers migrate.
  The bridge deletes when Plan 6 advances the semantic UI wire version and
  every in-tree grid consumer uses `GridPresenter`.

## Acceptance (Definition of Done)

- Observable: terminal output/hits remain equivalent; web consumes only the
  semantic side of the split while the deprecated wire bridge remains until
  Plan 6.
- Budgets: semantic snapshot cost does not acquire grid work; grid projection
  remains viewport-bounded where currently guaranteed.
- Gates: `scripts/check.sh` and `scripts/check.sh push`.
- Oracles: semantic snapshot equality with and without a grid presenter;
  terminal cell/hit parity; protocol fixtures reject removed geometry;
  per-view isolation; stale view-action rejection; command-resolution parity
  proving equal `ViewActionRequest` payloads and outcomes for equal key,
  pointer, and programmatic intents; resolved selection boundary rejection
  against the session's current document and revision; one-apply/one-submit
  termination.

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Introduce the semantic-frame and per-view grid-projection ownership seam | `include/ssg/session_snapshot.h`, `include/ssg/EditorSession.h`, new/existing grid presenter headers | invariant: dimensionless frame is complete | FRAME-1, FRAME-3 |
| 2 | Add closed view-action command results and validated semantic re-entry | `include/ssg/ClientInput.h`, `include/ssg/EditorClient.h`, `include/ssg/CommandInvocation.h`, command execution and protocol codecs | command-resolution parity; malformed/stale resolved-input rejection | FRAME-5, FRAME-6 |
| 3 | Move document/tree scroll, reveal, and center state/actions into presenters; make the deprecated wrapper project those fields from defaults | `include/ssg/GridPresenter.h`, `include/ssg/EditorSession.h`, `src/GridPresenter.cpp`, `src/runtime/presentation.cpp`, `src/runtime/navigation.cpp`, `src/runtime/snapshot.cpp`, `src/EditorSession.cpp`, TUI/web action handlers and focused scroll tests | presenter-not-session scroll ownership with unchanged semantic revision; stale-basis rejection; equal action payload/outcome across ingress; wrapper `present()` projects moved fields at fixed defaults after ignored view actions | FRAME-3, FRAME-4, FRAME-5 |
| 4 | Move pane state, visual selection movement, remaining grid caches, and projection out of `EditorSession::Impl` as one atomic ownership cut | `src/runtime/editor_session_internal.h`, `src/runtime/editing.cpp`, `src/runtime/snapshot.cpp`, grid presentation sources, follow-edits model | concurrent session changes appear only at snapshot revision boundaries; per-view pane/selection isolation | FRAME-3, FRAME-4, FRAME-6 |
| 5 | Move pointer-edge resolution onto presenter actions using the extracted projection state | `src/EditorSession.cpp`, grid presentation sources, document gesture tests | edge continuation parity and stale-basis rejection | FRAME-3, FRAME-6 |
| 6 | Complete TUI action application and presentation configuration through the adapter | `apps/ssg_main.cpp`, script host, terminal/render integration files | terminal cells, hits, scripted actions, and startup parity | FRAME-3, FRAME-5, FRAME-6 |
| 7 | Isolate the old presentation projection as a deprecated compatibility envelope outside semantic state | snapshot/protocol manifest, runtime and tests | semantic clients never read bridge fields; frozen bridge fixture round-trip | FRAME-1, FRAME-2 |
| 8 | Hand the bridge removal inventory to Plans 4 and 6 | `plan-4.md` surface inventory and Plan 1 wire manifest | every bridge consumer has an assigned migration/removal step | FRAME-2 |

## Rationale

Grid rendering is useful reusable presentation, but making it part of every
client's state contract reverses the intended dependency direction. The grid
projection introduced here is the same solved grid-tree value implemented by
Plan 4, not a legacy intermediate type.
