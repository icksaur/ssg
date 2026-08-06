# spec-file-management

Status: draft (spec review pending)

## Status

Steps 1-12 are implemented; this spec is complete. Corrections made against it
during implementation, recorded because they change what a reader can assume:

- The path prompt could not be typed into at all. `PromptSurface` had no way to
  change an input's value after `open()`, so `file.open`'s prompt was unusable.
  Step 1 therefore also added `prompt.update_value`.
- `pathPrompt()` accepted `FileCommand::Create`. It no longer does: `file.new`
  takes a label and creates immediately. `workspace.open_directory` is
  path-taking and is now flagged, so the set is
  {OpenDirectory, Open, SaveAs, Rename, NewDirectory}.
- `TabManager::updateDocument` synced mode/dirty/badge but NOT the label or key,
  so a renamed or saved-as document kept its old tab title. It is now a full
  sync from workspace state, which is what makes I4 hold.
- The client focused the editor at startup only when a FILE was opened, so the
  new unnamed buffer never received focus and everything typed was silently
  discarded. The condition is now "startup left something editable".
- The live-diff rule was going to be a `switch` over `FileCommand`. Perturbation
  showed that adding an enumerator compiles CLEANLY, so a switch would let a new
  command silently miss the rule. The classification is a declared descriptor
  field (`mutatesActiveDocumentFile`) with a table-driven test instead.
- `RecoveryManager::renamePath` replaces its destination. A rename that claimed
  the name with a placeholder first made the recovery snapshot record that
  placeholder as the destination's prior state, so a rollback restored an empty
  file where there had been none. `renamePathNoClobber` was added so the
  exclusion and the snapshot stay consistent.
- Deleting could not close the tab via `TabManager::close`: `deleteFile` removes
  the workspace entry first, so the close lifecycle failed on a missing document
  and stranded the tab. `TabManager::dropDocument` exists for the case where
  there is nothing left to flush.
- The archive root is created lazily. Creating it at startup materialised a
  `.ssg/` directory inside every workspace merely for being opened, which
  appeared as a node in the file tree.

## Goals

A user can start `ssg` with no arguments and immediately type into a real
document tab; create further empty buffers; save an unnamed buffer by typing a
name into the input line; copy the current buffer to a new name ("save as");
rename the file behind the current tab; and delete the current file without a
confirmation step but without ever losing the bytes irrecoverably. Every
name-taking operation refuses to clobber an existing file. Tabs always show the
name the bytes actually live under.

## Current behavior (verified in code, not from docs)

Most of the library already exists. This spec closes gaps; it does not build a
subsystem from scratch.

- `FileCommandsCommandSet` (`include/ssg/FileCommands.h`) already declares all
  twelve ids including `file.new`, `file.save_as`, `file.rename`, `file.delete`,
  `file.new_directory`, and they are already in `data/required-commands.json`.
- `Workspace::newDocument` creates an `Untitled` document
  (`src/Workspace.cpp:595`). `Workspace::saveAs`, `renameFile`, `deleteFile`,
  `newDirectory` all exist and are wired in `src/runtime/files.cpp`, which
  already calls `refreshTree()` + `updateTabsFor()` after rename and save-as, so
  tab-title updates are already correct.
- `Workspace::save` on an untitled document already fails with
  "untitled document requires save_as" (`src/Workspace.cpp:677`).
- `file.open` with no payload already opens a `PromptKind::Path` prompt
  (`src/runtime/files.cpp:59-62`); no other file command does.
- `Workspace::deleteFile` routes through `RecoveryManager::deletePath`, which
  snapshots the file into the recovery root before removing it.
- `RecoveryConfig` is `{maximumRecords = 32, maximumBytes = 64 MiB}`
  (`include/ssg/RecoveryManager.h:19`).
- `.gitignore` covers `.ssg/scratch/` only.

## Gaps this spec closes

- G1 No-argument startup opens no tab, so the editor cannot be typed into.
- G2 No keybinding creates a new buffer.
- G3 `file.save_as` / `file.rename` return "requires a path payload" instead of
  opening the path prompt. Only `file.open` prompts.
- G4 A submitted `PromptKind::Path` value cannot be attributed to the command
  that requested it, so a second path-taking command cannot be added.
- G5 No clash check against the disk. `Workspace::saveAs` rejects only
  "destination is already open"; an existing unopened file is silently
  overwritten.
- G6 Delete's only copy of the bytes lives in a bounded undo ring that evicts it
  after 32 later recovery operations or 64 MiB, with no confirmation at delete
  time. This is silent permanent data loss.
- G7 `.ssg/recovery/` and any archive directory are not gitignored, so editor
  internals pollute `git status` in the user's own repository.
- G8 `FileCommandDescriptor` records no machine-readable marker for "this
  command takes a path and should prompt when given none", so nothing can detect
  a path-taking command that was added without prompt wiring.

## Design

### Prompt attribution (G3, G4) -- the load-bearing decision

`PromptSubmission` carries `{kind, values}` only. Save-as, rename, new-file and
open would all be `PromptKind::Path`, so the runtime cannot tell which operation
a submitted path belongs to.

Mechanism chosen: **the prompt carries the command id to dispatch on submit.**
`PromptRequest` and `PromptSubmission` each gain a `std::string commandId`. A
path-taking command invoked with no payload opens a path prompt naming itself;
on submit the runtime re-dispatches that command id with the typed string as its
payload. Submit is one generic branch, not a switch.

Rejected alternative: a `PathPromptKind` enum plus catalog, mirroring
`PickerKind`. It works, but every new path-taking command would require an enum
entry, a catalog row, and a submit-branch edit -- three cascade sites. The
command-id mechanism makes such commands purely additive, which is the stated
requirement. `PickerKind` remains correct for pickers because a picker owns
candidate-list behavior, not just a string.

Scope of the change: `PromptRequest` and `PromptSubmission` are **not** protocol
types. Verified: `src/Protocol.cpp` encodes only `PromptViewState` (and
`PromptKind`, because the view state contains it); there is no codec for the
request or the submission. `commandId` is therefore runtime-domain state that
never crosses the wire, no golden fixture changes, and the feature work is
additive end to end.

Corollary: because the client only ever sees `PromptViewState`, the client needs
no knowledge of which command a prompt belongs to. Attribution is resolved
entirely inside the runtime, which preserves the library-is-contract invariant.

### Path-taking commands are self-declaring (G8)

`FileCommandDescriptor` gains `bool pathPrompt`, marking the commands that take
a workspace-relative path and must open the prompt when dispatched without one:
`file.open`, `file.save_as`, `file.rename`, `file.new_directory`. This is not
bookkeeping -- it is what makes the step-2 oracle real. A table-driven test over
`FileCommandsCommandSet::descriptors()` can then assert that *every* descriptor
with `pathPrompt` set, dispatched with no payload, leaves a prompt open naming
that command. Without the marker the test has nothing to enumerate and would
pass vacuously.

`FileCommandsCommandSet::pathPrompt(FileCommand)` already exists and already
throws for commands that accept no path prompt; the new flag and that method
must agree, which is itself a cheap oracle.

`file.save` is deliberately **not** marked: it takes no path, and it prompts only
in the specific case of an unnamed buffer, by opening the prompt on behalf of
`file.save_as`.

### The unnamed buffer (G1, G2)

`Workspace::newDocument` already produces the right object. Two changes:

- The label for a document created with no suggested label becomes
  `[new buffer]` (today: `Untitled`). This is the tab title only; the
  `JournalDocumentKeyKind::Untitled` key kind is unchanged.
- `apps/ssg_main.cpp` dispatches `file.new` when invoked with no path argument
  and nothing else opened a tab.
- Keymap: `Escape KeyN` -> `file.new`, context `*`, alongside the existing
  `Escape KeyS` -> `file.save`.

`file.save` on an unnamed buffer must open the path prompt rather than fail.
Because save-of-unnamed is really save-as, `file.save` on a document whose key
kind is `Untitled` opens the prompt naming `file.save_as`.

### Clash rule (G5)

One rule, stated once: **a name-taking operation fails if the destination
already exists on disk, except that `file.save` of an already-saved document may
overwrite its own current path.** This covers save-of-new, save-as, rename and
new-directory.

The check must not be `exists()` followed by a write -- that is a
time-of-check/time-of-use race that a concurrent process or a second ssg
instance can lose, and losing it silently destroys the other file. The
destination is created with exclusive-create semantics (`O_EXCL`) and the
"already exists" error comes from the filesystem itself. This is the reason the
IO seam grows a `createFileExclusively` primitive rather than an `exists`
helper; see `spec-file-io-seam.md`.

Rename likewise must not use `std::filesystem::rename`, which silently replaces
an existing destination.

### Delete and the archive (G6, G7)

The recovery store is an **undo ring**, not an archive: bounded to 32 records
and 64 MiB, evicting oldest-first. A delete whose only surviving copy is a
recovery record is therefore a delete with a silent expiry. Since the user
requirement is explicitly "no confirmation", the archive must be the durable
thing that makes the absence of confirmation safe.

Decision: **delete writes to a separate archive, `.ssg/archive/`, distinct from
`.ssg/recovery/`, with its own retention policy.** Delete continues to also go
through `RecoveryManager::deletePath` for immediate undo; the archive is the
long-lived copy. Conflating the two is what causes the data loss, so they stay
separate.

Archive layout: an archived file is stored at
`.ssg/archive/<utc-timestamp>-<counter>/<workspace-relative-path>`, preserving
the relative path under a unique per-deletion directory. Preserving the path
makes a manual restore obvious (`cp` it back) and makes collisions between two
deletions of the same name impossible without a name-mangling scheme.

Archiving is a copy-then-delete, not a rename, because the workspace and
`.ssg/` may sit on different filesystems and because a rename that fails
partway would leave the file neither in place nor archived.

Ordering rule: **the archive copy is durable before the original is removed.**
If the archive write fails, the delete fails and the original is untouched. A
delete never reduces the number of copies below one.

Retention: entries older than 14 days are pruned. Pruning runs at workspace
open, not on a timer, so it is deterministic and testable. Retention is
age-based rather than count-based because the failure this guards against is "I
deleted it last week and now I need it", which a count bound cannot express.

Git pollution: `.gitignore` covers `.ssg/` wholesale rather than individual
subdirectories, so no future subdirectory of the editor's private root can leak
into a user's `git status`. Nothing under `.ssg/` is ever intended to be
committed.

Scope limit: delete operates on the active document's file only. Directory
deletion and recursive delete are out of scope.

Failure policy -- the archive must never make the editor unusable, and must
never let a delete proceed unsafely:

- If `.ssg/archive/` cannot be created or written (read-only workspace, no
  permission, disk full), the **delete fails** and the file is left untouched,
  with the reason surfaced. This follows directly from I1: no archive copy means
  no delete.
- If the workspace root and `.ssg/` are on different filesystems, the
  copy-then-delete design already handles it; this is precisely why the design
  is a copy and not a rename.
- If **pruning** fails (an unreadable or malformed archive entry, a permission
  error), workspace open continues normally and the failure is reported as a
  non-fatal status. Pruning is housekeeping; a corrupt archive entry must never
  prevent the user from opening their workspace. An entry that cannot be
  parsed is left in place rather than removed, since an unparseable entry is
  more likely a bug than garbage, and deleting it would be the second data-loss
  path in a feature whose whole purpose is preventing the first.

### Live diff tabs

`file.save`, `file.save_as`, `file.rename` and `file.delete` are already refused
in live diff tabs. The rule stated once: **a command that mutates the
filesystem on behalf of the active tab is refused when the active tab is a live
diff tab.** By that rule `file.delete` is refused (it is already) and `file.new`
is **allowed** -- it creates a new unnamed buffer in a new tab and does not act
on the active tab at all. `file.new_directory` is likewise allowed; it takes an
explicit path and does not act on the active tab.

## Invariants

- I1 A delete never leaves zero copies of the bytes: the archive copy is
  durable before the original is unlinked.
- I2 A name-taking operation never overwrites an existing file, and the
  non-overwrite is enforced by the filesystem (exclusive create / non-clobbering
  rename), not by a prior existence check.
- I3 Nothing under `.ssg/` is git-visible.
- I4 A tab title always reflects the path the document's bytes are saved under;
  after save-as and rename the title changes in the same dispatch.
- I5 The library owns all of this; the TUI client contributes no file-management
  policy (existing library-is-contract invariant).
- I6 Adding a further path-taking command requires no edit to prompt submission.

## Considerations

- `file.save`, `file.save_as`, `file.rename` are already refused in live diff
  tabs. `file.new` and `file.delete` must be consistent with that.
- `saveAs` from an unnamed buffer transitions the document key from `Untitled`
  to `Saved`. Rename requires an already-`Saved` key and must keep failing on an
  unnamed buffer (rename of something with no name is meaningless; the correct
  command is save-as).
- Save-as leaves the original file on disk and moves the tab to the new path;
  rename moves the file and the tab. Both already call `refreshTree()`.
- The path prompt takes a workspace-relative path.
  `validateWorkspaceRelativePath` already rejects traversal, absolute paths and
  reserved names and must gate every prompt-supplied name.
- Creating a file in a subdirectory that does not exist: fail with a clear
  message rather than silently creating parents, since a typo in a directory
  name would otherwise scatter directories.
- Deleting the active document must close its tab; the document no longer has
  backing bytes.
- Command-catalog cascade: these ids already exist, so
  `data/required-commands.json`, the expected-command lists and the argument
  codec registry need no new entries -- but `file.save_as` / `file.rename`
  becoming payload-optional changes their codec behavior, and any new keybinding
  is a keymap-doc change.

## Risks and Mitigations

- Adding `commandId` to `PromptRequest` was initially assumed to be a wire
  change. It is not: `src/Protocol.cpp` encodes only `PromptViewState`. No
  golden fixtures change and the feature work stays additive. Mitigation: none
  needed, but the assumption is recorded so it is not re-introduced.
- An archive that grows without bound fills the user's disk. Mitigation:
  age-based pruning at workspace open, plus the archive is per-workspace so it
  is bounded by what the user actually deleted there.
- A user may not realize deletes are archived and may believe a secret was
  destroyed. Mitigation: the delete status message names the archive location.
- Exclusive-create changes the failure mode of existing save paths. Mitigation:
  save-over-self must keep working; that is its own oracle.

## Acceptance (Definition of Done)

- Observable: `ssg` with no arguments opens a `[new buffer]` tab that accepts
  typing; `Escape S` opens the path prompt (the `PromptKind::Path` prompt row
  reserved by `promptRowCount`, not the header input line) to type a name, saves,
  and the tab title becomes that name; save-as to an existing name is refused
  with a visible message; delete removes the file, closes the tab, and the bytes
  are present under `.ssg/archive/`. Requires user signoff (visible UI).
- Budgets: no regression in the fast gate's runtime; archive pruning at
  workspace open must not add measurable startup cost for an empty archive.
- Gates: `bash scripts/check.sh` and `scripts/check.sh push` both green, zero
  warnings.
- Oracles: as listed per plan step below.

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Carry `commandId` on `PromptRequest`/`PromptSubmission`; submit re-dispatches it. **This spec owns this step**; `spec-file-io-seam.md` depends on it and must not re-implement it | `include/ssg/PromptSurface.h`, `src/PromptSurface.cpp`, runtime submit path | behavior: two different path-taking commands prompted in turn each re-dispatch themselves, not each other (a submit path ignoring `commandId` sends both to one command and fails) | I6 |
| 2 | Add `pathPrompt` to `FileCommandDescriptor`, set on the path-taking commands | `include/ssg/FileCommands.h`, `src/FileCommands.cpp` | invariant: for every descriptor, `pathPrompt` set iff `pathPrompt(command)` does not throw. Perturbation: flip one flag, test fails | I6 |
| 3 | Open the path prompt from every `pathPrompt` command dispatched with no payload, and from `file.save` on an unnamed buffer | `src/runtime/files.cpp` | table-driven over `descriptors()`: every `pathPrompt` descriptor with no payload leaves a prompt open naming that command. Perturbation: add a `pathPrompt` descriptor without wiring, test fails | I6 |
| 4 | Add `createFileExclusively` + non-clobbering rename to the IO seam and route save/save-as/rename/new-directory through them | see `spec-file-io-seam.md` | ref-impl: create a destination file out-of-band, then attempt each name-taking op; every one fails and the pre-existing bytes are unchanged | I2 |
| 5 | Allow `file.save` to overwrite the document's own current path | `src/Workspace.cpp` | hand case: open, edit, save twice; second save succeeds and content matches | I2 |
| 6 | Label unnamed documents `[new buffer]`; dispatch `file.new` on no-argument startup; bind `Escape KeyN` | `src/Workspace.cpp`, `apps/ssg_main.cpp`, `src/EditorRuntime.cpp` | behavior: a runtime started with no path has exactly one tab titled `[new buffer]` and accepts an insert | I4 |
| 7 | Add the archive: copy-then-delete into `.ssg/archive/<ts>-<n>/<relpath>`, durable before unlink | new `src/FileArchive.cpp` + header, `src/Workspace.cpp` | invariant: archived bytes equal the pre-delete bytes; fault-injected archive-write failure leaves the original in place and the command failed | I1 |
| 8 | Prune entries older than 14 days at workspace open; prune failure non-fatal, unparseable entries retained | `src/FileArchive.cpp`, `src/Workspace.cpp` | hand case: entries backdated either side of the boundary plus one unreadable entry; only the older parseable one is pruned and workspace open still succeeds | I1 |
| 9 | Apply the live-diff rule: refuse `file.delete` (already), allow `file.new` and `file.new_directory` | `src/runtime/files.cpp` | table-driven: with a live diff tab active, each file command's accept/refuse matches the stated rule | - |
| 10 | Close the tab of a deleted document | `src/runtime/files.cpp` | behavior: delete the active document, no tab references it | I4 |
| 11 | Gitignore `.ssg/` wholesale | `.gitignore` | invariant: with a populated `.ssg/`, `git status --porcelain` mentions nothing under `.ssg/` | I3 |
| 12 | Update docs: keymap, `doc/config.md` if configurable, the file-commands feature doc, `spec-ux.md` for the prompt row's usage | `doc/` | existing doc-consistency tests | - |

## Rationale

The archive/recovery split is the one place this spec spends complexity
deliberately. It would be cheaper to declare the existing recovery snapshot
"the archive" and ship. That is wrong: the recovery store's contract is a
bounded undo ring, and its eviction is correct behavior for undo. Reusing it
would mean a delete's durability silently depends on how many unrelated saves
happened afterwards -- an interaction no user could predict and no error message
would surface. Two stores with two honest retention policies is simpler to
reason about than one store with two incompatible contracts.
