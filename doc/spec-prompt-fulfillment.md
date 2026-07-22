# spec-prompt-fulfillment

Status: DRAFT — sharpen the library/client boundary; move find/replace prompt
fulfilment into the library; bless the two legitimate client derived views.

## Goals

The library owns what editor commands MEAN; the client only translates platform
input and renders. After this change:
- Submitting / cancelling / navigating a find or replace prompt is decided by the
  LIBRARY (from the active prompt kind), not mapped by the client.
- The two things the client legitimately owns for responsiveness stay client-side
  and are named explicitly in the invariant so they are no longer ambiguous:
  (a) the palette's per-client fuzzy ranking + selection (already blessed), and
  (b) optimistic echo of a server-authoritative prompt text field per keystroke.
- `doc/spec.md` states the boundary crisply in one place; `doc/spec-m7.md` no
  longer documents client-side feature fulfilment as intended design.

Non-goal: changing what find/replace/palette DO, or the palette's client-owned
selection model. This is a boundary relocation, behavior-preserving.

## Design

### The rule (what belongs where)

A client may fulfil a generic prompt command locally ONLY as resolution of a
CLIENT-OWNED DERIVED VIEW (per spec.md I17). The palette qualifies: each client
computes its own fuzzy ranking and selection, so only that client knows which
candidate `Enter` targets — the client resolves `prompt.submit` to
`palette.execute{selectedId}` and `prompt.next`/`prompt.previous` to moving its
own selection. This stays.

Every other prompt kind has NO client-owned state. Find and replace prompts read
their query/replacement from a server-authoritative controller; there is no
client selection. Therefore the LIBRARY fulfils their generic prompt commands by
active kind:
- find prompt: `prompt.submit` -> `find.next`; `prompt.next` -> `find.next`;
  `prompt.previous` -> `find.previous`; `prompt.cancel` -> `find.close`.
- replace prompt: `prompt.submit` -> `replace.current`; `prompt.next` ->
  `find.next`; `prompt.previous` -> `find.previous`; `prompt.cancel` ->
  `find.close`.
This mapping is a product decision (every client would reduplicate it identically)
and so is library-owned by I25.

### Mechanism

The library already routes `prompt.submit`/`prompt.cancel` through
`promptStatusCommand` (`src/runtime/presentation.cpp:85`) and
`PromptSurface::submit()` returns a `PromptSubmission{kind, values, toggles}`
carrying the active `PromptKind`. Extend the library prompt fulfilment so that,
for `PromptKind::Find`/`Replace`, submit/cancel/next/previous perform the feature
action above; for `PromptKind::Palette` the library does NOT fulfil (the client
already sends `palette.execute` / moves selection); for other kinds
(Path/Settings/CommandArgument) submit/cancel keep today's generic behavior.

Generic navigation commands: the client currently emits `palette.next`/
`palette.previous` for ArrowDown/Up. Introduce kind-neutral `prompt.next`/
`prompt.previous` (the generic "advance/retreat within the active prompt")
OR have the library interpret the existing next/previous commands by active prompt
kind — mechanism to finalize in review; the invariant is that the LIBRARY decides
what next/previous mean for find/replace, and the client keeps deciding for the
palette (its derived view). Whichever naming is chosen, palette selection movement
stays a client derived-view operation and find/replace navigation becomes a
library-fulfilled command.

Any new/renamed command id is a catalog cascade (per spec-m7 precedent):
`data/required-commands.json` (+ owner count), `tests/test_required_commands.cpp`,
`tests/runtime/command_cases.h`, protocol codec + round-trip, and the Lua-parity
flags. Argument-free navigation/submit/cancel commands are `keymap:true`;
`lua:true` for user-visible ones (I20).

### Optimistic text echo (the SHOULD — keep, do not move)

The client edits find/replace query text locally (append/backspace) and dispatches
the full next string via `find.update_query` / `replace.update_replacement`; the
server query is authoritative and re-read on the next snapshot. This is the same
optimistic-echo-of-an-authoritative-field pattern the palette already uses for
responsiveness and is a legitimate client derived view. It STAYS client-side. The
fix is to NAME it in I17 so it is unambiguous, not to move it.

### spec.md changes

- Extend I17's derived-view exception list from two examples (leader resolution;
  fuzzy-filter a published list) to also include "optimistic echo of a
  server-authoritative input field (the client renders and edits a local copy for
  per-keystroke responsiveness; the server field remains authoritative and is
  re-read each snapshot)" — and state EXPLICITLY that resolving a generic command
  to a feature-specific command by prompt/context kind is NOT a permitted derived
  view (that is product semantics, library-owned by I25) UNLESS it is the
  resolution of a client-owned derived-view selection (the palette).
- Add one crisp cross-referencing sentence so the boundary reads as one model:
  roles paragraph + I17 (derived-view exceptions) + I25 (feature vs mechanism)
  are the three facets; I17 now enumerates the exhaustive derived-view set.

### spec-m7.md changes

- Delete the "Client fulfilment keyed on the active prompt kind" bullet
  (`spec-m7.md` ~line 192-196) that mandates client-side `prompt.submit ->
  find.next`, `ArrowDown -> find.next`, etc. Replace with: find/replace prompt
  fulfilment (submit/cancel/next/previous) is LIBRARY-owned by active prompt kind;
  the client emits generic prompt commands. Keep the render bullet (match
  highlighting) and the query-echo bullet (client edits a local copy of the
  authoritative query) unchanged — those are legitimate.

## Invariants

- I17 (amended): the derived-view exception set is EXHAUSTIVE and named — leader
  resolution, fuzzy filter/rank of a published list + its selection, and optimistic
  echo of an authoritative input field. Anything else a client does that decides
  product semantics violates I17/I25.
- I25: feature fulfilment (what submit/cancel/navigate mean for find/replace) is
  library-owned.
- Behavior preservation: the observable find/replace/palette behavior is identical
  before and after; only the owner of the decision moves.

## Considerations

- The palette branch must remain: its selection is a genuine client derived view
  (per-client ranking), so `Enter` -> execute-selected and Arrow -> move-selection
  cannot move server-side without inventing an authoritative palette selection the
  spec deliberately does not have (see spec-palette.md "Cross-client consistency
  is intentionally weak").
- `prompt.cancel` for find/replace currently maps to `find.close`. Moving it means
  the library's generic `prompt.cancel` must, for a find/replace prompt, run the
  feature close (which resets controller generation/options), not just clear the
  prompt surface. Verify `find.close` vs `prompt.cancel` semantics don't diverge.
- Command-id naming (`prompt.next`/`prompt.previous` vs reusing `palette.next`):
  decide in review; either way keep the catalog cascade consistent and the wire
  additive.
- No behavior change to Path/Settings/CommandArgument prompts.

## Acceptance (Definition of Done)

- Observable: find, replace, and palette behave identically to today (Enter/arrows/
  escape do the same things); confirmed by existing + new tests. No visible change.
- Gates: `bash scripts/check.sh` green (0 warnings, all tests) with and without
  `SSG_TREESITTER`; `data/required-commands.json` cascade tests pass.
- Oracles:
  - library fulfilment: a runtime test opens a find prompt and dispatches the
    generic `prompt.submit` (no client mapping) and asserts the library advances
    to the next match; same for replace -> replace.current; and
    `prompt.cancel` -> find closed with controller reset. Fails before (library
    prompt.submit did not perform find.next).
  - palette unchanged: submitting the palette still executes the client-selected
    candidate via `palette.execute{id}` (client derived view intact).
  - boundary: a grep/test guard that `apps/` contains no find/replace feature-
    command mapping (`find.next`/`replace.current` string literals gone from the
    client's prompt routing).
  - parity: in-process and (modeled) websocket clients get identical results
    because fulfilment is server-side.

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Amend spec.md I17 (name the exhaustive derived-view set incl. optimistic echo; exclude context-kind command remap unless derived-view selection) + one boundary cross-ref sentence | `doc/spec.md` | review | I17, I25 |
| 2 | Relocate feature semantics in spec-m7.md: delete "Client fulfilment" bullet; state find/replace prompt fulfilment is library-owned by kind; keep render + query-echo bullets | `doc/spec-m7.md` | review | I17, I25 |
| 3 | Library: fulfil find/replace prompt submit/cancel/next/previous by active `PromptKind` in the prompt command path; add/finalize generic `prompt.next`/`prompt.previous` (catalog cascade) | `src/runtime/presentation.cpp`, `include/ssg/PromptSurface.h` if needed, `data/required-commands.json`, `src/Protocol.cpp`, command cascade tests | library-fulfilment oracle | I25 |
| 4 | Client: delete the find/replace branches of `dispatchResolved` (send generic prompt commands); KEEP the palette branch (derived-view) and the optimistic query echo | `apps/ssg_main.cpp:388-405` | boundary grep oracle; palette-unchanged oracle | I17, I25 |
| 5 | Re-audit: confirm `apps/` holds only I17 derived views + input/render; both gates green | `apps/`, tests | full boundary re-audit clean | I17, I25 |

## Rationale (skippable)

The audit found one true violation (find/replace prompt fulfilment decided
client-side) that spec-m7 actively mandated — the specs contradicted the
invariants. The palette looks similar but is legitimately different: its selection
is a per-client derived view the server intentionally does not centralize, so its
fulfilment must stay client-side. The query echo is the same optimistic-echo
pattern the palette already uses. So the correct move is narrow: relocate
find/replace fulfilment to the library, and make I17's derived-view exception
exhaustive and explicit so the boundary stops being reinterpreted.
