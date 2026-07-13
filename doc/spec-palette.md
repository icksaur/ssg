# Spec: command palette and fuzzy finder

## Goals

Give the editor a keyboard-first command palette and fuzzy finder that is
responsive at every keystroke while keeping the authoritative catalog and command
execution server-owned.  The interaction has two surfaces: a header query with
inline ghost-text completion for the common "I know the verb" case, and a
read-only results pane for browsing and fuzzy scanning.  Both reuse existing
machinery (the header status area, the pane/scrollbar/selection renderer) rather
than a bespoke popup.

## Server and client responsibilities

The palette follows the same split as the leader key: the server owns the
authoritative data and the effect; the client owns the latency-sensitive
derivation.

- **Server (authoritative).**  The library publishes the **candidate list** - the
  set of things the palette can act on for the current mode: registered command
  descriptors (`{id, label}`) for the command palette, workspace file paths for
  the file finder, symbols for symbol search.  The server also owns **execution**:
  activating a candidate dispatches its command through the one command registry,
  exactly as a keybinding would.  The palette being open is `prompt` focus
  (authoritative session state, per `doc/spec-navigation.md`).
- **Client (responsiveness).**  The client fuzzy-filters and ranks the published
  candidate list against the query locally and maintains the selected index, so
  typing never round-trips.  This mirrors client-local leader resolution: a pure
  function of authoritative server state (the candidate list) plus local input
  (the query).
- **Presentation stays server-owned.**  Like the leader hint, the client reports
  its derived view - the current query and the ordered visible candidates with the
  selected index - as per-client presentation input, and the library places the
  header query, ghost-text completion, and results-pane rows into the cell grid
  with theme roles.  The client draws server-described cells and invents no UI.

Cross-client consistency is intentionally weak here, as with per-client viewport
and leader hint: two clients may show a slightly different fuzzy ordering, which
is acceptable because execution resolves to the same authoritative command id.

The existing library `SearchController` ranking becomes the **reference ranker**:
it defines the canonical fuzzy-scoring algorithm as the oracle and remains the
ranker for clients that cannot rank locally (the initial browser client may use
it before a local ranker lands).  The TUI ranks locally against the published
candidate list.

To avoid two rankers writing the same shared state, the ranked view is **not**
shared session state.  The shared `SearchViewState.results`/`query`/
`selected_index` fields are retired from the palette path; the authoritative
shared palette state is only the per-mode candidate list plus which client (if
any) has the palette open.  A client's query, ranked order, and selection are its
own reported presentation view.  A server-ranking client reports nothing and
instead requests the reference ranker, which computes **that client's** view
without touching another client's; it is never a second authority over an
already client-ranked view (P5).

### Relationship to I17

Client-side fuzzy ranking is permitted by the I17 carve-out for a
latency-sensitive derived view: it is a pure function of the server-published
candidate list and the local query, the authoritative catalog and command
execution stay server-owned, and the library owns presentation placement and
color.  This is the same sanctioned pattern as client-local leader resolution.

## Design

### Candidate list

The library publishes, per palette mode, a `PaletteCandidate { id, label,
detail }` list on the snapshot (its own small section, per-session for commands,
workspace-derived for files/symbols).  `id` is the command id or navigation
target; `label` is the fuzzy-matched display text; `detail` is optional secondary
text (e.g. the bound key sequence for a command, or the directory for a file).
The candidate list changes only when the catalog or workspace changes, not per
keystroke, so it is not on the typing hot path.

### The palette is a prompt

`focus == prompt` holds exactly when a prompt surface is active
(`doc/spec-navigation.md` N1), so the palette is a **`PromptKind::palette`**
prompt, not an ad-hoc mode.  Unlike the find/replace/settings prompts, which
render their controls in the reserved 1-3 rows below the tab bar, the palette
prompt renders its query in the header status area and its candidates in the pane
projection (below).  Opening it activates the prompt surface (so N1 holds and
focus becomes `prompt`); its text-input value is the query, edited through the
prompt text-input path and reported for header presentation.

### Header query and ghost-text completion

When the palette is open, the header shows the query the user is typing.  After
the query, the client's top-ranked candidate is shown as **ghost text** (the
remaining characters of the best completion) in a dim theme role, fish-style.
Accepting the completion is a **client-local query edit** that fills the query
input with the top candidate's label - it is not a command and mutates no
document or command state, only the client's reported query view.  Enter on the
query executes the selected candidate.  The query and ghost text are placed by the
library in the header status area using theme roles; the client reports the query
and the top completion as presentation input.

### Results pane

For browsing, the ranked candidates render into a **read-only results pane** that
reuses the document pane's cell/scrollbar/selection rendering.  While the palette
is open, the active pane projects the results list instead of the document: one
candidate per row (`label` left, `detail` right-aligned/dim), the selected row
highlighted with the selection role, and the shared scrollbar abstraction for
overflow.  The client reports only the bounded **visible window** of ranked rows
(viewport height), not the full list, so per-keystroke reporting re-diffs at most
a screenful.  The underlying document state is unchanged and restored (including
scroll offset) when the palette closes.  When no document/tab is open, the palette
projects over the empty-state surface and restores it on close.  With split panes,
the palette projects into the active pane only; other panes keep their documents.
This stays within the "cells, no overlays, non-modal" invariants (I7, I19) - it is
a projection of the active pane, not a floating popup.

Pane takeover fits `prompt` focus: `palette.open` opens the palette, focus becomes
`prompt`, keys drive the palette (query edit, `palette.next`/`previous`,
`palette.execute`), and closing restores the prior focus and the document
projection.

### Commands and flow

The existing `palette.*` commands are retained but re-scoped so ranking is a
client concern:

- `palette.open` / `palette.close` open and close the palette (focus in/out).
- The query is edited through the prompt text-input path (client-local, reported
  for header presentation), not a per-keystroke server rank.
- `palette.next` / `palette.previous` move the client selection; the client
  reports the new selected index.
- `palette.execute` dispatches the **selected candidate's command id**.  Its typed
  argument becomes the candidate id (previously arg-less), so `required-commands.json`
  records the new argument shape.  The server validates that the id is a member of
  the currently published candidate set for the open palette mode **and** that it
  passes normal capability gating before dispatching through the registry; an id
  outside the published candidates or lacking capability is rejected, so a client
  cannot reach a command the palette never offered.

## Invariants

- P1 (server-owned catalog and execution): the candidate list and command
  execution are authoritative library state through the typed API; a client never
  invents a command or executes anything not in the registry.
- P2 (client-side ranking): fuzzy filtering, ranking, and selection are derived
  client-local from the published candidate list and the query; typing does not
  round-trip.
- P3 (server-presented palette): the header query, ghost-text completion, and
  results-pane rows are placed by the library in the cell grid with theme roles;
  the client reports its derived view and draws server-described cells.
- P4 (non-modal projection): the results pane is a projection of the active pane,
  not an overlay or modal; the underlying document state is preserved and
  restored on close.
- P5 (reference ranker): the library ranker defines the canonical scoring oracle
  and serves clients that do not rank locally.  A client that ranks locally MUST
  match the reference ranker's order for the oracle fixture set; per-client views
  may differ only when a client deliberately uses a different ranker.  The TUI
  ranker is tested to match the oracle.

## Considerations

- **Per-client palette vs shared focus (decided: single interactive client).**
  `doc/spec-navigation.md` makes focus per-session shared, but the palette query,
  ranking, and selection are per-client.  SSG assumes a **single interactive
  client**, so the palette prompt and its focus are per-session and this tension
  does not arise; multi-client palettes (which would require per-client focus and
  prompt surfaces) are explicitly out of scope.  A second attached client
  observing another's open palette is not supported.
- The reported derived view is the bounded visible window (viewport rows), not the
  full ranked list; the browser wire message for reporting the view is deferred
  with the low-latency input model, exactly as the leader hint deferred its wire
  message.  The initial server-ranked browser path reports nothing and requests
  the reference ranker, which writes only that client's view.
- Performance: client-side fuzzy over thousands of candidates (large workspaces)
  can be slow; the initial implementation ranks the full list per keystroke.
  Deferred optimizations (server-side pre-filtering to a bounded candidate window,
  incremental/streamed candidates, a worker-thread ranker) build on the candidate
  seam and are out of scope here.
- The candidate `detail` for commands should carry the bound key sequence so the
  palette doubles as keybinding discovery; this depends on the keymap work (M6).
- Command candidates currently publish `label == id` because `p0_command_descriptors`
  carries no human-readable label; human labels and the key-sequence `detail` land
  with the command-metadata/keymap work (M6).  Until then the palette rows and the
  ghost completion show command ids.
- The authoritative fuzzy ranker is `ssg::palette_rank` (`src/palette.cpp`).  The
  retained `SearchController` ranker mirrors its scoring and tiebreak (score desc,
  label then id ascending) so the reference/browser path cannot diverge from it.
- Ghost-text completion must never change document or command state; it is a
  presentation hint until explicitly accepted.
- `SearchViewState.palette_open` is derived from `focus == prompt` with a
  `PromptKind::palette` surface, not an independent flag.
- The `SearchController` palette mutators (`open_palette`, `update_palette_query`,
  `select_next`/`previous`, `execute_palette`, `rank_palette`) are retained solely
  as the reference/browser ranker and are not on the TUI path.

## Risks and mitigations

- Two-ranker divergence: avoid a server ranker competing with the client on the
  same client's view.  The server publishes candidates only; the client owns its
  ranked view.  The library ranker is used by a client as its own ranker or as the
  scoring oracle, never as a second authority over an already client-ranked view.
- Result-pane state leakage: restoring the document projection on close is covered
  by a test that opens the palette, executes/cancels, and asserts the pane returns
  to the prior document and scroll offset.

## Acceptance (Definition of Done)

- Observable: pressing the palette key opens the palette in `prompt` focus; typing
  filters instantly with no per-keystroke round trip; the header shows the query
  with dim ghost-text of the top completion; the results pane lists ranked
  candidates with the selected row highlighted and a working scrollbar; Enter
  executes the selected command through the registry; Escape closes and restores
  the document.  The file-finder mode opens a file the same way.
- The candidate list, query, ghost text, and results rows are all placed by the
  library; the client reports its derived view and renders only cells.
- Gates: `ctest --preset dev` green.
- Oracles: the library reference ranker compared against hand-authored
  query/candidate/expected-order fixtures; a client-ranker test asserting the same
  order as the reference ranker for the fixture set; a palette-projection test
  proving the results pane renders ranked rows with the selected row highlighted
  and restores the document on close; a header test proving the query and
  ghost-text completion render in their theme roles; an execution test proving
  `palette.execute` dispatches the selected candidate's command id through the
  registry.

## Plan

| Step | Work | Files | Oracle |
|---|---|---|---|
| 1 | Publish a per-mode `PaletteCandidate {id,label,detail}` list on the snapshot from the command registry and workspace in a new `include/ssg/palette.h`; retire shared `SearchViewState` results/query/selection from the palette path | `include/ssg/palette.h`, `src/palette.cpp`, `src/runtime/*.cpp`, `src/protocol.cpp`, catalog/round-trip tests | candidate list matches the registry/workspace; round-trip |
| 2 | Add the `PromptKind::palette` surface and render the results pane: project ranked candidates into the active pane with selection highlight and scrollbar, restore document/empty-state on close | `include/ssg/prompt.h`, `src/ui_layout.cpp`, `src/render.cpp`, `tests/test_ui_layout.cpp`, `tests/test_render.cpp` | projection render + restore-on-close test |
| 3 | Header query + ghost-text completion in the status area | `src/ui_layout.cpp`, `src/render.cpp`, `tests/test_ui_layout.cpp` | header query/ghost-text render test |
| 4a | Client-local fuzzy ranker in the TUI reporting the bounded query/selected view | `apps/ssg_terminal.{h,cpp}`, `tests/test_ssg_app.cpp` | client ranker matches reference-ranker order for the fixture set |
| 4b | Wire `palette.*` under `prompt` focus; `palette.execute` sends the selected id; server validates membership + capability | `apps/ssg_main.cpp`, `src/runtime/navigation.cpp`, `data/required-commands.json`, `tests/*` | PTY palette demo; execute-validation test |
