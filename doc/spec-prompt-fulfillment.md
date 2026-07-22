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
  (b) deriving the next find/replace query from the authoritative published field
  plus a keystroke, holding no client-side authoritative copy (spec-m7's existing
  write-only capture model — NOT a browser "optimistic echo").
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

Generic navigation commands (DECIDED, review MUST + SHOULD): introduce kind-neutral
`prompt.next` / `prompt.previous` as the generic "advance/retreat within the active
prompt". Do NOT overload the palette-specific `palette.next`/`palette.previous` for
find/replace — a single id must not mean "move my client-owned palette selection"
in one context and "find.next" in another (that ambiguity is what boundary audits
must not have to disentangle). The keymap binds ArrowDown/ArrowUp to
`prompt.next`/`prompt.previous` in `prompt` focus. The client, when the active
prompt is the PALETTE (a client-owned derived view), resolves these locally
(moves its selection); for any other prompt kind the client emits them and the
LIBRARY fulfils by active kind (find/replace -> find.next/find.previous). Palette
also keeps its existing `palette.next`/`palette.previous` selection-movement path,
or those are subsumed by the palette's client-side handling of `prompt.next`/
`prompt.previous` — implementer's choice, but the SHIPPED result is: one generic
`prompt.next`/`prompt.previous` pair, library-fulfilled for find/replace,
client-derived-view-resolved for palette.

Any new/renamed command id is a catalog cascade (per spec-m7 precedent):
`data/required-commands.json` (+ owner count), `tests/test_required_commands.cpp`,
`tests/runtime/command_cases.h`, protocol codec + round-trip, and the Lua-parity
flags. Argument-free navigation/submit/cancel commands are `keymap:true`;
`lua:true` for user-visible ones (I20).

### Deriving prompt input from the authoritative field (the SHOULD — keep as-is)

The client edits find/replace text WITHOUT holding an authoritative copy: on a
keystroke it computes the next query as *the published
`FindReplaceViewState.query` + the typed code point* (or minus the last, on
Backspace) and dispatches the full string via `find.update_query` /
`replace.update_replacement`. The server query is the sole authority and is
re-read each snapshot; the client persists no product state. `spec-m7.md` already
documents exactly this ("Query editing without a client copy… write-only input
capture derived from the latest snapshot, not a display echo") — it is the
CORRECT boundary-clean model, not the browser "optimistic echo" pattern (which m7
explicitly defers). This STAYS unchanged. The fix is only to NAME this pattern in
I17 so it is unambiguously permitted; do NOT reframe it as a client-held copy.

### spec.md changes

- Extend I17's derived-view exception list from two examples to an EXHAUSTIVE,
  named set (extended only by a future spec amendment, never by client
  interpretation): (1) key-sequence/leader resolution from the published keymap;
  (2) fuzzy filter/rank of a published candidate list AND the resulting per-client
  selection; (3) deriving the next value of an input-carrying command from the
  authoritative published field plus a local keystroke, holding no authoritative
  copy (the find/replace query-editing model in spec-m7). State EXPLICITLY that
  resolving a generic command to a feature-specific command by prompt/context kind
  is NOT a permitted derived view (that is product semantics, library-owned by
  I25) UNLESS it is the resolution of a client-owned derived-view SELECTION (the
  palette). Phrase the set as "exhaustive as of this spec; new derived views are
  added only by amending this invariant" so a future genuinely-needed view (IME,
  latency caret echo) is added deliberately, not assumed.
- Add one crisp cross-referencing sentence so the boundary reads as one model:
  roles paragraph + I17 (the exhaustive derived-view set) + I25 (feature vs
  mechanism) are the three facets of the same rule.

### spec-m7.md changes

- DELETE only the "Client fulfilment keyed on the active prompt kind" bullet
  (`spec-m7.md` ~line 192-196) that mandates client-side `prompt.submit ->
  find.next`, `ArrowDown -> find.next`, etc. Replace with: find/replace prompt
  fulfilment (submit/cancel/next/previous) is LIBRARY-owned by active prompt kind;
  the client emits generic `prompt.submit`/`prompt.cancel`/`prompt.next`/
  `prompt.previous`. KEEP UNCHANGED the "Query editing without a client copy"
  bullet and the "authoritative server state / no client-side query echo" bullet
  (~line 156-190) — those already document the correct boundary-clean model and
  must NOT be reframed. KEEP the render (match highlighting) bullet.

## Invariants

- I17 (amended): the derived-view exception set is NAMED and closed to client
  interpretation — leader resolution; fuzzy filter/rank of a published list + its
  selection; deriving an input command's next value from the authoritative
  published field + a keystroke (no client-held authoritative copy). "Exhaustive
  as of this spec; extended only by amending this invariant." Anything else a
  client does that decides product semantics violates I17/I25.
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
  - boundary: PRIMARY oracle is behavioral — a test dispatches ONLY the generic
    `prompt.submit`/`prompt.cancel`/`prompt.next`/`prompt.previous` (the exact set
    the client now emits for find/replace) and asserts the library performs the
    feature action; a client that sends no `find.next`/`replace.current` still gets
    correct find/replace behavior. A grep guard that `find.next`/`replace.current`
    string literals are gone from the client prompt path is a SECONDARY check only
    (weak alone — easily evaded by indirection).
  - parity: in-process and (modeled) websocket clients get identical results
    because fulfilment is server-side.

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Amend spec.md I17 (name the exhaustive derived-view set incl. deriving input from the authoritative field; exclude context-kind command remap unless derived-view selection) + one boundary cross-ref sentence | `doc/spec.md` | review | I17, I25 |
| 2 | Relocate feature semantics in spec-m7.md: delete "Client fulfilment" bullet; state find/replace prompt fulfilment is library-owned by kind; keep render + query-editing (no-client-copy) bullets unchanged | `doc/spec-m7.md` | review | I17, I25 |
| 3 | Library: add kind-neutral `prompt.next`/`prompt.previous` commands; fulfil find/replace prompt submit/cancel/next/previous by active `PromptKind` in the prompt command path (submit->find.next/replace.current, next->find.next, previous->find.previous, cancel->find.close with controller reset). Catalog cascade for the new ids | `src/runtime/presentation.cpp`, `include/ssg/PromptSurface.h` if needed, `data/required-commands.json`, `src/Protocol.cpp`, `tests/test_required_commands.cpp`, `tests/runtime/command_cases.h` | behavioral library-fulfilment oracle (generic commands only) | I25 |
| 4 | Client: delete the find/replace branches of `dispatchResolved` (send generic prompt commands); KEEP the palette branch (derived-view) and the query-derivation from the authoritative field | `apps/ssg_main.cpp:388-405` | boundary grep oracle; palette-unchanged oracle | I17, I25 |
| 5 | Re-audit: confirm `apps/` holds only I17 derived views + input/render; both gates green | `apps/`, tests | full boundary re-audit clean | I17, I25 |

## Rationale (skippable)

The audit found one true violation (find/replace prompt fulfilment decided
client-side) that spec-m7 actively mandated — the specs contradicted the
invariants. The palette looks similar but is legitimately different: its selection
is a per-client derived view the server intentionally does not centralize, so its
fulfilment must stay client-side. The find/replace query editing is already
boundary-clean in spec-m7 (the client holds no authoritative copy; it derives the
next query from the published field each snapshot), so it stays. So the correct
move is narrow: relocate find/replace fulfilment to the library, and make I17's
derived-view exception exhaustive and explicit so the boundary stops being
reinterpreted.
