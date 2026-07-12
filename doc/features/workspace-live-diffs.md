# spec-workspace-live-diffs

## Goals

Watch the CWD on Linux and Windows, expose filesystem/Git trees and external-modification status, compute live diffs, and follow external edits consistently across clients.

## Design

Watcher normalization, seeded baselines, diff tabs, dirty conflicts, shared follow state, per-client scrolling, pause/resume, and target validity follow `doc/spec.md`. Diff correctness is defined by patch reconstruction and normalized changed-line sets, not byte-identical Git hunk boundaries.

### Diff model contract

`DiffModel` is a content consumer and never invokes Git or a shell. The
repository service supplies a stable file ID, workspace-relative current and
prior paths, the index blob content (absent for an untracked file), current
working-tree content (absent for a deletion), and an opaque index identity.
Resubmitting a file after the index or branch identity changes recomputes its
view against the new index blob. The identity tags the published result so a
session can reject work computed for an obsolete index.

Outside a Git worktree, watcher startup reads eligible bounded file content and
passes it to `seed_non_git` before events are published. The diff model owns
that acknowledged content baseline. A content-bearing normalized event carries
the stable file ID, revision, paths, kind, and post-event content; delete
events omit post-event content. An accepted event computes against the
acknowledged content and then advances it atomically. Rename and delete views
retain the stable ID, prior path, and prior content needed to describe the
event. The watcher remains responsible only for event normalization and
metadata.

Diff lines retain their line terminators, so replacing emitted hunk baseline
lines with target lines reconstructs target bytes exactly, including the final
newline. A normalized changed-line set is derived independently from every
hunk: pair removed and added lines by ordinal as `modified`, then emit any
unpaired old lines as `removed` and any unpaired new lines as `added`.
Coordinates are zero-based; modified entries carry both old and new
coordinates, removed entries only the old coordinate, and added entries only
the new coordinate. Entries are ordered by hunk, paired modifications,
remaining removals, then remaining additions. This normalization, rather than
the implementation's hunk boundaries, is the fixture oracle. The canonical
line alignment is a longest common subsequence; when two alignments have equal
remaining length, the old line is consumed first.

Every mutation carries a strictly increasing source `Revision`. Stale input,
unknown or cross-source identities, missing Git index identity, malformed event
content, and configured diff-work limit exhaustion return a typed error and
leave the model unchanged. A
successful publication is all-or-nothing; invariant I5 applies to that
atomicity rather than editor undo.

The immutable `DiffCommandSet` contains exactly, in order,
`diff.next_hunk`, `diff.previous_hunk`, and `diff.open_file`. Navigation is
pure model navigation: next selects the first hunk whose target start is
strictly after the supplied target line and wraps to the first; previous
selects the last hunk strictly before it and wraps to the last. With no current
line they select first and last respectively; with no hunks they return no
target. `diff.open_file` resolves the stable file ID and retained
workspace-relative path (the prior path for a deletion) for later session
assembly; it does not open a tab in this component. These helpers neither
change revision nor own presentation state.

Normative commands owned by this feature:

- `tree.toggle_expanded`, `tree.invoke_node_command`
- `follow_edits.resume`, `follow_edits.pause`
- `diff.next_hunk`, `diff.previous_hunk`, `diff.open_file`

Plan 1 owns only generic tree data and the two `tree.*` commands. It performs
explicit filesystem scans and consumes caller-supplied Git and symbol records;
filesystem watching, Git repository access, and Tree-sitter parsing belong to
later tasks. `tree.invoke_node_command` selects a provider-supplied immutable
command descriptor by provider, node, and command ID; session assembly later
binds those descriptors to handlers.

## Invariants

I3, I5, I9, I10, I14, I16, I19, I21 from `doc/spec.md`.

## Considerations

- inotify and `ReadDirectoryChangesW` normalize into one event vocabulary.
- Overflow triggers bounded rescan.
- SSG-originated saves do not duplicate events.
- Dirty buffers remain unchanged while disk diffs and footer actions update.

### Watcher contract

`FilesystemWatcher` is an injected, caller-owned interface. A platform watcher
owns its native handles but no thread; the session host owns the polling thread
and posts returned batches to the session command executor. A poll has a finite
caller-supplied timeout, so destruction and shutdown never depend on an
uninterruptible background callback. Callbacks from native watcher threads are
not part of the interface.

Normalized events contain a kind (`create`, `modify`, `rename`, `delete`, or
`overflow`), a workspace-relative path, the prior workspace-relative path for a
rename, an optional stable `FileIdentity`, monotonically increasing watcher
sequence, observed size and modification time when available, and an origin
(`external` or `ssg_save`). Rename/delete identity and metadata come from the
watcher's seeded cache because the path may no longer resolve.

The watcher layer owns only mechanical save correlation. After a successful
save, the file lifecycle layer registers the resulting path, identity, size,
and modification time. The next matching normalized event is marked
`ssg_save`; it is not discarded. The later external-modification flow consumes
that origin to advance baselines without publishing duplicate status or follow
transitions. Expectations and event queues are bounded.

Debounce duration, maximum queued events, maximum pending rename pairs,
maximum save expectations, and maximum rescan entries are construction
configuration. An incomplete bounded rescan suppresses incrementals and retries
at the configured finite backoff without repeatedly publishing overflow. Tests
inject time rather than sleeping. Within one debounce
window for the same path, with stable identity retained to distinguish path
reuse,
normalization applies these deterministic rules:

- create + modify becomes create;
- create + delete disappears;
- modify + modify becomes the newest modify;
- modify + delete becomes delete;
- delete + create becomes modify when stable identity is unchanged, otherwise
  the two events remain ordered;
- a rename retains its original prior path while later modify events update its
  final path and metadata.

Native rename halves are paired by the native cookie/order token. A paired move
within the CWD is one rename; an unmatched move-from becomes delete after the
debounce window; an unmatched move-to becomes create. Moves out of and into the
CWD therefore normalize to delete and create respectively.

The initial cache and rescans recursively enumerate ordinary entries beneath
the canonical CWD without following directory symlinks. New directories are
watched and scanned before their create event is released, closing the common
inotify subtree race. Native overflow, rename-pair exhaustion, or normalized
queue exhaustion collapses pending incremental output to one overflow event and
requests a rescan. A complete bounded rescan emits synthetic normalized changes
after the overflow and replaces the cache. If the entry bound is reached, no
partial synthetic changes are emitted and overflow remains the only result, so
incremental correctness is never claimed from a partial snapshot.

## Risks and Mitigations

- Event storms/races: debounce by stable identity and discard stale revision-tagged work.
- Follow fighting users: user navigation pauses globally before movement.

### Tree provider contract

Filesystem node identity is the normalized workspace-relative path, Git node
identity is the normalized workspace-relative path plus the provider namespace,
and symbol identity is the caller-supplied stable symbol key plus the provider
namespace. Labels, status, ordering, and source locations are presentation data
and never participate in identity. Refreshing a provider retains expansion for
IDs that still exist and removes state for IDs that disappeared.

Filesystem snapshots are rooted at a canonical CWD, do not follow directory
symlinks, and sort children by normalized workspace-relative path. Git and
symbol snapshots sort caller-supplied records by stable key. All three produce
the same generic `TreeProviderSnapshot`; providers do not perform watching or
own repository/parser services.

This feature exports immutable `TreeCommandSet`, `TreeViewState`, and
`TreeDelta` values. Delta derivation is pure and revision-based. A configured
operation limit bounds every incremental delta; an over-limit change emits a
typed snapshot-required result with no partial operations. Replay rejects a
stale base revision and otherwise either reconstructs the independently
derived target view exactly or reports that a replacement snapshot is needed.

## Acceptance (Definition of Done)

- Observable: external edits update tabs/diffs and follow state identically on Linux and Windows and across clients.
- Budgets: watcher queues and rescans remain bounded.
- Gates: watcher/diff/follow tests are green on Linux and Windows.
- Oracles: temporary-directory event scripts, reconstructing diff properties, hand-authored changed-line cases, and independent follow transition tables.

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Implement generic tree providers and owned tree commands, without watching | `include/ssg/tree.h`, `src/tree.cpp`, `tests/test_tree.cpp`, `cmake/components/tree-providers.cmake` | temporary-directory ground truth and hand-authored Git/symbol snapshots; bounded delta replay equals an independently derived full view | I9, I10, I16 |
| 2 | Implement Linux/Windows watcher normalization | `include/ssg/watcher.h`, `src/watcher.cpp`, `src/platform/*watcher.cpp`, `tests/test_watcher.cpp` | identical normalized event scripts | I10, I21 |
| 3 | Implement Git/non-Git diff models | `include/ssg/diff.h`, `src/diff.cpp`, `tests/fixtures/diff/`, `tests/test_diff.cpp`, `cmake/components/diff-model.cmake` | patch reconstruction, independent normalized changed-line fixtures, revision failure atomicity, and exact command/navigation cases | I3, I5 |
| 4 | Implement follow/pause/resume | `include/ssg/follow_edits.h`, `src/follow_edits.cpp`, `tests/test_follow_edits.cpp` | independent transition table | I14, I19 |

## Rationale (optional, skippable)

Watcher, diff, and follow behavior form one ordered external-change pipeline.
