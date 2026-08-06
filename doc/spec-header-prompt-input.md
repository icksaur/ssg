# spec-header-prompt-input

Status: draft

## Goals

Typing into the command palette (`Alt+Shift+P`) and the file finder (`Alt+P`)
filters their candidates again. Today the query is silently dropped: the pickers
open and take focus, but no typed character reaches their query.

After this change:

- A printable key typed while a header-hosted picker (palette / file finder) is
  open appends to that picker's query and re-filters, exactly as find/replace and
  the footer prompts already do.
- The fix generalizes: any prompt the server marks active routes its text
  regardless of WHERE that prompt draws its input (header input line vs a footer
  reservation), so a future header/footer hosting change cannot re-break input.

## Design

### Root cause

The app (`apps/ssg_main.cpp`, the input-refresh block ~1140-1168) decides which
prompt is open — `pickerOpen` / `findOpen` / `replaceOpen` / `textPromptOpen` —
from `snapshot->sections().promptStatus.prompt`, the prompt's **footer layout
view**, reading its `->kind`. But a `PromptKind::Palette` prompt (both pickers)
renders its query in the HEADER input line, so `promptRowCount(Palette) == 0` and
`computePromptLayout` cannot place the prompt's one `query` input row into a
zero-row reservation — it returns no view, leaving `promptStatus.prompt ==
nullopt`. So `pickerOpen` is false and `routeText`'s `PromptQuery` branch (which
requires one of the open-flags) drops the character. `shell.focus` is correctly
`Prompt` and `SemanticInputRouter::textRouting` correctly returns `PromptQuery`;
only the app's picker-identity inference is wrong.

The design flaw: the app infers an AUTHORITATIVE state (which prompt is active)
from a RENDERING ARTIFACT (its footer layout view). A prompt that draws elsewhere
has no such view, so the inference silently fails.

### The fix: publish the active prompt kind authoritatively

Add the active prompt's KIND to the published snapshot, independent of whether a
footer layout view exists, and derive the app's open-flags from it.

- `PromptStatusViewState` (include/ssg/StatusQueue.h) gains
  `std::optional<PromptKind> activeKind`. `promptStatusView`
  (src/runtime/snapshot.cpp) sets it from `prompt.request() ? prompt.request()
  ->kind : std::nullopt` — the prompt surface's own authoritative request, set
  the moment a prompt opens and cleared when it closes, with no dependence on
  `computePromptLayout` succeeding. `promptStatus.prompt` (the layout view) stays
  exactly as-is: it remains the source for footer prompts' rendered controls and
  values; `activeKind` is the orthogonal "which prompt is active" signal.
- The Protocol codec (src/Protocol.cpp) round-trips `activeKind` in the
  prompt-status section, beside the existing `prompt` field.
- `apps/ssg_main.cpp` derives every open-flag from `promptStatus.activeKind`
  instead of `promptStatus.prompt->kind`:
  - `pickerOpen = activeKind == Palette`
  - `findPromptActive = activeKind == Find`; `replacePromptActive = activeKind
    == Replace` (each still ANDed with `findView.open`, unchanged)
  - `textPromptOpen = activeKind ∈ {Path, CommandArgument, Settings}`
  The VALUE reads are unchanged: footer prompts still read
  `promptStatus.prompt->controls` for their rendered value (that view is present
  for them), and the palette's query stays the client-owned `picker.query`.

Because `activeKind` comes straight from `prompt.request()`, it is present for a
header-hosted prompt exactly as for a footer-hosted one, so the palette and file
finder are detected and their text routes.

### Why not "make the palette produce a layout view"

Forcing `computePromptLayout` to emit a degenerate zero-row view for the palette
would re-introduce the coupling this bug came from (open-detection riding on the
layout). Publishing the kind directly severs it: the authoritative signal is the
prompt request, which is what "is a prompt active, and which" actually means.

## Invariants

- **Prompt-open detection does not depend on the prompt's layout view.** The
  app's open-flags derive from the published active prompt KIND, so a prompt that
  renders in the header (no footer view) is detected identically to one that
  reserves footer rows. This is the invariant whose violation caused the bug.
- **One authoritative source for "which prompt is active".** `activeKind` mirrors
  `PromptSurface::request()`; the app never re-derives prompt identity from a
  rendering artifact. `promptStatus.prompt` remains solely a layout/value view.
- **No behavior change for footer prompts.** find/replace/path/settings already
  populate `promptStatus.prompt`; their open-flags and value reads are unchanged
  (they now key off `activeKind`, which equals the old `prompt->kind` for them).
- **Server owns state; client renders.** The kind is server-published; the client
  routes text by it. No new client-side prompt state.

## Considerations

- **Codec compatibility.** `activeKind` is an added optional field in the
  prompt-status section; the codec round-trip tests (test_protocol.cpp) cover new
  fields. A snapshot with no active prompt carries `nullopt`.
- **The two `kind == Palette` sites.** `apps/ssg_main.cpp` also sets `pickerOpen`
  optimistically in `dispatchResolved` (on the opener command) — that stays as a
  same-frame nicety; the authoritative refresh now agrees with it instead of
  overwriting it to false. `promptFocusRegion(kind)` (the server-side header vs
  footer anchoring) is unrelated and unchanged.
- **Missing regression coverage.** No test drove "type into an open palette →
  candidates filter" at the seam that broke (the app's kind derivation). The
  oracle below adds a runtime-level check that the active prompt kind is published
  for a header-hosted palette, which is exactly the signal the app consumes.

## Risks and Mitigations

- **Codec drift** (field added to the section but not round-tripped): the
  existing prompt-status round-trip test asserts equality across encode/decode,
  which fails if `activeKind` is not carried.
- **Silent re-regression** if someone reverts the app to read `prompt->kind`:
  mitigated by an oracle asserting `promptStatus.activeKind == Palette` while
  `promptStatus.prompt == nullopt` for an open palette — the exact divergence the
  app must tolerate.

## Acceptance (Definition of Done)

- Observable: open the palette or file finder and type — candidates filter.
  Because it is a live keyboard gesture, the end-to-end result needs VISUAL
  SIGNOFF before merge (the automated oracles below cover the seam).
- Budgets: n/a.
- Gates: `bash scripts/check.sh` green.
- Oracles:
  - Runtime (`tests/runtime/test_runtime_presentation.cpp` or test_runtime_
    editing) — after `palette.open`, `snapshot->sections().promptStatus.activeKind
    == PromptKind::Palette` AND `promptStatus.prompt == nullopt` (the header-hosted
    prompt publishes its kind but no footer view); after `file_finder.open`,
    `activeKind == Palette`; after a footer prompt (e.g. a `Path` save-as/goto),
    `activeKind` is that kind AND `promptStatus.prompt` is present; with no prompt,
    `activeKind == nullopt`.
  - Codec (`tests/test_protocol.cpp`) — a `PromptStatusViewState` with a set
    `activeKind` round-trips through the Protocol codec unchanged.
  - Regression — every existing prompt/render/protocol oracle stays green with no
    golden regeneration.

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | This spec; review to the standing session; fold. | `doc/spec-header-prompt-input.md` | — | — |
| 2 | Add `PromptStatusViewState::activeKind`; set it from `prompt.request()->kind` in `promptStatusView` (independent of `computePromptLayout`); round-trip it in the Protocol codec. | `include/ssg/StatusQueue.h`, `src/runtime/snapshot.cpp`, `src/Protocol.cpp` | runtime oracle (activeKind published for palette w/ nullopt view; present kind for footer prompt; nullopt when none) + codec round-trip | authoritative single source; server owns state |
| 3 | Derive the app's `pickerOpen`/`findOpen`/`replaceOpen`/`textPromptOpen` from `promptStatus.activeKind`; leave value reads on `promptStatus.prompt`. | `apps/ssg_main.cpp` | (covered by the runtime oracle + manual/visual) | open-detection independent of layout view; no footer-prompt change |
| 4 | Gate; VISUAL SIGNOFF (type into palette + file finder); code review to the standing session; fold; merge to master and push. | — | `bash scripts/check.sh` green | no footer-prompt behavior change |

## Rationale (optional, skippable)

The header-hosted palette landed with spec-chrome-stacks (the palette query moved
into the header input line, `promptRowCount(Palette)` became 0). That correctly
changed WHERE the palette draws, but the app still inferred "is the palette open"
from the palette's now-absent footer view. The right contract is that the server
publishes what prompt is active; the client renders and routes by it. This spec
makes that explicit with one added field, and pins it so the header/footer
hosting of a prompt can change again without touching input routing.
