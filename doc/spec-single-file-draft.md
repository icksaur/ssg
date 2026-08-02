# spec-single-file-draft

Status: DRAFT (spec only — collaborate on open questions before implementing)

Scope: the **single-file** half of the autosave/reopen feature. This is the
foundation; multi-tab / directory session restore (Notepad++/Kate "reopen all my
tabs") is a **separate, later spec** that builds on this one. See
`doc/spec-autosave-tabs.md` for that larger design and why it is deferred (it is
unreliable in a terminal where CWD and launch arguments vary).

## Goal

One file, one promise: **you never lose unsaved edits, and reopening a file is
never a blind, blocking choice.**

Concretely, for a single saved file:

1. Edit a file, exit SSG without saving (clean quit, tab close, `kill -9`, or
   crash).
2. Reopen the *same file* later — by any path that opens the same file (see the
   recovery-scope note below), from any working directory.
3. Your unsaved edits come back as a dirty buffer. Disk is untouched.
4. If the file on disk changed **externally** since your edits branched off it,
   SSG says so **non-modally** and offers to diff draft-vs-disk — it never
   silently discards either side, and never blocks you to force a choice.

**Recovery point (RPO) — what "never lose" honestly means.** Drafts flush on tab
close and on **clean** process exit with **zero loss**. On an **uncatchable**
`kill -9` or a crash, edits are preserved only up to the **last debounced flush**:
the flush interval (default 10s) is the maximum window of most-recent edits that
can be lost, because `SIGKILL` cannot run an exit hook. To bound this window
tightly, the library performs an **eager first flush** on the first edit after a
document becomes dirty (so a brand-new edit is durable within one tick, not up to
a full interval), then debounces subsequent edits at the interval. The promise is
therefore: **clean quit/close = no loss; kill/crash = at most one flush-interval
of the newest edits.** Acceptance tests that assert kill-9 recovery must first
wait for a heartbeat flush.

**Recovery scope.** A draft is keyed within its workspace by the file's
workspace-relative path, under a store directory named by `sha256(canonical
workspace root)` — together an absolute-path identity. So it is recovered by *any*
path that opens *that same file* under the *same workspace root* (a different
relative path, or a symlink, resolving to it). Because SSG's `Workspace` rejects
paths outside the launched workspace root (`src/Workspace.cpp:328`), a file can
only be opened — and thus have a draft, and be recovered — when it is within the
workspace SSG was launched in. Recovering the same file opened under a *different*
workspace root, or from an out-of-workspace absolute path, is **out of scope**
(see Decision 1). Recovery across restarts requires the stable root (Decision 1),
not per-PID temp.

This upholds SSG's two standing principles: **no blocking actions** and
**everything recoverable**.

## Non-goals (this spec)

- Reopening a *set* of tabs / restoring tab order / active tab — the directory
  session manifest. Deferred to `spec-autosave-tabs.md`.
- Restoring **clean** files (no unsaved edits). Single-file recovery is about
  *unsaved edits*; a file with nothing unsaved has nothing to recover. (The
  GUI-editor "reopen my clean tabs too" behavior belongs to the deferred
  session-restore spec, not here.)
- Untitled (never-named) buffers. They have no file identity to reopen *by*;
  their recovery rides the deferred session-restore spec. (Their draft records
  already exist; we simply do not surface them on file-open here.)
- Cross-instance coordination (two SSGs editing one file). Out of scope; last
  writer to scratch wins, as today.

## What already exists (build on it)

- **`ScratchStore`** persists unsaved content + dirty flag to a crash-safe
  append-only journal. Today it is keyed to the canonical workspace root
  (`ScratchStore::create(scratchRoot, canonicalWorkspace)`,
  `include/ssg/ScratchStore.h:56`); Decision 1 re-keys drafts by canonical
  absolute path instead. `updateDocument(JournalDocument)` upserts,
  `removeDocument(key)` erases, `recovery()` returns all drafts. Tail-discard
  replay already tolerates a torn write.
- **`JournalDocument`** (`include/ssg/ScratchJournal.h:65`) = `{key, mode, dirty,
  utf8Content}`, keyed by `JournalDocumentKey` (Saved workspace-relative path OR
  Untitled UUID). **It stores NO disk baseline** (no mtime/size/hash) — the gap
  this spec must close.
- **The diff engine is source-agnostic**: `computeDiff(baseline, target,
  config)` (`src/DiffModel.cpp:377`) diffs any two text buffers; `seedNonGit()` /
  `applyNonGitEvent()` (`include/ssg/DiffModel.h:164`) already build a non-git
  live-diff tab. Only the git *scan source* is git-specific — the engine is not.
- **`ExternalModificationFlow`** already models the exact conflict we hit on
  reopen: it holds `baselineContent`, compares it to `diskContent`, and exposes
  actions `{Reload, KeepBuffer, OpenDiff}` (`processEvent`, `reload`,
  `keepBuffer`, `openDiff`, `ExternalModificationFlow.h:107-160`). Today it fires
  from a live filesystem-watcher event; reopen must drive the same machinery from
  a *stored* baseline instead of a live one.
- **`TabRecoveryBadge`** (`include/ssg/TabManager.h:55`) already surfaces
  per-tab durability (Durable/Pending/Failed) — a precedent for a small per-tab
  state, though the conflict notice below needs a richer, clickable affordance.
- **Drafts currently flush only on tab close** (`src/EditorRuntime.cpp:816`), so
  a kill mid-edit leaves no draft at all. Continuous (debounced) flush of *open*
  documents is a prerequisite (Phase 1 below); it is also Phase 1 of the deferred
  spec, and is shared.

## Design

### The draft lifecycle for one file

- **Open** `foo.txt`: read disk once (as today). **Also record a disk baseline**
  — the state the user's edits will branch from: `{mtime, size, contentHash}`.
- **Edit**: the buffer becomes dirty. A debounced flush writes the draft
  (`JournalDocument{dirty=true, utf8Content}`) **plus the recorded baseline** to
  the journal while the file is open (Phase 1). The baseline is the disk state at
  open/last-save, NOT the draft content.
- **Save**: disk is written; the draft is removed (`removeDocument`, as today);
  the baseline is refreshed to the just-written state.
- **Exit without saving**: the draft + baseline survive in the store. A
  best-effort final flush on process exit captures any edits newer than the last
  interval tick. Disk is untouched, so on-disk `foo.txt` is still exactly the
  baseline — *unless something external changes it.*
- **Reopen** `foo.txt`: classify (below), then load.

### The baseline identity — and your mtime question, answered

Store, alongside each dirty saved-file draft, the **baseline the edits branched
from**: `mtime`, `size`, and a `contentHash` of the disk file at open/last-save.

A draft always implies a document we are about to load, and classification needs
the current disk content anyway (for the converged-check and any diff), so on
reopen we **always read the current disk file and hash it** — the hash is the
authority. `(mtime, size)` are stored only as a **cheap negative pre-check**:

- **`(mtime, size)` differ from the baseline** ⇒ definitely suspect a change;
  compute the disk hash and compare.
- **`(mtime, size)` match** ⇒ *probably* unchanged, but this is NOT trusted on
  its own: a same-size rewrite with a preserved or coarse-resolution mtime would
  slip through. We still hash the disk content to be sure. (The stored
  `(mtime,size)` can gate an optimization *only* if a future measurement shows
  hashing on open is too costly for large files — and even then only by
  weakening the acceptance, not the invariant. v1 always hashes.)

Decision by hash:

- **Disk hash == baseline hash** ⇒ disk is unchanged since the edits branched
  (mtime touched-but-identical, `touch`, no-op `git checkout`, a copy) — no
  conflict, offer the draft.
- **Disk hash != baseline hash** ⇒ the file was saved by something other than SSG
  since the edits branched. **Conflict** → the non-modal notice below.

So: **yes, mtime (with size) is a cheap hint that something *might* have changed,
but content — the hash — is the authority.** Storing both lets us phrase a fast
"nothing to see" negative, while never *concluding* "unchanged" without the hash.

### Reopen classification (single file)

On `file.open` of a saved path that has a **dirty** draft in the store, read and
hash the current disk content once, then decide **in this order** (convergence is
checked *first* so an undone-edits draft is never briefly loaded as dirty):

| Check (in order) | Disk state | Result |
|---|---|---|
| 1. draft content == current disk | present | Edits converged with disk (or were undone). Drop the draft silently, open clean — no dirty, no notice. |
| 2. disk hash == baseline hash | present | Disk unchanged since the edits branched. Load draft as dirty buffer; subtle "restored draft" badge; **no** conflict notice. |
| 3. disk hash != baseline hash | present | External change. Load draft as dirty buffer **and** show the conflict notice (below). |
| 4. — | file deleted / unreadable | Load draft as an orphaned dirty buffer; notice says "file missing on disk"; diff shows draft-vs-empty. |

A file with **no draft**, or only a **clean** draft, opens normally — this
feature is invisible unless there are real unsaved edits. (The hash comparison
loads disk content into memory once at open; this file is being opened regardless,
so the cost is the read we would do anyway plus one fast hash.)

### The conflict notice (non-modal, visible, reversible)

The design lesson from vim's swap prompt: it is **modal** (blocks you), **blind**
(never shows the difference), and offers **too many cryptic choices**. SSG
inverts all three.

- **Non-blocking.** The file opens showing the **draft** (your unsaved edits) as
  a normal dirty buffer. Nothing is forced.
- **A reserved yellow notice row** sits in the tab's chrome **above** the document
  content — a dedicated reserved row like the prompt/status row, NOT document
  content row 0. This is deliberate: stealing the first *document* row would
  shift line 1, the caret, line numbers, scroll math, and hit-testing by one.
  The notice is a virtual chrome row the viewport reserves (the layout already
  reserves such rows for the prompt); the document's own coordinate space is
  untouched. Text: *"Unsaved draft — file changed on disk externally. [Diff] ·
  [Use disk] · [Dismiss]"*, yellow background, one row.
- **Clickable.** The `[Diff]` / `[Use disk]` / `[Dismiss]` regions are
  hit-tested (like a `HeaderField`); clicking dispatches the corresponding
  command. Keyboard bindings dispatch the same commands.
- **Visible before deciding.** `[Diff]` opens a draft-vs-disk diff tab (existing
  engine) so you *see* what differs — you never choose blind.
- **Reversible / non-destructive.** `[Use disk]` **archives** the draft (does not
  delete it) and loads disk content; `[Dismiss]` leaves you on the draft with the
  draft parked. No path silently destroys the other side.

Only two decisions, both reversible, and a way to see the difference first.
Because the notice is a reserved chrome row, its presence must not perturb the
document viewport model — see the viewport/hit-test/selection oracles in
Acceptance.

### New commands

- **`draft.diff`** — open a live diff tab of **draft (current buffer) vs current
  disk content**, built from the existing engine: read disk, `computeDiff(disk,
  draft, config)` (or `seedNonGit` with disk as baseline, draft as target). No
  git involved. This is also what the notice's `[Diff]` dispatches.
- **`draft.discard`** (a.k.a. "use disk") — remove the scratch draft
  (`removeDocument`), reload the buffer from disk (`file.reload` machinery), clear
  dirty, and clear the notice. The removed draft is **archived** first (moved to
  the central archive root beside the draft store — see Decision 1, NOT a
  workspace `.ssg/archive`) so a mis-click is recoverable, satisfying "everything
  recoverable."

`draft.diff` is additive (new). `draft.discard` composes `removeDocument` +
existing reload. Both register through the normal command builder pattern
(`src/runtime/files.cpp` spec/handler).

### Ownership (library owns policy; app owns OS conventions — I25)

- **Library** owns: the baseline capture, the debounce/flush decision, the
  reopen classification, the conflict state, the diff assembly, and all scratch
  reads/writes.
- **App** owns: the wall-clock heartbeat tick that drives the debounce, and the
  resolution of the scratch/recovery/archive **root paths** (see the storage
  open question). The app never decides what/when to flush.

## Invariants

- **Non-destructive:** reopen and recovery write only the scratch/archive area
  and read only disk; a user's file is written solely by an explicit save.
- **No blind blocking choice:** a conflict never blocks input and is never
  resolved by SSG silently discarding a side; the user can always diff first and
  every resolution is reversible (draft archived, not deleted).
- **Authority is content, not clock:** conflict is decided by `contentHash`;
  mtime/size are only a fast-path hint. A touched-but-identical file is not a
  conflict.
- **Single-threaded session mutation:** flush, reopen, and discard run on the
  command thread; background durability stays behind the existing fsync thread.

## Corner cases

- **Touched but identical** (`touch foo.txt`): mtime differs, hash equal → no
  conflict.
- **Converged draft** (edits equal current disk, or edits then undone): draft ==
  disk → drop draft, open clean.
- **File deleted on disk**: orphaned dirty buffer; diff is draft-vs-empty; saving
  recreates the file.
- **File replaced by a directory / unreadable / permission-denied on read**:
  cannot read disk → keep the draft, notice reports "disk unreadable," `[Use
  disk]` disabled until readable.
- **Path aliasing** (`./foo.txt` vs a symlink, under the same workspace root):
  the draft key is `sha256(workspace root)` + the workspace-relative path, so one
  file has one draft regardless of the relative path or symlink used to open it
  (Decision 1). Out-of-workspace absolute paths cannot be opened today (Workspace
  contract) and are out of scope (see Recovery scope).
- **Line-ending / trailing-newline differences**: `contentHash` is over raw
  bytes, so a CRLF↔LF or final-newline change *is* a real disk change (correct —
  it would be overwritten on save).
- **Large file**: baseline hash + draft storage are bounded by the existing
  `ScratchStoreConfig` quota (256 MiB / 30 days); a file over the draft cap gets
  no draft (and thus no crash-safety) — must be reported, not silent.
- **Binary / non-UTF-8 file**: scratch stores `utf8Content`; SSG edits text, so
  this is the same limitation as today, out of scope.
- **Draft from a prior SSG version** (baseline field absent): a draft with no
  stored baseline is treated as "unknown baseline" → always show the conflict
  notice (safe default: never assume unchanged). Forward-compatible via the
  append-only journal's tolerant replay.
- **Clock skew / network filesystem** (unreliable mtime): hash fallback makes the
  decision correct regardless; only the fast path is skipped.
- **Save-while-notice-showing**: saving resolves the conflict (draft removed,
  baseline refreshed, notice cleared) — a save is an explicit "my buffer wins."

## What's missing in the code (the build list)

1. **Draft baseline in the record.** *(DONE — commit `36f70c2`.)* The draft
   record carries an optional `{mtime, size, contentHash}`, the journal format is
   v2 with version-tolerant replay, and `fastContentHash` (FNV-1a-64) exists. The
   store keeps its existing per-workspace keying (Decision 1 — no re-key).
1b. **Capture the baseline** at open, save, and reload (fill the field). Plus a
   **stable XDG root** in the app (`userStateRoot` → `$XDG_STATE_HOME/ssg`)
   replacing per-PID temp, sequenced with the reopen classification.
   *(src/Workspace.cpp, src/EditorRuntime.cpp, src/platform/*_files.cpp,
   apps/ssg_main.cpp)*
3. **Debounced flush of OPEN documents** (shared Phase 1): flush a dirty doc at
   most once per configurable interval (default 10s) since its last edit, plus on
   tab close (today) and a best-effort flush of all dirty docs on process exit.
   *(src/EditorRuntime.cpp, apps/ssg_main.cpp heartbeat + exit hook,
   include/ssg/EditorRuntime.h, Settings for the interval)*
4. **Reopen classification** on `file.open`: look up the draft for the file,
   compare baseline to disk, and drive load + conflict state. *(src/runtime/
   files.cpp, src/EditorRuntime.cpp)*
5. **The yellow clickable conflict notice**: a new **reserved chrome row**
   (a virtual notice row the viewport reserves above the document, like the
   prompt row — NOT document content row 0, which would shift line/caret/scroll/
   hit-test math), hit-tested for `[Diff]`/`[Use disk]`/`[Dismiss]`.
   *(src/snapshot.cpp / src/ShellState.cpp, viewport/layout reservation,
   HitTester, Renderer)*
6. **`draft.diff` command**: assemble a draft-vs-disk diff tab from `computeDiff`
   + `seedNonGit`. *(src/runtime/files.cpp or navigation.cpp, src/DiffModel.cpp
   reuse)*
7. **`draft.discard` command**: archive + `removeDocument` + reload-from-disk +
   clear dirty/notice. *(src/runtime/files.cpp, reuse ExternalModificationFlow
   reload)*

## Decisions (settled — implement to these)

1. **Storage: keep the existing per-workspace store; make its root stable.**
   *(Revised after implementation investigation — supersedes the original
   "central per-file abspath store" sketch.)* The existing `ScratchStore` already
   keys saved-file drafts by **`sha256(canonical-absolute workspace root)` +
   workspace-relative path**, which *is* the file's canonical absolute path,
   decomposed. So "recover a file opened by any relative path / symlink / CWD
   **within the same workspace**" is already structurally keyed by absolute
   identity — the reasons draft recovery does not work today are NOT the keying,
   but (a) the app points the scratch/recovery root at a **per-PID temp dir**
   (`apps/ssg_main.cpp:657`) so nothing survives a restart, and (b) the baseline
   is never captured. Therefore:
   - **Do NOT re-architect the store into a central per-file abspath store.** That
     would rewrite the crash-safe `ScratchSession` remnant/claim/quota model for a
     narrow benefit (recovering the *same file* under *different* workspace roots)
     and collide with the deferred multi-tab spec's per-workspace session model.
   - **Make the root stable:** the app resolves the scratch/recovery/archive root
     under `$XDG_STATE_HOME/ssg` (add a `userStateRoot` beside the existing
     `userConfigRoot`/`userCacheRoot`), overridable, instead of per-PID temp. This
     is what makes drafts survive a restart. Sequence it with/after the reopen
     classification (a stable root activates the currently-dormant remnant/claim
     machinery, which needs classification in place to behave).
   - **Cross-workspace recovery** (the same file opened under two different
     workspace roots getting one shared draft) is **out of scope**; revisit only
     if it proves necessary, as its own designed+reviewed change.

   This keeps the deferred multi-tab spec's per-workspace session model intact —
   no reconciliation split is needed after all; saved-file drafts and the session
   manifest live in the same per-workspace journal, exactly as that spec assumed.
2. **Hash: a fast non-cryptographic hash** (xxHash/FNV-class) over raw bytes,
   for change-detection only — no integrity/security claim.
3. **Diff: two-way, draft vs current disk.**
4. **Flush cadence: configurable seconds, default 10.** Flush a document's draft
   only when it is **dirty**. To bound `kill -9`/crash loss, do an **eager first
   flush** on the first edit after a document becomes dirty, then debounce
   subsequent edits to at most once per interval since the last flush. Also flush
   unconditionally on **tab close** (as today) and on **clean process exit** (a
   best-effort final flush of all dirty open documents before teardown — this
   does NOT run on `SIGKILL`, which is why the interval bounds the crash window;
   see RPO in Goal). The interval is a setting (`SettingsModel`,
   `.ssg/settings.json`); the app emits the heartbeat, the library decides which
   dirty docs are due.
5. **`draft.discard` / `[Use disk]`: no prompt.** It is non-destructive because
   the draft is archived first, so it needs no confirmation. Bind to a
   discoverable key (proposed `Alt+Shift+D`, confirm during implementation) and
   expose via the notice's `[Use disk]` region and the palette.

## Acceptance (Definition of Done)

- Observable (needs user signoff): edit a file, **wait for one flush tick**,
  `kill -9`, reopen → unsaved edits return as a dirty buffer, no disk write. Then
  `touch` the file externally and reopen → same, no false conflict. Then genuinely
  change the file externally and reopen → the yellow notice appears, `[Diff]`
  shows draft-vs-disk, `[Use disk]` loads disk and archives the draft.
- Gates: `bash scripts/check.sh` green (0 warnings), `SSG_TREESITTER` ON and OFF.
- Oracles (write before code):
  - Round-trip: edit → heartbeat flush → drop session → reopen → buffer
    byte-identical, dirty=true.
  - Exit flush: edit → clean-exit hook (no interval tick elapsed) → reopen →
    edits present (proves clean exit is a flush point, not just the interval).
  - Eager first flush: the first edit after a doc becomes dirty is durable within
    one tick (bounds the kill/crash window), not after a full interval.
  - Interval gating: after the eager first flush, a dirty doc is flushed at most
    once per interval; a clean doc is never flushed.
  - No-conflict path: baseline unchanged (disk hash == baseline hash) → draft
    offered, notice absent — asserted with `(mtime,size)` BOTH matching and
    differing-but-hash-equal, so the fast pre-check is never trusted alone.
  - Touched-identical: mtime differs, hash equal → no conflict.
  - Same-size external rewrite: disk content changed but size preserved and mtime
    coarse/unchanged → still detected as a conflict (proves hash is authority, not
    the `(mtime,size)` pre-check).
  - Conflict: disk content changed → notice present, `draft.diff` yields the
    correct draft-vs-disk hunks.
  - Converged: draft == disk → draft dropped, buffer clean, and it is NEVER
    briefly loaded as dirty first (convergence checked before the dirty load).
  - Missing file: deleted on disk → orphaned dirty buffer, diff vs empty.
  - Discard is reversible: `draft.discard` archives the draft (present in the
    central archive root) and loads disk.
  - Unknown baseline (legacy draft, no baseline field) → treated as conflict, not
    as unchanged.
  - Torn draft record → ignored by replay, open proceeds (no crash).
  - Draft write failure / quota-full / undraftable oversized file: the failure is
    surfaced (failed durability state on the tab) and the tab is NEVER presented
    as safely recoverable; the user is not falsely reassured.
  - Notice row does not perturb the document: with the conflict notice shown, the
    caret line, line numbers, scroll offset, and click→cell hit-testing are
    identical to the same document without the notice (proves the notice is
    reserved chrome, not document row 0).
  - Recovery scope: a file opened via two different in-workspace paths (and via a
    symlink) resolves to ONE draft (same workspace-relative key under the same
    workspace store); an out-of-workspace absolute path is rejected by the
    Workspace contract (documents the scope boundary).
