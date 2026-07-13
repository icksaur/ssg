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
candidate list.  There are not two rankers that must agree on shared state: each
client owns its own ranked view; the server owns only candidates and execution.

## Design

### Candidate list

The library publishes, per palette mode, a `PaletteCandidate { id, label,
detail }` list on the snapshot (its own small section, per-session for commands,
workspace-derived for files/symbols).  `id` is the command id or navigation
target; `label` is the fuzzy-matched display text; `detail` is optional secondary
text (e.g. the bound key sequence for a command, or the directory for a file).
The candidate list changes only when the catalog or workspace changes, not per
keystroke, so it is not on the typing hot path.

### Header query and ghost-text completion

When the palette is open, the header shows the query the user is typing.  After
the query, the client's top-ranked candidate is shown as **ghost text** (the
remaining characters of the best completion) in a dim theme role, fish-style.
Accepting the completion (a bound key) fills the query with the top candidate;
Enter on the query executes the selected candidate.  The query and ghost text are
placed by the library in the header status area using theme roles; the client
reports the query and the top completion as presentation input.

### Results pane

For browsing, the ranked candidates render into a **read-only results pane** that
reuses the document pane's cell/scrollbar/selection rendering.  While the palette
is open, the active pane projects the results list instead of the document: one
candidate per row (`label` left, `detail` right-aligned/dim), the selected row
highlighted with the selection role, and the shared scrollbar abstraction for
overflow.  The underlying document state is unchanged and restored when the
palette closes.  This stays within the "cells, no overlays, non-modal" invariants
- it is a projection of the pane, not a floating popup.

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
- `palette.execute` dispatches the **selected candidate's command id** directly.
  Because the client owns the ranked view and selection, it sends the resolved id;
  the server validates and dispatches it through the registry.

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
  and serves clients that do not rank locally; per-client ranked views need not be
  byte-identical across clients.

## Considerations

- Performance: client-side fuzzy over thousands of candidates (large workspaces)
  can be slow; the initial implementation ranks the full list per keystroke.
  Deferred optimizations (server-side pre-filtering to a bounded candidate window,
  incremental/streamed candidates, a worker-thread ranker) build on the candidate
  seam and are out of scope here.
- The candidate `detail` for commands should carry the bound key sequence so the
  palette doubles as keybinding discovery; this depends on the keymap work (M6).
- Ghost-text completion must never change document or command state; it is a
  presentation hint until explicitly accepted.

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
| 1 | Publish a per-mode `PaletteCandidate` list on the snapshot from the command registry and workspace, with `detail` | `include/ssg/search.h` or a new palette header, `src/search.cpp`, `src/runtime/*.cpp`, `include/ssg/session_snapshot.h`, `src/protocol.cpp`, catalog/round-trip tests | candidate list matches the registry/workspace; round-trip |
| 2 | Render the results pane: project ranked candidates into the active pane with selection highlight and scrollbar, restore document on close | `src/ui_layout.cpp`, `src/render.cpp`, `tests/test_ui_layout.cpp`, `tests/test_render.cpp` | projection render + restore-on-close test |
| 3 | Header query + ghost-text completion in the status area | `src/ui_layout.cpp`, `src/render.cpp`, `tests/test_ui_layout.cpp` | header query/ghost-text render test |
| 4 | Client-local fuzzy ranker in the TUI reporting query/selected view; wire `palette.*` and open under `prompt` focus; `palette.execute` sends the selected id | `apps/ssg_main.cpp`, `apps/ssg_terminal.{h,cpp}`, `tests/test_ssg_app.cpp` | client ranker matches reference-ranker order; PTY palette demo |
