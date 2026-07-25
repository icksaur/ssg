# spec-file-finder

## Status

Done. P1 landed in d0a35bb/f1d9bdb, P2 in this change. Deviations from the plan
as written are recorded inline below.

Status: draft (spec review pending)

## Goals

Add a VSCode-style fuzzy file opener bound to the leader chord `Escape p`,
moving the existing command palette to `Escape Shift+P`. The opener lists
workspace **files only** (never directories), respects `.gitignore` by
default, and exposes a command to toggle that filtering on and off. It reuses
the existing palette's fuzzy matcher, windowing, rendering, prompt routing,
and client input handling unchanged — the only genuinely new capability is
"where the candidate list comes from" and "what happens on submit".

Delivered in two phases: **P1** objectifies the picker seam so a second
picker is transcription rather than surgery; **P2** implements the file
opener on top of it.

## Design

### The problem P1 solves

Today "palette" names two different things: the generic fuzzy-select overlay
MECHANISM (query + ranked list + windowed scroll + selection + paint) and the
specific command-list INSTANCE of it. The mechanism is already fully generic
— `PaletteSearcher::rank` already scores both `label` and `id` and already
awards a word-boundary bonus after `/`, `_`, `-`, `.` (`src/PaletteSearcher.cpp`),
i.e. it is already a path matcher. Only two points are command-specific:

- candidate production: `EditorRuntime::Impl::paletteView()` (`src/runtime/snapshot.cpp:301`)
  hardcodes `mode = SearchMode::Command` and builds from `descriptors()`.
- submit fulfilment: the client maps `prompt.submit` -> `palette.execute`
  (`apps/ssg_main.cpp`'s `dispatchResolved`).

Adding a second picker by copy-paste would fork the client's four loose
locals (`paletteOpen`, `paletteQuery`, `paletteSelected`, `paletteFirstVisible`)
and add a second unchecked submit branch, with no compiler or test help if a
future third picker forgets one of the points.

### P1 mechanism: an enumerated picker kind with a per-kind descriptor

`PickerKind` — a new, small, exhaustive enum (P1: `Command` only; P2 adds
`File`) with a `kAllPickerKinds` array, mirroring this project's existing
`kAllSemanticRoles` / `kAllSyntaxScopes` / `kAllOptionalSubsystems`
exhaustiveness convention.

`PickerDescriptor` — one per kind, declaring every point a picker must wire:
- `kind`
- `openCommandId` (the command that opens it)
- `promptTitle` (the `PromptRequest` title)
- `wireMode` (the `SearchMode` published on the snapshot)

`PickerCatalog` — returns the descriptor for a kind; the single place the set
is enumerated.

Chosen over the alternative of reusing `SearchMode` directly as the
discriminator: `SearchMode` has five values (`File`, `Line`, `Symbol`,
`Text`, `Command`) of which only two are pickers, so an exhaustiveness oracle
over it would be diluted by three permanently-inapplicable values. A
dedicated two-value enum makes "every picker kind is fully wired" a real,
failing-when-violated check. `SearchMode` remains the WIRE vocabulary
(`PaletteViewState.mode`, already carried by the protocol with a `File` value
that predates this spec) via the descriptor's `wireMode`, so **no protocol
change and no wire-name rename is in scope**.

### P1 ownership changes

Server (`EditorRuntime::Impl`):
- new field `std::optional<PickerKind> openPicker` — which picker the active
  `PromptKind::Palette` prompt belongs to.
- `paletteView()` becomes kind-dispatched: it reads `openPicker`, sets
  `view.mode` from that kind's `wireMode`, and fills candidates from that
  kind's candidate source. With `openPicker` unset it returns the empty view.
- the open handler sets `openPicker`; `palette.close` and any other path that
  closes the palette prompt clear it (see Considerations).

Client (`apps/ssg_main.cpp`):
- the four loose palette locals collapse into ONE `PickerWindow`-shaped value
  so a second picker cannot introduce a parallel drifting set.
- submit branches on the snapshot's published `palette.mode`, not on a
  client-invented flag.

### P2 mechanism: a dedicated, on-open workspace file index

`WorkspaceFileIndex` — a library-owned object that walks the workspace root
once and produces `std::vector<PaletteCandidate>`. Owns the walk, the
gitignore pruning, the cap, and the ordering.

Chosen over reusing `TreeProviderSnapshot::fromFilesystem`: that walk emits
directories as well as files, carries per-node tree/expansion/git-status
state the picker does not want, is coupled to the panel's provider lifecycle,
and has no ignore support. A dedicated index is smaller than adapting it and
keeps the tree panel's contract untouched.

Candidate shape (mirrors VSCode's two-column look with zero matcher changes):
- `id` = workspace-relative path (`src/runtime/snapshot.cpp`) — exactly what
  `file.open` takes as its payload, and what `idScore` matches for
  path-segment queries.
- `label` = filename (`snapshot.cpp`) — what `labelScore` matches, giving
  filename queries the start-of-string bonus, and what paints left-aligned.
- `detail` = parent directory (`src/runtime`) — paints right-aligned in the
  existing `paintPalette` detail column.

Gitignore is consulted through the EXISTING injected mechanism-adapter
pattern: `GitRepository` (`include/ssg/GitDiffSource.h:64`) gains an ignore
query, implemented in `src/platform/git_repository.cpp` over libgit2's
`git_ignore_path_is_ignored`. The index opens its OWN repository handle via
`makePlatformGitRepository`; it does not share the git-diff worker's handle
(libgit2 handles are not safe to share across threads).

**Path domains do not coincide and must be rebased explicitly.** Candidate
`id` is WORKSPACE-relative, but libgit2's ignore query is REPO-WORKDIR-relative,
and the workspace root may be a subdirectory of the repository (opening
`ssg/src` as the workspace inside the `ssg` repo). Passing a workspace-relative
path to libgit2 in that case silently mis-filters — patterns match against the
wrong prefix, producing plausible-looking but wrong results with no error.
The ignore query therefore takes a path the adapter rebases onto the repo
workdir; the workspace-relative form is never handed to libgit2 directly.

Toggle: `SettingKey::FileFinderRespectGitignore` (bool, default `true`) plus
an argument-free `file_finder.toggle_gitignore` command. A dedicated command
rather than `settings.set` because `settings.set` requires a typed payload and
is therefore not reachable from a bare keystroke or a parameterless palette
entry (the same reason `view.toggle_word_wrap` exists alongside it).

Both new commands (`file_finder.open`, `file_finder.toggle_gitignore`) join
`searchCommandSet()` alongside the `palette.*` ids and are bound through the
existing `bindRuntimeNavigation` search-command loop, keeping the whole picker
domain — open, submit guard, close, toggle — in `src/runtime/navigation.cpp`
rather than split across command modules.

### Submit is mode-dispatched for BOTH keyboard and pointer

There are TWO submit paths today, not one, and both are command-specific:

- keyboard: `prompt.submit` -> `executeSelectedCandidate` -> `palette.execute`
  (`apps/ssg_main.cpp`).
- pointer: a click on a palette row resolves the hit row to a candidate id
  (`apps/ssg_main.cpp`'s `HitRegion::Palette` branch sets
  `targets.palette_command_id`), and `route_pointer`
  (`apps/pointer_routing.cpp`) then unconditionally emits
  `palette.execute{that id}`.

For the file picker a candidate's `id` is a PATH, so the pointer path would
both fail the hardened `palette.execute` guard AND, absent that guard, try to
execute a path as a command id. Both paths must therefore dispatch through the
same mode dispatch: `route_pointer` gains the published picker mode as an
input and emits the mode's submit command (`palette.execute{id}` for Command,
`file.open{id}` for File), keeping it a pure, unit-testable function as it is
today. The target field is renamed to reflect that it carries an activated
CANDIDATE id, not specifically a command id.

### Keymap

- `Escape KeyP` -> `file_finder.open` (was `palette.open`)
- `Escape Shift+KeyP` -> `palette.open` (new; verified free — the only other
  Shift-bearing `*`-context chords are `Escape Shift+KeyZ` and the
  `editor`-context `Escape Shift+Arrow*`)

Both keep the incidental Alt equivalence documented in `doc/spec-mod-keys.md`:
`Alt+p` sends `ESC p` and `Alt+Shift+P` sends `ESC P`, which `decode_input`
already decodes as `KeyP` with `shift` set from the ASCII case.

## Invariants

- I25 / feature-not-mechanism (`doc/spec.md`): the walk, the ignore filtering,
  the ranking and the candidate list all live in the library. The client owns
  only its window state (query/selection/scroll) and byte decoding. A second
  client must get the file opener for free.
- K5 (`doc/spec-keymap.md`): every keymap-bound command dispatches with an
  EMPTY payload. `file_finder.open` and `file_finder.toggle_gitignore` are
  argument-free; the existing `curatedKeymapBindingsAreArgumentFree` oracle
  covers them once bound.
- K2 (`doc/spec-keymap.md`): prefix-freeness per resolvable context. `Escape
  KeyP` and `Escape Shift+KeyP` are distinct 2-stroke sequences, neither a
  prefix of the other.
- P0 command-catalog completeness (`data/required-commands.json` +
  `tests/test_required_commands.cpp` + `tests/runtime/command_cases.h` +
  a `doc/features/*.md` mention): both new command ids must be added to all
  four, or the existing catalog-parity and feature-doc-union tests fail.
- I22 / theme-is-sole-color-source: the file rows reuse `paintPalette`
  unchanged and introduce no color.
- INV-derived-view-bounded: the client adopts back the clamped selection and
  resolved offset from `PaletteSearcher::report`; it invents no product data.

## Considerations

- **Prune ignored DIRECTORIES during descent; do not post-filter files.**
  This is the single most important performance rule and is not reconstructible
  from the behavior: a post-hoc filter still pays the full walk of
  `node_modules`/`build`/`target`. In this repo alone the tree is 2866 files
  on disk vs 1224 tracked; the gap is almost entirely gitignored build
  directories. `std::filesystem::recursive_directory_iterator` supports this
  via `disable_recursion_pending()` on a directory entry.
- **`.git/` is always pruned**, regardless of the gitignore setting. It is not
  listed in `.gitignore` (git ignores it implicitly) but must never appear as
  an openable file.
- **Toggling gitignore while the picker is open must rebuild the index**, or
  the toggle silently appears to do nothing — the most likely user-visible
  bug in this feature.
- **Index freshness is open-scoped.** Built when the picker opens, discarded
  when it closes. Files created or deleted while the picker is open are not
  reflected; reopening picks them up. Accepted to keep the walk off the
  per-keystroke and per-frame paths.
- **The index is capped** (mechanism: a fixed maximum entry count). A
  pathological tree must not hang the picker's open. Truncation is a
  documented, deterministic outcome (walk order), not a silent one.
- **No usable git repository** (not a repo, or libgit2 fails to open it): the
  ignore query is a no-op and every file is listed, regardless of the
  setting. Not an error, no diagnostic.
- **Symlinks are not descended into**, matching the existing tree walk's
  `disable_recursion_pending()` behavior, so a cyclic link cannot hang the
  walk.
- **`openPicker` must be cleared on EVERY path that closes the palette
  prompt**, not just `palette.close`: `prompt.cancel` fulfilment, the
  `reconcilePromptFocus` path, and a successful `palette.execute` (which
  cancels the prompt) all end the picker. A stale `openPicker` would make the
  NEXT `palette.open` publish the wrong candidate list. The oracle for this is
  an explicit open-file-picker -> cancel -> open-command-palette -> assert
  `mode == Command` sequence.
- **`palette.execute` must keep rejecting when a non-command picker is open.**
  Its existing guard (`src/runtime/navigation.cpp:34`) only checks that a
  Palette-kind prompt is active; with two pickers sharing that kind it must
  also check `openPicker == Command`, or a file-picker submit could be coerced
  into executing an arbitrary command id.
- **There are TWO submit paths, and the pointer one is easy to miss.** The
  palette-row CLICK path (`apps/ssg_main.cpp`'s `HitRegion::Palette` branch ->
  `route_pointer` -> unconditional `palette.execute`) is entirely separate
  from the keyboard `prompt.submit` path. Hardening `palette.execute` without
  fixing it makes clicking a file row silently fail. Both must be
  mode-dispatched (see Design).
- **Workspace root may be a subdirectory of the git repository**, so
  workspace-relative and repo-workdir-relative paths differ. This mis-filters
  silently rather than erroring (see Design), and is covered by a dedicated
  nested-workspace test fixture.
- **`SettingKey` is enumerated by hand in the protocol codec.**
  `src/Protocol.cpp:1569` lists every key for decoding, and `kSettingKeyCount`
  is a separate constant. A new key added only to `Settings.h` compiles but
  fails to decode over the wire.
- The client's `paletteOpen` name becomes a lie in P1 (it means "a picker is
  open"). Renaming it is in P1 scope; leaving it is the drift P1 exists to
  prevent.

## Risks and Mitigations

- **Risk: P1 silently changes palette behavior.** Mitigation: P1 is
  behavior-preserving; the entire existing suite (including
  `test_terminal_parity`'s real-binary comparison and the palette tests) is
  its acceptance gate, with no test edits permitted in P1 except the new
  structural one.
- **Risk: the P1 exhaustiveness oracle is a fake oracle** (only one kind
  exists in P1, so it passes trivially). Mitigation: it MUST be verified by
  perturbation during P1 — temporarily add a dummy `PickerKind` value,
  confirm the test fails, revert — the same verification this project used
  for `test_config_doc` and the theme-drift oracle. P2 then exercises it for
  real.
- **Risk: picker open on a huge tree blocks the UI.** Mitigation: the cap,
  plus directory pruning, plus a measured budget (below). If the budget is
  missed, the fallback is a background build — deliberately NOT in scope
  here, and the budget test is what would force that decision.
- **Risk: libgit2 handle misuse across threads.** Mitigation: the index owns
  a separate handle; a code-review checkpoint on this specific point.
- **Risk: the key swap surprises muscle memory.** Mitigation: it is the
  explicit request; the palette's own entry for `palette.open` shows its new
  chord automatically (`detail` is derived live from the keymap).

## Acceptance (Definition of Done)

- Observable (P1): no user-visible change whatsoever. `Escape p` still opens
  the command palette, types, ranks, scrolls, selects and executes exactly as
  before.
- Observable (P2, needs visual signoff): `Escape p` opens a file list showing
  filename + parent directory; typing fuzzy-filters it; `Enter` opens the
  selected file in a tab; **clicking** a row opens it too; `Escape Escape`
  cancels; `Escape Shift+P` opens the command palette instead; build/ and
  other gitignored files are absent by default and present after
  `file_finder.toggle_gitignore`.
- Budgets: building the index for this repo's own workspace completes within
  a bound asserted by a test (mechanism: a wall-clock assertion in the index
  test, calibrated so the measured value has meaningful headroom). Ranking and
  per-frame windowing add no new budget — they are the existing palette path.
- Gates: `bash scripts/check.sh` green, zero warnings.
- Oracles: see the Plan's Oracle column; every non-mechanical step names one.

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Add `PickerKind`/`kAllPickerKinds`/`PickerDescriptor`/`PickerCatalog` (P1: `Command` only), mapping kind -> open command id, prompt title, and wire `SearchMode` | `include/ssg/PaletteSearcher.h` (or a new `include/ssg/Picker.h`), `src/PaletteSearcher.cpp`/new `src/Picker.cpp`, `cmake/components/search-palette.cmake` | test: every `kAllPickerKinds` value has a catalog descriptor whose `openCommandId` is present in `p0CommandDescriptors()` and whose title is non-empty, AND the `Command` descriptor's exact semantics are pinned (`openCommandId == "palette.open"`, `wireMode == SearchMode::Command`) so a wrong-but-existing id cannot pass — **verify by perturbation** (add a dummy kind, confirm failure, revert) | P0-catalog |
| 2 | Add `EditorRuntime::Impl::openPicker`; set it in the palette-open handler; clear it on `palette.close`, on `prompt.cancel` fulfilment, on successful `palette.execute`, and in `reconcilePromptFocus` when the palette prompt is gone | `src/runtime/editor_runtime_internal.h`, `src/runtime/navigation.cpp`, `src/EditorRuntime.cpp` | test: open -> cancel -> open again publishes the correct `mode` (no stale kind); every close path clears it | - |
| 3 | Kind-dispatch `paletteView()`: read `openPicker`, set `view.mode` from the descriptor's `wireMode`, fill candidates from the kind's source (P1: only the existing command source) | `src/runtime/snapshot.cpp` | existing palette/runtime snapshot suite green, unmodified | I25 |
| 4 | Collapse the client's four palette locals into one `PickerWindow` value; branch submit on the snapshot's published `palette.mode` rather than a client flag; rename `paletteOpen` to reflect "a picker is open" | `apps/ssg_main.cpp` | existing `test_ssg_app` + `test_terminal_parity` real-binary suites green, unmodified | INV-derived-view-bounded |
| 5 | **P1 gate**: both build gates green with no test edits other than step 1's | - | full suite green | - |
| 6 | Extend `GitRepository` with an ignore query that rebases a workspace-relative path onto the repo workdir; implement over `git_ignore_path_is_ignored`; index opens its OWN handle | `include/ssg/GitDiffSource.h`, `src/platform/git_repository.cpp`, `tests/test_git_repository.cpp` | test: in a temp repo with a `.gitignore`, an ignored path reports ignored and a tracked path does not; a NESTED case where the workspace root is a SUBDIRECTORY of the repo asserts the rebase (a pattern anchored at repo root still matches correctly); a non-repo directory reports "not usable" and never ignores | I25 |
| 7 | Add `WorkspaceFileIndex`: recursive walk, files only, prune ignored directories during descent, always prune `.git/`, no symlink descent, capped, deterministic order; emits `PaletteCandidate{id=relpath, label=filename, detail=parentdir}` | new `include/ssg/WorkspaceFileIndex.h`, `src/WorkspaceFileIndex.cpp`, new `cmake/components/workspace-file-index.cmake`, `tests/test_workspace_file_index.cpp` | test: hand-built fixture tree with a `.gitignore` — asserts exact candidate set (files only, no dirs, ignored subtree absent, `.git/` absent) against an independently-enumerated expected list; second case with filtering off asserts the ignored subtree reappears; a case asserting an ignored DIRECTORY's contents are never visited (not merely filtered out); cap case asserts truncation is deterministic; budget assertion on the walk | I25 |
| 8 | Add `SettingKey::FileFinderRespectGitignore` (bool, default true), bumping `kSettingKeyCount` AND the hand-maintained SettingKey decode array in the protocol codec (`src/Protocol.cpp:1569`), which enumerates every key by hand and will silently fail to decode a missing one | `include/ssg/Settings.h`, `src/Settings.cpp`, `src/Protocol.cpp`, `tests/test_settings.cpp`, `tests/test_protocol.cpp` | test: default resolves true; `settings.set` round-trips it; protocol round-trip covers the new key (a key absent from the decode array fails to decode) | - |
| 9 | Add `PickerKind::File` + its catalog entry, candidate source (build the index on open, honoring the setting), and `file_finder.open` command; guard `palette.execute` to `openPicker == Command` | `include/ssg/Picker.h`, `src/runtime/navigation.cpp`, `src/runtime/snapshot.cpp`, `src/runtime/editor_runtime_internal.h` | test: step 1's exhaustiveness oracle now genuinely forces the new wiring; runtime test — `file_finder.open` publishes `mode == File` with the workspace's files; `palette.execute` is REJECTED while the file picker is open | K5, P0-catalog |
| 10 | Add `file_finder.toggle_gitignore` (argument-free); rebuild the open picker's index when toggled | `src/runtime/navigation.cpp`, `src/runtime/editor_runtime_internal.h` | test: with the picker open, toggling changes the published candidate set in the SAME session (the rebuild corner case) | K5 |
| 11 | P0 catalog cascade for both new ids. **Both join `searchCommandSet()`** alongside the `palette.*` ids and are bound via the existing `bindRuntimeNavigation` search-command loop, so `tests/test_search.cpp`'s EXACT expected-descriptor list (`tests/test_search.cpp:253`) must be updated or it fails while P0 parity is still green | `data/required-commands.json`, `tests/test_required_commands.cpp` (list + count `static_assert` + the `capabilityAndSurfaceExclusionsAreExact` branch), `tests/runtime/command_cases.h` (+ its `static_assert`), `include/ssg/Search.h`, `src/Search.cpp`, `src/runtime/navigation.cpp`, `tests/test_search.cpp`, new/updated `doc/features/*.md` | existing catalog-parity, feature-doc-union, and search-command-set exact-list tests green | P0-catalog |
| 12 | Mode-dispatch BOTH submit paths: keyboard (`prompt.submit`) and pointer (palette-row click). `route_pointer` takes the published picker mode and emits `palette.execute{id}` or `file.open{id}`; rename the target field from `palette_command_id` to an activated-candidate id | `apps/ssg_main.cpp`, `apps/pointer_routing.h`, `apps/pointer_routing.cpp`, `tests/test_ssg_app.cpp` | test: extend the existing `routePointerPalettePressExecutesTheCandidate` unit case with a File-mode case asserting `file.open{path}` (NOT `palette.execute`); keyboard fulfilment table covers the File branch | INV-derived-view-bounded |
| 13 | Rebind the keymap: `Escape KeyP` -> `file_finder.open`, `Escape Shift+KeyP` -> `palette.open` | `src/EditorRuntime.cpp`, `tests/runtime/test_runtime_snapshot.cpp`, `doc/spec-keymap.md` | test: `resolveSequence` returns the new command for each chord; existing `curatedKeymapBindingsAreArgumentFree` and keymap-validation oracles green | K2, K5 |
| 14 | **P2 gate**: both build gates green; PTY end-to-end verification of the Observable list; user visual signoff | - | real-binary PTY harness | - |

## Implementation deviations

- Step 6 landed as a dedicated `GitIgnoreMatcher` interface plus
  `makePlatformGitIgnoreMatcher`, not as a method on `GitRepository`. Every
  `GitRepository` method opens and closes a repository handle per call, which is
  right for one scan per refresh but wrong for the thousands of queries a walk
  makes; the matcher holds its handle for its lifetime.
- The file picker closes server-side when its submit succeeds, in
  `EditorRuntime::dispatch` beside the `pendingPaletteTarget` handling.
  `file.open` has no prompt side effects of its own (unlike `palette.execute`,
  which cancels the prompt as part of executing), so without this the picker
  survived its own submit -- found by driving the real binary under a pty, not
  by any test. Owning it server-side rather than closing from the client gives
  close-on-success semantics matching `palette.execute`, identically for
  keyboard and pointer: a rejected open leaves the picker up with its query.
- The client resets the picker window for ANY id in `pickerCatalog()`, not for
  a hardcoded `"palette.open"`. The hardcoded form left the file picker
  inheriting the previous query (observed: opening the file picker after a
  command-palette query pre-filtered the file list).

## Rationale

The two-phase split is the user's explicit request and is also the honest
engineering order: the existing palette is one instance of a mechanism that
was never named, and the cost of not naming it is paid once per new picker.
P1 pays that cost while the behavior is frozen and the whole existing suite
can act as the oracle — the safest possible moment. P2 is then additive.

The decision to reuse `PaletteCandidate` unchanged (rather than introduce a
file-specific candidate type) is what makes almost the entire stack — wire
codec, projection, renderer, scroll windowing, client input — apply with zero
edits. The matcher's existing `/`-boundary bonus means even the ranking
quality was already tuned for paths; this spec adds no scoring code.
