# spec-diff-ux

Status: DRAFT — UX ONLY. No architecture, ownership, or implementation mechanism.
This is the contract the follow-on architecture spec must satisfy.

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
