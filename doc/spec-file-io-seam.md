# spec-file-io-seam

Status: draft (spec review pending)

Architecture prep for `spec-file-management.md`. This is preparatory work: it
changes no user-visible behavior except where it removes a silent-failure bug.
The existing test suite is the primary oracle for the routing steps.

## Goals

All filesystem access in the library and the TUI client goes through one named
seam, so that: name-clash and durability rules are enforced in one place rather
than restated at each call site; failure paths (a full disk, a lost race, an
unreadable config) become unit-testable; and a missing file can no longer be
mistaken for an empty one.

## Current state (verified)

`include/ssg/platform_files.h` is already the seam, with Linux and Windows
implementations under `src/platform/`. It owns:

- `validateWorkspaceRelativePath` (traversal, absolute, reserved names, length)
- `fileIdentity`, `tryLockFile` / `ExclusiveFileLock`, `setOwnerOnlyPermissions`
- `userCacheRoot`, `userConfigRoot`
- `replaceFileAtomically` (mkstemp -> fchmod -> write -> fsync -> rename ->
  fsync parent)

What it does **not** own is reading. There is no read primitive at all, which is
why three independent ad-hoc readers exist. The journal and recovery subsystems
are separately well-hardened (fsync on every write, atomic rename-into-place)
and are not in scope to change.

Raw I/O to route (from a full-repository audit):

- `src/EditorRuntime.cpp:212` `readFileText` -- raw `ifstream`; **returns an
  empty string for a missing or unreadable file with no error**. This is the
  same failure shape as the tree-sitter highlight-query bug: absence
  indistinguishable from emptiness, silently accepted downstream.
- `src/EditorRuntime.cpp:1022` `Impl::writeFile` (LSP workspace edits) -- raw
  `ofstream`, truncating, **no fsync, no atomic replace**. An LSP-applied edit
  is not durable and a crash mid-write truncates the user's file.
- `src/EditorRuntime.cpp:1037` LSP rename, `:1045-1055` LSP delete -- raw
  `std::filesystem` calls that clobber.
- `src/Settings.cpp:308` settings read -- raw `ifstream`.
- `src/RecoveryActions.cpp:169` manifest write, `:285` manifest read -- raw
  streams inside an otherwise-hardened subsystem.
- `src/ScratchJournal.cpp:355` journal read -- raw `ifstream`.
- `apps/ssg_main.cpp:429, 600` init.lua read -- raw `ifstream`.

Explicitly staying raw: the direct `::open`/`::fsync` directory-sync calls in
`ScratchJournal.cpp` and `RecoveryActions.cpp`, and read-only tree walks
(`TreeModel`, watchers, scratch session scanning). They are durability
primitives and directory enumeration respectively, not file content access.

## Design

### Shape of the seam

The seam stays **free functions in `platform_files.h`**, not an injected
interface. Reasons: it is already the single point of truth for writes, locks,
permissions and roots, so growing it keeps one seam rather than creating a
second; the platform split (`linux_files.cpp` / `windows_files.cpp`) is the
existing mechanism for varying behavior; and virtualizing the filesystem behind
a vtable would touch every subsystem for a testability benefit that fault
injection already delivers more cheaply.

Testability comes from two things that already work in this codebase: higher
layers take their roots from config (`scratchRoot`, `recoveryRoot`, workspace
root), so tests run against temporary directories; and failure paths are
exercised by fault injection, following the existing `RecoveryFaultInjector`
precedent rather than inventing a second style.

### New primitives

- `readFile(path) -> ReadResult` returning bytes or a distinct not-found /
  io-error status. **There is no overload that returns a bare string**, so
  "missing reads as empty" is not expressible. Every current ad-hoc reader is
  replaced by this.
- `createFileExclusively(path, contents)` -- creates and fails if the path
  exists, with the exclusivity enforced by `O_CREAT|O_EXCL` (and the Windows
  `CREATE_NEW` equivalent), then fsync + parent fsync as `replaceFileAtomically`
  does. This is the primitive behind the clash rule; an `exists()` helper is
  deliberately **not** added, because offering one invites the
  check-then-write race the clash rule exists to prevent.
- `renameFileNoClobber(source, destination)` -- fails rather than replacing an
  existing destination. Plain `std::filesystem::rename` silently replaces, so
  the seam must offer the safe form and call sites must not reach past it.
- `removeFile(path)` -- reports not-found distinctly from a permission failure.
- `copyFileDurably(source, destination)` -- exclusive-create at the destination
  plus fsync of the file **and of the destination's parent directory**, so the
  new directory entry itself is durable, not merely the bytes. The archive
  depends on this: a copy whose parent entry is not synced can vanish on crash,
  which would break I1 in `spec-file-management.md` while appearing to succeed.
  Every write primitive in the seam carries the same parent-fsync obligation, as
  `replaceFileAtomically` already does.

### Fault injection

A `FileIoFaultInjector` seam, modeled on `RecoveryFaultInjector`, lets a test
force a chosen primitive to fail at a chosen call. This is what makes
"the archive write failed, so the delete must not proceed" a unit test rather
than a code-reading exercise. It is compiled in but inert by default; the
default path must have no branch cost beyond a null check.

### init.lua

`apps/ssg_main.cpp` reads init.lua with a raw `ifstream` in two places. Both
route through `readFile`, and the two read sites collapse to one.

**User-visible behavior is unchanged**: a missing or blank init.lua remains
normal and silently uses defaults. That is the existing contract and this spec
does not alter it. What changes is internal: the *code* can now distinguish
"absent" from "present but empty" from "present but unreadable", so a permission
error or an I/O failure on a config the user does believe exists is no longer
indistinguishable from having no config. Only the unreadable case gains a
report; absence stays silent.

## Invariants

- J1 No `ifstream`/`ofstream`/`fopen` outside `src/platform/` in library or app
  code. Enforced by a test that greps the tree, with an explicit exemption list,
  in the style of the existing `sourceAndConfigHaveNoIndependentColorSources`
  guard. **J1 admits no "or record why not" escape**: a site that is not routed
  must appear in the guard's exemption list with a one-line justification, so
  the exception is itself a reviewed, greppable artifact rather than a silent
  omission.
- J2 A missing file is never observable as empty content.
- J3 Every content-producing write is durable (fsync of file and parent) before
  the operation reports success.
- J4 Non-clobbering is enforced by the filesystem, not by a prior check.
- J5 Existing behavior is otherwise unchanged; the current suite must pass
  without test edits except where a silent-failure bug is being fixed.

## Considerations

- The grep guard (J1) is the highest-value artifact here, because routing every
  call site once is worthless if the next feature adds a fresh `ifstream`. Its
  exemption list must be small and justified.
- `readFile` returning a status type changes call-site shape; the LSP and
  settings paths currently swallow errors and will now have a real error to
  report. Surfacing those is in scope; inventing new user-facing messaging for
  them is not.
- Fixing `Impl::writeFile` to be atomic changes LSP-applied-edit behavior from
  "truncate then stream" to "replace". Anything that held the file open across
  the write, or relied on inode stability, would notice. Nothing found does, but
  this is the riskiest routing step.
- Windows must reach parity: `CREATE_NEW`, `MoveFileEx` without
  `REPLACE_EXISTING`. The seam's contract is defined by behavior, not by the
  POSIX flag names.
- `RecoveryActions` manifest I/O is inside a two-phase-commit subsystem; routing
  it must not reorder its fsyncs. If routing would change ordering, leave it raw
  and add it to the J1 exemption list with the reason, so the exception is
  visible in the guard rather than only in prose.

## Risks and Mitigations

- Touching the recovery/journal subsystems risks breaking durability
  guarantees that are hard to test. Mitigation: route only the plain manifest
  read/write, leave the directory-sync syscalls alone, and stop if ordering
  would change.
- A large mechanical change can hide a behavior change. Mitigation: each step is
  separately committed with the full suite green, and the suite is the oracle;
  any step needing a test edit is a signal to stop and re-examine.
- The grep guard could produce false positives in vendored or generated code.
  Mitigation: reuse the existing guard's directory-exclusion approach
  (`vendor/`, `build*`, `doc/`).

## Acceptance (Definition of Done)

- Observable: no user-visible change. A missing init.lua and a missing document
  continue to behave exactly as today; only genuinely unreadable files (present
  but erroring) gain a report where they previously read as empty.
- Budgets: fast gate stays at its current runtime; no new test binary in the
  fast tier.
- Gates: `bash scripts/check.sh` and `scripts/check.sh push` green, zero
  warnings, no edits to existing tests.
- Oracles: per plan step.

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Add `readFile` returning bytes-or-status; no string-returning overload | `include/ssg/platform_files.h`, `src/platform/linux_files.cpp`, `windows_files.cpp` | hand case: missing path, unreadable path, empty file, and a normal file each produce distinct results | J2 |
| 2 | Add `createFileExclusively`, `renameFileNoClobber`, `removeFile`, `copyFileDurably` | same | ref-impl: pre-create the destination out-of-band; each primitive fails and leaves the existing bytes untouched | J3, J4 |
| 3 | Route `readFileText`, `Settings` read, `ScratchJournal` read to `readFile` | `src/EditorRuntime.cpp`, `src/Settings.cpp`, `src/ScratchJournal.cpp` | existing suite green, no test edits | J5 |
| 4 | Route init.lua reads; collapse the two sites to one | `apps/ssg_main.cpp` | behavior: absent init.lua is reported as absent, not as an empty script | J2 |
| 5 | Make LSP `writeFile` an atomic durable replace; route LSP rename/delete to the non-clobbering primitives | `src/EditorRuntime.cpp` | invariant: after an LSP write the file's content is complete or unchanged, never truncated (fault-injected mid-write) | J3, J4 |
| 6 | Route `RecoveryActions` manifest read/write; if ordering would change, leave raw and add it to the J1 exemption list with a justification | `src/RecoveryActions.cpp`, `tests/` | existing recovery suite green, no test edits; J1 guard passes either way | J1, J5 |
| 7 | Add `FileIoFaultInjector` | `include/ssg/platform_files.h`, `src/platform/` | behavior: an injected failure makes a chosen primitive fail; inert by default | - |
| 8 | Add the grep guard forbidding raw stream I/O outside `src/platform/` | `tests/` | perturbation: adding an `ifstream` to a non-exempt file fails the test | J1 |
| 9 | Prompt `commandId` -- **owned by `spec-file-management.md` step 1**, listed here only as the dependency this seam work must land alongside. Do not implement it twice | see `spec-file-management.md` | see that spec | - |

## Rationale

The audit's headline finding is that this codebase is not uniformly weak at I/O
-- the journal and recovery layers are careful, with fsync on every write and
rename-into-place. The weakness is that the careful primitives were never
offered as the *easy* path, so the document, settings, LSP and config paths each
reached past them to a raw stream. That is a pit-of-failure shape: the safe
operation is more typing than the unsafe one.

So the fix is not "add an abstraction layer" for its own sake. It is to make the
seam complete enough that reaching past it is never convenient, and then to make
reaching past it fail the build. Step 8 is therefore the step that gives this
work a lifespan; steps 1-7 without it would decay.
