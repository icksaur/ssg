# Project learnings

This file records useful project-level tribal knowledge discovered during task
implementation. Children read it before work so they do not repeat expensive
investigations or violate non-obvious constraints.

Record only durable, cross-task knowledge such as:

- surprising platform, compiler, dependency, or build behavior;
- non-obvious API or lifetime constraints;
- reliable debugging or testing techniques specific to SSG;
- integration seams whose practical behavior was not apparent from the specs.

Do not record task summaries, progress, speculation, obvious documentation, or
temporary workarounds. Each entry names the discovering task, states the
learning, and explains its consequence. Promote a learning into
`copilot-instructions.md` or `doc/spec.md` when it becomes a required invariant,
then remove or shorten the redundant entry here.

## foundation-harness

- CMake component manifests are included in the caller's directory scope, but
  `CMAKE_SOURCE_DIR` always names the outermost project's root. SSG captures
  `SSG_SOURCE_DIR` from `CMAKE_CURRENT_SOURCE_DIR` in its root manifest; every
  component manifest must use that variable so `add_subdirectory` consumers do
  not resolve SSG sources beneath the host project.
- `file(GLOB CONFIGURE_DEPENDS)` reliably discovers newly added component
  manifests during builds with Ninja and Makefile generators. Windows parity
  gates that exercise automatic manifest discovery must use Ninja because
  Visual Studio and Xcode generators do not reliably recheck the glob.
- A nested consumer oracle should call an out-of-line public API, not only
  include a header. Linking an out-of-line symbol proves that the component
  source was discovered and compiled from SSG's source root.

## reference-editor

- Deleting a final line without a trailing newline must also consume its
  preceding newline. Deleting only the final line's byte range leaves an
  unintended empty trailing line.
- Multi-selection transforms that operate on touched lines must partition line
  starts into contiguous runs. Replacing one span from the first touched line
  through the last would modify untouched lines between disjoint selections.

## platform-file-io

- Durable Linux atomic replacement requires flushing the temporary file before
  rename and flushing the parent directory after rename. Flushing only file
  contents does not make the directory-entry replacement crash durable.
- Compensating records need durable in-progress, published, and restored states.
  Restart recovery and cleanup retries must not reapply a completed restoration
  over newer state.
- Rename compensation must persist partial restoration explicitly. Inferring
  progress from path existence can replace an identity-preserving moved inode
  with a copied inode on retry.

## http-cross-platform

- An HTTP upgrade read may contain bytes from the first WebSocket frame after
  the header terminator. The transport must preserve and feed those bytes into
  frame decoding rather than discarding them with the handshake buffer.
- Cross-platform native socket handles require a pointer-width representation
  and an explicit invalid sentinel. Narrowing a Windows `SOCKET` to `int` can
  corrupt valid handles.

## core-websocket-slice

- The HTTP library invokes WebSocket `onOpen`, `onMessage`, and `onClose` on the
  connection thread. Adapter writer threads must stop and join from `onClose`
  before server shutdown completes.
- A feature-state handler captured by `CommandRegistry` needs one outer lock
  spanning dispatch and response-snapshot derivation so another command cannot
  interleave between an accepted mutation and its snapshot.

## input-keymap-contract

- Reserved-chord validation rejects any key sequence beginning with a
  browser-reserved chord. Appending later strokes cannot make an intercepted
  prefix deliverable.

## settings-model

- State import or reload must advance mutation generations so compensating
  actions created before the import cannot become valid again through an ABA
  value cycle.
- A feature command set enumerates every command ID normatively owned by that
  feature even when downstream session assembly binds handlers or presentation.

## required-command-catalog

- Adding normative feature command IDs requires updating both
  `data/required-commands.json` and the independently maintained expected IDs,
  owners, category counts, and total count in `tests/test_required_commands.cpp`.
- The feature-spec union oracle treats backticked dotted tokens as command IDs.
  Write dotted filenames without backticks or use repository-qualified paths
  such as `src/layout.cpp` so they do not become false catalog entries.

## scratch-journal-format

- Journal append assumes its parent directory already exists. The scratch
  session layer owns namespace and directory creation before opening a journal.
- Windows durable journal creation uses write-through file creation followed by
  `FlushFileBuffers`; unlike Linux, Windows exposes no parent-directory fsync
  step for making the new directory entry durable.

## scratch-compaction-quota

- Atomic journal compaction must use the recovery snapshot captured at its
  queued generation. Reading current mutable state can reorder later accepted
  updates across replacement.
- Quota and purge eligibility can use the restored marker to preserve live and
  unrestored sessions while evicting imported remnants oldest-first.

## filesystem-watchers

- Overflow recovery must reconcile native watch registrations as well as cached
  entries. Rebuilding only the snapshot leaves newly created Linux
  subdirectories unwatched.
- `ReadDirectoryChangesW` should keep an overlapped read continuously
  outstanding. Copy completed bytes before rearming so parsing cannot race
  buffer reuse.
- Event coalescing must distinguish same-path identity replacement from
  same-identity recreation and apply subsequent changes to the newest pending
  event.
- Moving a populated directory into a workspace requires an explicit subtree
  scan on Linux and Windows because native APIs may report only the directory
  move.

## diff-model

- Git diff computation consumes caller-supplied index blob content and an opaque
  index identity; it does not read Git objects or invoke a shell.
- Non-Git watcher startup must seed bounded file content before publishing
  events. Accepted events atomically advance the diff model's acknowledged
  baseline.

## search-palette

- Copied background requests need shared cancellation state. Publication must
  independently validate both request generation and source revision to reject
  superseded and stale results.

## treesitter-syntax

- Optional background services receive immutable, revision-tagged requests that
  own their input and share an atomic cancellation flag. Session state changes
  only when the matching non-stale result is accepted.

## lsp-sync-diagnostics

- LSP stream polling must drain every decoded frame after soft per-message
  rejection. Otherwise one stale or bounded diagnostic can discard later
  lifecycle responses or valid diagnostics from the same read.
- Cancelled requests remain in bounded pending-request accounting until their
  responses arrive, preventing cancellation churn from creating unbounded
  tombstone state.

## lsp-language-features

- Feature consumers require the exact synchronized document text and revision
  for UTF-16 position conversion and stale-result rejection; document version
  alone is insufficient.
- Ordinary request errors remain correlated to request IDs without failing the
  shared connection. Cancelled and superseded responses remain observable for
  deterministic rejection.

## unicode-cell-layout

- Byte-pinned upstream Unicode data needs `.gitattributes` entries with `-text`
  so Windows checkouts preserve hashes. Use `-whitespace` when official data
  contains trailing whitespace that must not fail repository diff checks.

## viewport-wrap-scrollbar

- Keep Unicode cell-run computation separate from viewport composition.
  Wrapping, scrolling, scrollbar metrics, and hit targets consume immutable
  `CellRun` values without modifying Unicode layout.
- Define hit targets from source spans so every visible cell of a wide grapheme
  maps to the same stable logical cell and byte range.
- Use 64-bit intermediates for proportional scrollbar arithmetic and signed
  scroll saturation, then clamp through the same viewport construction path to
  avoid overflow and divergent bounds behavior.

## selection-navigation

- APIs accepting raw tab widths must validate the inclusive `[1, 16]` range
  before calling cell layout. Otherwise `compute_cell_run` throws and bypasses
  the feature's typed error contract.

## text-input-commands

- Normalized selections do not guarantee non-overlapping derived edits.
  Multi-selection mutation commands must normalize generated edit ranges before
  constructing a document transaction.

## edit-command-suite

- Display tab width remains distinct from indentation width when validating or
  rematerializing `DocumentPosition` cell coordinates.
- Document-end text transforms account for the phantom empty logical line
  created by a trailing line terminator.

## undo-redo-history

- History integration captures erased UTF-8 bytes from the pre-edit
  `DocumentSnapshot` before calling `Document::apply`; `TransactionResult` does
  not retain removed content needed to construct undo inverses.

## shell-layout

- Priority-based field collapse must retain a prefix of collapse ranks.
  Skipping an oversized field and admitting lower-priority fields inverts the
  contract.
- Reserve footer action rectangles before status-field collapse so accessible
  child nodes cannot overlap.
- Distraction-free rendering suppresses chrome without mutating retained pane
  or panel focus state.

## document-transactions

- Apply validated multi-edit transactions from highest to lowest byte offset to
  preserve pre-transaction coordinates without rebasing later edits.
- Validate replacement UTF-8 and every erase-range endpoint boundary before any
  piece-tree mutation so invalid transactions remain failure-atomic.

## session-state

- Test assertion macros bind operands by const reference. Store members of
  temporary snapshots or optionals in locals before asserting to avoid dangling
  references detected by sanitizers.
- Session command handlers run while the serialization mutex is held. They use
  `CommandContext` and must not re-enter `EditorSession`.

## clipboard-register

- Non-mutating asynchronous clipboard-write responses match request identity,
  not the current document revision. Later edits must not suppress denied or
  unavailable status for an earlier clipboard request.

## browser-input-conformance

- A mandatory browser gate explicitly requires every target engine. Running only
  discovered runtimes can silently pass an incomplete conformance matrix.
- Probe Firefox's BiDi TCP endpoint before opening one WebSocket. Repeated
  pre-start WebSocket attempts can consume the single automation session.

## find-replace

- `DocumentHistory` callers supply the true post-edit `SelectionSet`; redo
  restores that value verbatim rather than deriving it from replacement spans.
- For variable-length regular expressions, apply whole-word filtering before
  choosing the longest candidate end.

## file-commands

- Mixed per-line EOL metadata transforms with each document transaction.
  Reconstructing it from post-edit line indices corrupts untouched lines after
  insertions or deletions.

## edit-history-integration

- Clipboard cut/paste and find/replace already create non-coalescing history
  units internally. The reusable integration seam applies only to text-input
  and edit-command mutations.
- Word deletion uses the same directional history kind as character deletion,
  allowing adjacent same-direction operations to coalesce when history shape
  and timing rules permit.
