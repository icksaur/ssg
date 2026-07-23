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
- Clicking the header path opens the bar in files view.
- Clicking the header branch name opens the bar in git status view.
- Clicking the header element (path or branch) whose view is already open in the
  bar closes the bar (toggle off).
- The bar's git status view lists staged and unstaged changed files, respecting
  `.gitignore`.
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
  edit tab (starting point: a red "D").
- Opening the git status bar (and any other new bar/view action introduced here)
  is exposed as a command in the command palette.
- Every new parameterless action introduced here is available as a command in the
  command palette.
- As with existing commands, every new command is bindable to a key.
- A diff tab's document view behaves like an edit-tab document view in nearly all
  functionality (scrolling, selection, search, etc. all work the same).
- The editor has a follow mode with two states: following, or not following.
- The current follow state is shown in the footer as text: "following" or "not
  following" (exact copy TBD, but always visibly one of the two).
- Clicking the footer follow-state text toggles between following and not
  following.
- While following, when the diff source detects a new edit to a git-tracked
  (non-ignored) file, the editor opens (or focuses, if already open) that file's
  diff tab and jumps to the new change.
- Jumping to a change centers it vertically in the diff view.
- If a change's full extent does not fit in the vertical diff view space, the top
  of the change is always kept visible; the bottom may scroll out of view.
- Follow-jump works for a newly created file (it opens and jumps into the new
  file's diff).
- Follow-jump does NOT trigger for a `git rm` (deletion) — there is no content to
  jump to.
- A follow-triggered jump/tab-open never changes the bar's current view — the bar
  stays in whatever view (files/git status/closed) the user last left it in.
- Any user interaction with a view disables follow mode: editing text, switching/
  selecting tabs, or scrolling a document view in any tab all turn follow off.

## Open questions for review

- Exact footer copy for the two follow states ("following"/"not following" vs
  other wording) — placeholder above, confirm before implementation.
- Does opening a NEW diff tab (not just jumping within an already-open one) count
  as a "user interaction" that could re-disable follow, given follow itself is
  what opened it? (Assumed: no — the follow-triggered open/jump itself must not
  disable follow; only INDEPENDENT user actions do.)
- What happens to an open diff tab for a file that becomes clean (e.g. reverted,
  committed, or `git rm`'d after being modified)? Does the tab close, go stale, or
  show "no changes"? Not specified above — needs an answer.
- Is there a maximum number of diff tabs, or a policy for many simultaneously
  open/followed diffs (e.g. an agent editing many files quickly)?
- Should the git status bar auto-refresh live (new items appearing/disappearing
  as files change), or only refresh on open/explicit action? (Implied "live" by
  the diff-source design, but not stated as a UX expectation above.)
- Branch glyph: any preference/fallback if the terminal/font lacks the glyph, or
  is a plain-text fallback acceptable everywhere?
- Command/keybinding names for the new actions (open files bar, open git status
  bar, toggle follow, etc.) — left to the architecture spec unless there's a
  naming preference now.
