# spec-diff

Status: UX layer FINALIZED; ARCHITECTURE layer DRAFT (this addition). The UX
section above is the contract; everything from "## Architecture" on is the
class-collaboration design that ships it.

## Goals

Let a user (or a following agent) see git status and live diffs from the header,
a bar, diff tabs, and footer follow-mode — driven by the already-shipped Git diff
source, with no manual refresh.

## Expectations

- The header shows path and filename today; filename moves to the tab and is
  removed from the header entirely.
- Outside a git repo, the header shows [path] only, as today.
- Inside a git repo, the header shows [path] [branch name], ideally with a branch
  glyph before the branch name.
- The header is for durable context (directory, git branch/status); the footer is
  for fluid/transient status (follow state, leader key, search hit count, etc.).
  Both header and footer fields are configured/rendered via a callback (a
  registered list of field providers), not hardcoded layout — this UX spec states
  WHICH fields exist and where; the callback/config mechanism itself is an
  architecture concern.
- The footer's existing `git_branch`/`git_repository` status fields are REMOVED
  from the footer now that branch is a durable header field; the footer does not
  duplicate header content.
- Clicking the header path opens the bar in files view.
- Clicking the header branch name opens the bar in git status view.
- Clicking the header element (path or branch) whose view is already open in the
  bar closes the bar (toggle off).
- The bar's git status view lists staged and unstaged changed files, respecting
  `.gitignore`.
- The git status bar list refreshes LIVE while open — items appear/disappear as
  the underlying git status changes, with no manual refresh action required.
- Each git status bar item shows a single colored status letter before its name:
  A/M/D/R/etc., using whatever status enum the library exposes.
- Clicking a git status bar item opens a diff tab for that file.
- If a diff tab for that file is already open, clicking the item focuses it
  instead of opening a new one.
- Opening a diff for a file that already has a regular edit tab open does NOT
  reuse or focus that edit tab; a diff tab is separate.
- The same document can be open simultaneously in an edit tab and a diff tab, in
  two different tabs.
- A diff tab's tab text carries a glyph that visually disambiguates it from an
  edit tab (starting point: a "D"; exact color/style is theme-derived, not a
  hardcoded color).
- Opening the git status bar (and any other new bar/view action introduced here)
  is exposed as a command in the command palette.
- Every new parameterless action introduced here is available as a command in the
  command palette.
- As with existing commands, every new command is bindable to a key.
- A diff tab's document view is READ-ONLY (no edits, no undo/redo) for this
  version; it otherwise behaves like an edit-tab document view (scrolling,
  selection, search, etc.).
- The editor has a follow mode with two states, reusing the existing terms:
  "following" or "paused". State labels are configurable strings (not hardcoded
  copy), starting with these two defaults.
- The current follow state is shown in the footer as text: "following" or
  "paused".
- Clicking the footer follow-state text toggles between following and paused.
- Follow-mode pause scope is per-session (shared across all clients of the same
  session), consistent with the existing shared `FollowEditsModel` state; it is
  NOT per-client.
- While following, when the diff source detects a new edit to a git-tracked
  (non-ignored) file, the editor opens (or focuses, if already open) that file's
  diff tab and jumps to the new change.
- Follow jumps BETWEEN diff tabs as different files are edited in turn — the
  active/focused diff tab tracks whichever file most recently changed, moving
  from tab to tab as the changed file changes; it is not limited to one fixed tab.
- Jumping to a change centers it vertically in the diff view.
- If a change's full extent does not fit in the vertical diff view space, the top
  of the change is always kept visible; the bottom may scroll out of view.
- Follow-jump works for a newly created file (it opens and jumps into the new
  file's diff).
- Follow-jump does NOT trigger for a `git rm` (deletion) — there is no content to
  jump to.
- A follow-triggered jump/tab-open never changes the bar's current view — the bar
  stays in whatever view (files/git status/closed) the user last left it in.
- If a file's diff tab is open and the file returns to clean (reverted, staged
  changes committed, etc.), the tab stays open — it is never auto-closed — and
  shows no changes (an empty diff), since the diff source no longer reports the
  file. The user closes it manually like any other tab.
- A `git rm`'d (deleted) file's diff renders as the entire prior file content shown
  as removed, using the diff source's existing deleted-file view (which retains
  the prior content); its diff tab likewise stays open until manually closed.
- Any user interaction with a view disables follow mode: editing text, switching/
  selecting tabs, or scrolling a document view in any tab all turn follow off.

## Open questions for review

- Does opening a NEW diff tab (not just jumping within an already-open one) count
  as a "user interaction" that could re-disable follow, given follow itself is
  what opened it? (Assumed: no — the follow-triggered open/jump itself must not
  disable follow; only INDEPENDENT user actions do.)
- Is there a maximum number of diff tabs, or a policy for many simultaneously
  open/followed diffs (e.g. an agent editing many files quickly)?
- Branch glyph: any preference/fallback if the terminal/font lacks the glyph, or
  is a plain-text fallback acceptable everywhere?
- Command/keybinding names for the new actions (open files bar, open git status
  bar, toggle follow, etc.) — left to the architecture spec unless there's a
  naming preference now.

## Architecture

### Design

#### Current-object survey (Phase 1 findings)

Four areas were audited before deciding what to split/generalize/recompose:

- **Header/footer fields** — `ShellState`/`computeShellLayout` (`src/ShellState.cpp`)
  already collapse-rank and lay out two SEPARATE ordered lists,
  `ShellLayoutRequest.headerFields`/`footerFields` (`include/ssg/ShellState.h`).
  But the CONTENT of every field is hardcoded inline in
  `EditorRuntime::Impl::shellView()` (`src/runtime/snapshot.cpp:94-97`) — there is
  no provider/callback abstraction, `data/ui/status_fields.json` is consumed only
  by a fixture test, and `git_branch`/`git_repository` are dead fields (declared,
  never populated). No header/footer region is hit-tested for clicks anywhere.
  **This area needs real generalization** — the field-provider abstraction the UX
  spec calls for does not exist yet.
- **Bar/tree/panel** — `TreeModel` (`include/ssg/TreeModel.h`/`.cpp`) is ALREADY a
  correctly-generic multi-provider store (`ShellState state({"Files","Git",
  "Symbols"})` is an existing test); the front provider is the active one; open/
  close and provider-cycle commands already exist. `GitTreeRecord`/`GitTreeStatus`
  (Added/Modified/Deleted/Renamed/Untracked) is already the right FLAT shape for a
  git-status list. What's missing is a real feed: `TreeProviderSnapshot::fromGit`
  is called only by tests today; nothing wires it to the shipped `GitDiffSource`/
  `DiffModel`. **This area needs a new adapter, not a new abstraction** — the
  container is already correct.
- **Tabs/document view** — `TabManager` ALREADY has `TabKind::{Document,LiveDiff,
  ReadOnlyOutput,SearchResults,TreeView}` and `DocumentMode::{Edit,ReadOnly,Diff}`;
  `Document::apply()` already rejects edits for `ReadOnly`/`Diff` mode; tab dedup
  is already kind-aware (a `Document` tab and a `LiveDiff` tab for the same file
  coexist without conflict today); the renderer already accepts an optional
  `DiffFileView*` per document with no mode branching. **This area needs almost no
  new types** — the UX's "edit tab + diff tab, same document, two tabs" requirement
  is already representable. What's missing: (a) a glyph prefix on `LiveDiff` tab
  titles, (b) a runtime path from "click a git-status bar item" to "open/focus a
  `LiveDiff` tab" (the `diff.open_file` command already resolves a
  `DiffOpenTarget` but intentionally does not open a tab — session-assembly
  binding it to `TabManager` is exactly the "later task" `workspace-live-diffs.md`
  deferred, and is now due).
- **Follow mode + commands** — `FollowEditsModel` states are `Following`/`Paused`;
  its ONLY pause trigger is `NavigationClass::User` inside `applyNavigation()`.
  Text edits (`bindEdit` path) never call `recordNavigation` and so currently do
  NOT pause follow — a genuine gap against this spec's "edits pause follow"
  expectation. The command-catalog cascade (required-commands.json + owner count,
  `test_required_commands.cpp`, `command_cases.h`, protocol round-trip, Lua-parity)
  is well-trodden (used for `prompt.next`/`prompt.previous`); the palette
  auto-discovers every cataloged command with `"palette":true` (no separate
  registration) and any cataloged command with `"keymap":true` is automatically
  bindable (`KeymapMatcher::validate` checks syntax, not command existence).
  Footer/header click-to-command hit-testing does not exist for ANY field today
  (`HitTester.cpp` has no header/footer region) — this is net-new, not a
  generalization of something existing.

#### Target object model (Phase 2 — splits, generalizations, recompositions)

1. **Status field providers (NEW).** Introduce a `StatusFieldProvider` — a
   library-owned callback shape, one per field id, that reads runtime/session
   state and yields a field's live value plus an optional bound command id to
   dispatch on click. Two ordered PROVIDER lists (header, footer) replace the two
   hardcoded literal lists in `shellView()`; `data/ui/status_fields.json` becomes
   real config for id/label/collapse-rank/HEADER-OR-FOOTER placement (not just a
   test fixture) while the live VALUE and click behavior stay compiled providers
   (mirrors the existing split between compiled `CommandSet` descriptors and the
   validated `required-commands.json` catalog — data drives shape, code drives
   behavior). `StatusField` gains an optional bound command id. New header fields:
   `path` (click -> open bar to Files, toggle-closes if already showing Files),
   `branch` (click -> open bar to Git status, toggle-closes if already showing
   Git; glyph + name, present only inside a git repo). Footer's dead `git_branch`/
   `git_repository` fields are REMOVED (superseded by the header field, per UX
   spec); a new footer `follow` field's value comes from `FollowEditsModel`
   directly, click -> the new toggle command (below).
2. **Header-click bar commands (NEW, library-owned per I25/I17).** Two
   parameterless commands — e.g. `panel.show_files`, `panel.show_git_status` —
   each meaning "show bar to this provider, or close the bar if it is already
   showing this provider" (the UX's open/toggle-closed rule). This is a FEATURE
   decision (what a click means), not a client-composed one: the client sending
   "open Files" vs "open Git" IF it also had to decide close-vs-open based on
   current state would replicate the exact class of I17/I25 violation just fixed
   in the prompt-fulfillment milestone (a client deciding which of two
   library-meaningful actions to take from view state it must read to decide).
   Two named commands (rather than one `panel.show_provider{id}` parameterized
   command) keep this palette/keymap-simple, matching the existing pattern of
   distinct named commands for distinct fulfillments (`external.reload`/
   `keep_buffer`/`open_diff`).
3. **Git status tree feed (NEW adapter, no new container).** A pure function/
   small adapter mapping the shipped `DiffModel`'s `DiffViewState` to
   `vector<GitTreeRecord>` (already the right flat shape), producing a
   `TreeProviderSnapshot::fromGit` refresh whenever the diff revision changes.
   Wired at the same runtime seam that already applies `GitDiffScan` results
   (`EditorRuntime::applyGitDiffScan`) — after updating `DiffModel`, also refresh
   the Git tree provider. No new panel/tree type; this is pure recomposition of
   two already-correct pieces (`DiffModel` as source of truth, `TreeModel` as the
   generic container) that were never connected.
4. **`DiffFileStatus` (NEW small field on `DiffFileView`).** Today
   `DiffFileView::deleted` and `previousPath` derive Deleted/Renamed, but "file has
   no baseline content" (Added/untracked) is discarded during `updateGitFile`
   ingestion — nothing downstream can currently tell Added from Modified. Add a
   `DiffFileStatus{Added,Modified,Deleted,Renamed}` computed once, at ingestion, in
   `DiffModel::updateGitFile`, with an EXPLICIT precedence order (a file can be
   simultaneously renamed and content-modified — one enum value must still be
   chosen deterministically): `workingContent` absent -> Deleted (highest
   precedence — no content to show regardless of any other fact); else baseline
   absent -> Added; else `previousPath` set -> Renamed (a rename is reported as
   Renamed even when its content also changed, matching `git status` porcelain,
   which reports `R` with a similarity score rather than `M` for a modified
   rename); else Modified (default/lowest precedence). Stored on `DiffFileView`.
   This directly answers "colored status letter" without a new parallel data
   source, and is a minimal, backward-compatible field addition (existing
   consumers ignore it). Oracle: hand cases for every pairwise combination
   (renamed+modified, renamed+unmodified, added, deleted, plain modified) plus a
   ground-truth comparison against real `git status --porcelain` output (mirrors
   `test_git_repository.cpp`'s existing pattern).
5. **Diff-tab session assembly (RECOMPOSE existing pieces, one new seam, TWO
   entry points into ONE binding function).** A single library function —
   "open-or-focus the `LiveDiff` tab for file X" — resolves `diffOpenFile(view)`
   (existing, unmodified) into a `DiffOpenTarget`, then asks `TabManager`
   for-or-creates a `TabKind::LiveDiff` tab keyed by the target's stable file id
   (kind-aware dedup already exists, confirmed above) with `DocumentMode::Diff`
   (already edit-blocking). It has exactly two callers, kept structurally
   separate so one cannot be mistaken for the other:
   (a) USER path — a git-status bar item click dispatches `tree.activate` on a
   Git-provider node (`NavigationClass::User`, so it pauses follow like any other
   user navigation, per the EXISTING `applyNavigation` rule — no new pause logic
   needed here, tab/pane/scroll/cursor navigation already pauses follow today);
   (b) FOLLOW path — the diff-source-driven auto-open (UX section) calls the same
   binding function directly under `NavigationClass::Programmatic`, never through
   `tree.activate`, so it cannot accidentally reuse the user-navigation pause
   trigger. This structural separation is what actually prevents the
   self-triggering bug the Considerations section warns about — it is not merely
   a documentation note, it is why two distinct call sites exist.
   DELETED-FILE TARGETS (a `DiffOpenTarget` with `deleted=true`) MUST NOT go
   through the normal `Workspace::openFile` disk-read path, which would fail (the
   file no longer exists on disk). The binding function branches on
   `DiffOpenTarget::deleted`: for a deleted target it constructs the `LiveDiff`
   tab's content directly from the `DiffFileView`'s own retained prior content
   (already present in the model — `DiffFileView` retains baseline content for a
   deletion per the existing content-consumer contract), never touching disk. This
   is the same mechanism the UX section's "`git rm` renders as entire file
   removed" expectation depends on.
6. **`LiveDiff` tab title glyph (small, additive).** Tab-title composition (the
   label-building step in `runtime/snapshot.cpp`) prefixes a theme-derived glyph
   for `TabKind::LiveDiff` tabs; `TabLabel` needs no structural change (already
   carries a free-form title string). Glyph color/style is theme-derived
   (deriveSelectionFill-style resolution), never a hardcoded terminal color.
7. **Follow-mode pause-on-edit (behavior addition, not redesign).** Tab-switch,
   pane-switch, scroll, and cursor movement ALREADY pause follow today via the
   EXISTING `NavigationClass::User` classification in `applyNavigation()` — this
   item adds ONLY the missing edit-pause trigger, it does not touch or duplicate
   the existing navigation-pause path. Add an explicit
   `FollowEditsModel::notifyLocalEdit()` distinct from `applyNavigation()` (edits
   are not navigation, so folding them into `NavigationClass` would be a category
   error) that pauses exactly like a User navigation. The call site MUST be the
   single most-downstream point where a user-originated edit is committed to a
   `Document` — not scattered across each command TYPE (`bindEdit` alone is
   insufficient: paste/cut, undo/redo, replace, and multi-cursor edits are
   separate call paths that would each need their own hook and would regress
   independently if added ad hoc). Concretely: hook at the point the command
   executor advances the session revision as a result of a `Document` mutation
   that originated from a USER-classified command (excluding Lua-driven and
   recovery/replay mutations, which are not user edits and must NOT pause — same
   exclusion Considerations/Risks already requires). This keeps ONE call site
   authoritative for "a user just edited something," matching how
   `NavigationClass::User` is already a single classification point rather than
   one flag per navigation command.
8. **`follow_edits.toggle` (NEW library command; do not client-compose).** The
   footer's single click text must flip between Following/Paused; deciding WHICH
   underlying transition to perform based on currently-published mode is exactly
   the class of decision I17's exhaustive derived-view set does NOT permit
   client-side (it is not leader resolution, not a palette rank+selection, not
   deriving an input field from a keystroke) — so add one library-owned
   `follow_edits.toggle` command that reads its own current state and flips it.
   `follow_edits.pause`/`resume` remain for direct/programmatic use.
9. **Header/footer/bar click-to-command hit-testing (NEW).** `HitTester` gains
   region kinds for header/footer status fields (and confirms tree-item hit
   regions already cover bar items — Phase 1 found `tree.activate` already
   dispatches from a hit tree node). The SERVER publishes which screen regions are
   clickable and which command each maps to (as part of the existing
   snapshot/ShellViewState publication, extending `StatusField` with its bound
   command id per item 1) — the client only translates a raw click coordinate to
   the published region and command id, never invents the mapping (I17/I25: the
   region-to-command mapping is a feature decision, published by the library).
10. **Git branch source (Consideration, not yet confirmed).** The shipped
    `GitRepository` adapter (`doc/spec-git-diff-source.md`) exposes changed-file
    scans and a baseline identity token, but was not designed to expose the
    current branch NAME. Verify during implementation whether a narrow
    `currentBranch()` query needs adding to the `GitRepository` interface (still
    an I25-compliant adapter extension — reading a ref name is exactly the kind of
    raw repository read the adapter already owns) or whether the baseline-identity
    token can be reused/extended for display.

### Invariants

- I17 — the header-click bar commands (item 2) and `follow_edits.toggle` (item 8)
  are library-owned; no client composes an open-vs-close or pause-vs-resume
  decision from view state.
- I25 — the git-status tree feed (item 3) and any branch-name read (item 10)
  route through the already-injected `GitRepository`/`GitDiffSource`; no new
  client-side Git access is introduced.
- Document lifetime (`doc/spec-document-lifetime.md`) — a `LiveDiff` tab and a
  `Document` tab referencing the same file are two independent tabs; closing one
  must not destroy the document out from under the other (existing per-kind dedup
  and weak `FileDocumentId` handles already guarantee this — verify by test, not
  by new mechanism).
- Diff content-consumer contract (`doc/features/workspace-live-diffs.md`) —
  `DiffModel` still never invokes Git; `DiffFileStatus` (item 4) is computed from
  data already passed into `updateGitFile`, not by a new Git read.
- Reveal-not-pause (existing) — the diff-tab session-assembly binding (item 5)
  opening/focusing a tab under follow must use PROGRAMMATIC navigation, so it does
  not itself trip the new pause-on-edit/pause-on-navigation behavior (item 7).

### Considerations

- The header/footer field-provider abstraction (item 1) is the largest net-new
  piece; get its shape (provider signature, where the provider list is assembled
  — likely `EditorSessionBuilder.cpp`, mirroring `p0CommandDescriptors()`) right
  before wiring individual fields, since every other header/footer expectation in
  the UX section depends on it.
- `DiffFileStatus` (item 4) must be computed ONCE at ingestion and carried on
  `DiffFileView`, not re-derived ad hoc at render time in multiple places (a
  classic implicit-coupling smell — two call sites re-deriving the same fact
  differently).
- The "close bar if already showing this provider, else show it" toggle (item 2)
  needs its own small state machine or explicit rule at the point `panel.show_*`
  is handled: reuse `ShellState`'s existing open/active-provider fields; do not
  invent parallel state.
- Follow-jump "does not change the bar's current view" (UX spec) means the
  diff-tab session-assembly path (item 5) must NOT call the header-click bar
  commands (item 2) even though both ultimately touch tabs/panels — keep these
  two paths structurally separate so one cannot accidentally trigger the other.

### Risks and Mitigations

- Pause-on-edit (item 7) firing too eagerly (e.g. on programmatic/recovery-replay
  edits, not just user typing) ⇒ scope `notifyLocalEdit()` to genuinely
  user-originated edit dispatch only, mirroring how `NavigationClass::User` is
  scoped today; add a test with a programmatic/recovery edit that must NOT pause.
- Adding a bound command id to `StatusField` (item 1) could tempt ad hoc parallel
  hit-test paths per field ⇒ one generic field-click handler keyed by field id,
  not one handler per field.
- `DiffFileStatus` (item 4) silently wrong for an edge case (e.g. a renamed file
  that is ALSO modified) ⇒ oracle against real `git status` porcelain output
  (mirrors the existing `test_git_repository.cpp` ground-truth pattern).

### Acceptance (Definition of Done)

- Observable: every UX-section expectation above is demonstrable in the TUI (and
  ideally browser client) against a real git repo, with visual signoff before
  merge (this is user-visible UI work).
- Gates: `bash scripts/check.sh` green with and without `SSG_TREESITTER`.
- Oracles: each numbered item above gets its own test per its existing project
  pattern (hand-case for `DiffFileStatus`, ground-truth-vs-real-`git`-status for
  the tree feed, a pause/no-pause transition table for follow, a dedup/lifetime
  test for coexisting Document+LiveDiff tabs, a hit-test coordinate table for new
  click regions).

### Plan

Ordered so each step is independently gate-able; a detailed file-list/oracle Plan
table is written per-step at implementation time (this sketch fixes ORDER and
per-step SCOPE now so the design is reviewable and implementable without
re-discovery):

| # | Step | Scope | Oracle intent |
|---|------|-------|----------------|
| 1 | `DiffFileStatus` on `DiffFileView` (item 4) | `DiffModel.h`/`.cpp`; no consumers yet | hand cases incl. renamed+modified precedence; ground-truth vs `git status --porcelain` |
| 2 | Status field provider abstraction (item 1) + header/footer field lists rewired to providers; remove dead footer git fields | `ShellState`/`EditorSessionBuilder`/`runtime/snapshot.cpp` | header/footer content matches provider output per field id |
| 3 | Header path/branch fields wired; `panel.show_files`/`panel.show_git_status` commands (item 2), full command-catalog cascade | provider callbacks + new commands + cascade files | click-region-to-command table; toggle-open/close transition table |
| 4 | Git status tree feed (item 3): `DiffViewState` -> `GitTreeRecord` adapter, wired at `applyGitDiffScan` seam | new adapter fn; `EditorRuntime` seam | live-refresh test: scan changes -> tree snapshot changes, ground-truth vs real git status list |
| 5 | Diff-tab session-assembly binding (item 5): one function, two callers (user `tree.activate`, follow-triggered programmatic); deleted-target branch reads from `DiffFileView` not disk | new binding fn; `tree.activate` handler; follow trigger call site | dedup/coexistence test (Document+LiveDiff tabs); deleted-file tab renders retained content, no disk read |
| 6 | `LiveDiff` tab title glyph (item 6) | `runtime/snapshot.cpp` label composition; theme glyph resolution | tab label test; theme-color-not-hardcoded invariant test |
| 7 | Follow pause-on-edit (item 7): single authoritative user-edit-commit hook + `notifyLocalEdit()` | `FollowEditsModel`; command-executor revision-advance call site | pause/no-pause transition table incl. Lua/recovery exclusion; existing tab/scroll/cursor pause cases unchanged (regression) |
| 8 | `follow_edits.toggle` command (item 8) + footer follow field wired to it via provider (item 1/3) | `FollowEditsModel` command set + cascade | toggle flips Following<->Paused from either starting state |
| 9 | Header/footer/bar click-to-command hit-testing (item 9) | `HitTester.cpp`; snapshot publishes field click regions | hit-test coordinate table per field; one generic field-click handler (Risks) |
| 10 | Git branch source (item 10, if needed): `GitRepository::currentBranch()` or reuse existing token | `GitDiffSource`/`GitRepository` adapter | branch field shows correct name across checkout/detached-HEAD cases |

Step 1 first because item 4 has zero dependents to break; steps 2-3 unblock every
other header/footer/bar UX line; step 5 depends on steps 3-4 (needs the bar item
and the tree feed to click); step 7 is independent of 1-6 and can run in parallel;
steps 8-9 depend on the provider abstraction (2) and the toggle command (8) resp.

### Rationale (skippable)

Three of four investigated areas (bar/tree, tabs, command cascade) turned out to
already be correctly generalized — `TreeModel`'s multi-provider design and
`TabManager`'s kind-aware dedup were built for exactly this kind of extension
without knowing it yet. That is the "pit of success" already paying off: this
spec adds one adapter (Git status feed) and one binding (diff-tab assembly)
rather than new containers. The one area that was NOT ready — status fields — was
hardcoded from the start (`data/ui/status_fields.json` was aspirational, never
consumed), so it gets the only genuinely new abstraction in this design. Follow
mode and header/footer hit-testing needed small, targeted behavior additions
(pause-on-edit, a toggle command, click regions) rather than redesign, because the
underlying state machines (`FollowEditsModel`, `ShellState`) were already the
right shape — they just never had all their triggers wired.

