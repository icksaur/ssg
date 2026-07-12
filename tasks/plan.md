# SSG task plan

## Task contract

Each slug in this directory is one independently reviewable unit. Its branch is
`<slug>-task`; its recommended worktree is `../ssg-<slug>`.

Before worktrees can be created, commit every reviewed tracked/untracked
planning artifact on `master` and verify it with `git ls-files`. Create a task
worktree only from the recorded dependency-complete SHA:

```sh
git worktree add -b <slug>-task ../ssg-<slug> <dependency-complete-sha>
```

Every task follows this mandatory workflow:

1. Read `copilot-instructions.md`, `doc/spec.md`, `doc/learnings.md`, the
   referenced feature spec, and the task file.
2. Use the `task` tool with Claude Opus 4.8 for a task/spec sanity review.
3. Fold warranted findings into the task's feature spec before implementation.
4. Write the stated oracle test first and confirm it fails for the missing
   behavior.
5. Implement only the task scope.
6. Run the task tests, project build/tests, and relevant sanitizer or platform
   gates.
7. Use the `task` tool with Claude Opus 4.8 for the final diff review.
8. Fold warranted findings and rerun gates.
9. Commit the reviewed task on `<slug>-task`; merge it into the integration
   branch before starting dependent tasks.
10. After merge, answer the parent's required tribal-knowledge prompt; the
    parent triages useful project-level findings into `doc/learnings.md`.

One herd child owns one worktree and one task. Children never modify `master`.
Parallel groups below are file-disjoint by design; do not start a later group
until all named dependencies are merged. Default to at most four active
implementers and serialize heavy sanitizer/browser gates when needed.

`http-cross-platform` uses paired isolated SSG/HTTP worktrees as defined in
`process.md`. The original `../http` checkout remains pinned for readers. The
parent integrates the HTTP branch first, records the merged SHA in
`tasks/dependencies/http.commit`, then integrates the SSG branch.

The `foundation-harness` task establishes component CMake manifests so later
parallel branches add unique manifest files instead of editing shared source or
test lists.

Every feature task that owns normative commands exports one immutable
`<Feature>CommandSet`; every task with observable state exports typed
`<Feature>ViewState` and `<Feature>Delta` derivation functions. Feature tasks do
not edit session, aggregate snapshot, or protocol codec files. The
`core-websocket-slice` owns the serialized minimal seam; later,
`editor-session-assembly` is the sole feature-aggregation owner and
`protocol-codec` is the sole complete-codec extension owner.

## Execution order

### Gate 0 — serial

| Task | Depends |
|---|---|
| `foundation-harness` | planning baseline commit |

### Wave 1 — parallel

| Task | Depends |
|---|---|
| `reference-editor` | `foundation-harness` |
| `required-command-catalog` | `foundation-harness` |
| `settings-model` | `foundation-harness` |
| `unicode-cell-layout` | `foundation-harness` |
| `theme-model` | `foundation-harness` |
| `platform-file-io` | `foundation-harness` |
| `tree-providers` | `foundation-harness` |
| `http-cross-platform` | `foundation-harness`; exclusive `../http` ownership |

### Wave 2 — parallel

| Task | Depends |
|---|---|
| `piece-tree` | `reference-editor` |
| `viewport-wrap-scrollbar` | `unicode-cell-layout` |
| `shell-layout` | `unicode-cell-layout`, `theme-model` |
| `encoding-eol` | `platform-file-io` |
| `scratch-journal-format` | `platform-file-io` |
| `scratch-session-locking` | `platform-file-io` |
| `filesystem-watchers` | `platform-file-io` |

### Wave 3 — parallel

| Task | Depends |
|---|---|
| `document-transactions` | `piece-tree`, `reference-editor` |
| `prompt-status-surface` | `shell-layout` |
| `scratch-compaction-quota` | `scratch-journal-format`, `scratch-session-locking` |
| `diff-model` | `filesystem-watchers` |

### Wave 4 — parallel

| Task | Depends |
|---|---|
| `selection-navigation` | `document-transactions`, `viewport-wrap-scrollbar` |
| `session-state` | `document-transactions` |
| `recovery-actions` | `scratch-compaction-quota` |
| `search-palette` | `document-transactions`, `prompt-status-surface` |
| `treesitter-syntax` | `document-transactions` |

### Gate 5 — serial core input and early vertical slice

| Task | Depends |
|---|---|
| `text-input-commands` | `selection-navigation`, `settings-model` |
| `core-websocket-slice` | `session-state`, `text-input-commands`, `http-cross-platform` |

This gate proves `text.insert` through direct API and loopback WebSocket reaches
the same document snapshot before broader feature fan-out.

### Wave 6 — parallel

| Task | Depends |
|---|---|
| `edit-command-suite` | `text-input-commands`, `core-websocket-slice` |
| `undo-redo-history` | `selection-navigation`, `document-transactions`, `core-websocket-slice` |
| `file-commands` | `document-transactions`, `encoding-eol`, `recovery-actions`, `prompt-status-surface`, `core-websocket-slice` |
| `lua-command-host` | `session-state`, `required-command-catalog`, `core-websocket-slice` |
| `lsp-sync-diagnostics` | `session-state`, `document-transactions`, `core-websocket-slice` |
| `input-keymap-contract` | `required-command-catalog`, `prompt-status-surface`, `settings-model`, `core-websocket-slice` |

### Wave 7 — parallel

| Task | Depends |
|---|---|
| `clipboard-register` | `edit-command-suite`, `undo-redo-history` |
| `find-replace` | `search-palette`, `edit-command-suite`, `undo-redo-history` |
| `tab-management` | `file-commands`, `session-state` |
| `lsp-language-features` | `lsp-sync-diagnostics` |
| `lsp-workspace-edits` | `lsp-sync-diagnostics`, `recovery-actions` |
| `browser-input-conformance` | `input-keymap-contract` |
| `external-modification-flow` | `filesystem-watchers`, `file-commands`, `recovery-actions`, `prompt-status-surface`, `diff-model` |

### Wave 8 — parallel

| Task | Depends |
|---|---|
| `edit-history-integration` | `edit-command-suite`, `undo-redo-history`, `clipboard-register`, `find-replace` |
| `follow-edits` | `diff-model`, `session-state`, `prompt-status-surface`, `external-modification-flow` |

### Gate 9 — session assembly

| Task | Depends |
|---|---|
| `editor-session-assembly` | `edit-history-integration`, `follow-edits`, `tab-management`, `lsp-language-features`, `lsp-workspace-edits`, `browser-input-conformance`, `lua-command-host`, `treesitter-syntax`, `tree-providers`, `theme-model`, `viewport-wrap-scrollbar`, `settings-model`, `external-modification-flow` |

This task is the sole owner of command-set registration and complete
snapshot/delta aggregation.

### Gate 10 — complete protocol/server

| Task | Depends |
|---|---|
| `protocol-codec` | `editor-session-assembly`, `core-websocket-slice` |
| `websocket-server` | `protocol-codec`, `http-cross-platform`, `clipboard-register` |

### Wave 11 — parallel clients

| Task | Depends |
|---|---|
| `tui-client` | `editor-session-assembly`, `websocket-server`, `browser-input-conformance` |
| `browser-client` | `editor-session-assembly`, `websocket-server`, `browser-input-conformance` |

### Gate 12 — serial

| Task | Depends |
|---|---|
| `end-to-end-parity` | `tui-client`, `browser-client` |
| `performance-ci` | `end-to-end-parity` |

## Merge rules

- Merge only reviewed, green task commits.
- If a required rebase changes the reviewed tree, the same child reruns gates
  and obtains a new Opus 4.8 diff review before returning a replacement SHA.
- Never merge a task whose feature spec changed without the sanity-review
  findings folded into that same branch.
- Resolve integration conflicts in a dedicated follow-up task, not by silently
  changing reviewed behavior during merge.
- Disown a herd child after its branch is merged; resume it only for review
  findings within the same task scope or the required post-merge learnings
  prompt.
- Remove merged worktrees and branches; do not run git prune/gc while children
  own worktrees.
