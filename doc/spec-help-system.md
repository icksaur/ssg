# spec-help-system

An in-editor help page: a command opens a read-only, in-memory tab whose content
is part compiled-in prose and part generated at open time (current keybindings
and configuration help). A persistent `Alt+H  Help` hint sits at the
bottom-right of the footer.

Architecture-first. The dominant design question is not "how do we render help"
-- the existing document renderer already paints any text buffer -- but "what is
the correct abstraction for a read-only, in-memory tab that never persists and
cannot be saved," so the help page is a thin caller of pre-existing machinery.

## Goals

- A `help.open` command opens (or re-focuses) a Help tab.
- The tab shows text: compiled-in prose (overview, mouse usage) plus
  runtime-generated sections (the live keybinding list and configuration help).
- The tab is read-only: the cursor navigates, selection and mouse work, but the
  buffer cannot be edited and cannot be saved, and it never writes a scratch or
  draft file.
- A right-aligned `Alt+H  Help` footer hint, clickable, whose key label reflects
  the actual binding.

## What already exists (and is reused unchanged)

The read-only in-memory tab is almost entirely a matter of composing existing
parts. This section is normative: the design MUST reuse these rather than add
parallel mechanisms.

- **`DocumentMode::ReadOnly`** (`include/ssg/types.h`). Already a closed enum
  value. `Document::apply()` (`src/Document.cpp:135`) rejects every edit
  transaction on a `ReadOnly` document at the single edit chokepoint -- all edit
  sources (typing, edit commands, clipboard, undo/redo, multi-file replace, LSP
  code actions, drop) funnel through `Document::apply`, so read-only enforcement
  is already total and needs no new guard.
- **Autosave exclusion.** `autosaveCandidates()`
  (`src/EditorRuntime.cpp:1719-1741`) already skips any document whose
  `mode() != DocumentMode::Edit` (line 1730). A `ReadOnly` virtual document is
  therefore already excluded from draft persistence with zero new code -- the
  same gate that excludes live-diff (`Diff`) documents.
- **`Workspace::openVirtualDocument(label, text, mode)`**
  (`src/Workspace.cpp:708-716`). Creates an untitled (no path,
  `JournalDocumentKey::untitled(...)`) in-memory document. The live-diff feature
  already uses it with `DocumentMode::Diff`; help uses it with
  `DocumentMode::ReadOnly`.
- **`TabKind::ReadOnlyOutput`** (`include/ssg/TabManager.h:34`). A tab kind
  already reserved for read-only, non-file content. Help tabs use it.
- **Document rendering, mouse, URL linkification.** The renderer paints the
  active document's text regardless of mode; mouse selection, scrolling, and the
  existing clickable-URL runs (`urlsInTheDocumentBecomeClickableRuns`) all work
  on any document. "Mouse input" and "markdown shown as text" require no new
  rendering -- the help buffer is plain UTF-8 text (its Markdown is shown
  literally; there is no Markdown renderer and this spec does not add one).
- **`KeymapMatcher::preferredBinding(commandId)`** (`src/Keymap.cpp:396`) and
  **`KeyCodec::formatSequence(sequence)`** (`src/Keymap.cpp:231`). Together they
  turn a command id into a display string like `Alt+KeyH`. Both the generated
  keybinding section and the footer hint use them; neither reimplements binding
  lookup.
- **Command catalog + `renderCommandReference`** (`include/ssg/CommandReference.h`).
  The catalog carries every command's id, summary, and Lua/init surfaces. The
  generated "commands and keys" help section reads the live catalog rather than
  hand-maintaining a command list.

## Design

### The read-only in-memory tab seam (the "correct abstraction")

Introduce ONE reusable entry point on `EditorRuntime::Impl`, not specific to
help:

```
FileDocumentId openReadOnlyTab(TabKind kind,
                               std::string contentIdentity,
                               std::string label,
                               std::string text);
```

Ownership and behavior:

- It calls `workspace.openVirtualDocument(label, text, DocumentMode::ReadOnly)`,
  registers the document's runtime state, opens a tab via
  `tabs.openContent(kind, contentIdentity, label, ...)`, and activates it.
- **Dedupe and refresh by `contentIdentity`, via remove+recreate (no edit
  backdoor).** Like the live-diff path (`src/EditorRuntime.cpp:1207-1233`), a
  second call with an identity that is already open re-focuses the existing tab.
  Refreshing the content does NOT mutate the existing `ReadOnly` document in
  place -- there is no in-place text setter, and there must not be one, because
  `DocumentMode::ReadOnly` blocks `Document::apply` and that chokepoint must have
  no bypass. Instead, refresh REMOVES the old virtual document and opens a new
  `ReadOnly` virtual document with the new text, rebinding the same tab identity
  -- exactly the pattern live-diff already uses when its diff text changes
  (`src/EditorRuntime.cpp:1213-1220`: `removeDocument` then re-`openVirtualDocument`).
  The read-only edit invariant is therefore preserved: content only ever changes
  by constructing a fresh document, never by editing a read-only one. The
  identity is caller-chosen and stable; help uses a constant (e.g. `"help:main"`)
  so there is at most one help tab.
- It sets no path and no draft key. Because the document is `ReadOnly` and
  untitled, it is already excluded from autosave and has no disk identity.

This seam is the reusable "read-only output tab" primitive. Help is its first
caller; a future "command output" or "message log" tab would reuse it. It lives
in the runtime because tab/document/workspace ownership already lives there
(`EditorRuntime::Impl` owns `Workspace` and `TabManager`).

Rationale for a distinct seam rather than inlining into the help command: the
requirement was explicitly "make correct abstractions for readonly in-memory
tabs so the help system renders using primarily pre-existing code." Concentrating
the open+dedupe+activate+refresh policy in one method keeps every read-only-tab
caller consistent and keeps the help command a content-assembly function.

### Read-only UX (the decision the user offered two options for)

Chosen: **the cursor navigates freely; edits are rejected at the chokepoint; the
tab is clearly marked read-only; save fails gracefully.** This is the
pit-of-success combination of the two options the user described, using the
mechanism that already exists.

- Navigation, selection, copy, find, scroll: all work (none mutate the buffer).
- Typing / delete / paste / any mutating command: rejected by `Document::apply`
  returning `DocumentError::ReadOnly` (already implemented). It is a silent
  no-op to the buffer. OPTIONAL (reviewer to weigh): surface a transient status
  line "read-only" on the first rejected edit attempt so the block is not
  mysterious; default to no status noise if it complicates the edit path.
- **Read-only marker.** The tab is visibly marked so the user knows why editing
  does nothing. `TabState` already carries `mode` (`include/ssg/TabManager.h:53`).
  The tab title composition (`composedTabTitle`) gains a read-only affordance,
  but with an EXPLICIT PER-MODE POLICY so it does not silently change other tab
  kinds' established labels: `DocumentMode::ReadOnly` tabs get the `(readonly)`
  marker (e.g. `Help (readonly)`); `DocumentMode::Diff` tabs KEEP their existing
  live-diff affordance (`liveDiffPrefix`) and are NOT given the read-only marker;
  `Edit` tabs are unchanged. The marker is thus scoped to `ReadOnly` mode (which
  also covers existing binary/decode-failure read-only tabs -- an intentional,
  reviewed consistency win), not "all non-Edit modes." Exact spelling is a
  Style/Considerations detail.

### Closing the save gap

Today `file.save` on an untitled document routes to a Save-As prompt
(`src/runtime/files.cpp:170-177`). A read-only help tab is untitled, so without a
guard, `Alt+S` on it would open a Save-As prompt -- wrong. Add a guard at the
`file.save` handler: if the active document's `mode() != DocumentMode::Edit`,
fail gracefully with a user-facing status ("This document is read-only") and do
NOT open the Save-As prompt and do NOT write anything. `Workspace::saveTo`
already refuses non-text content (`src/Workspace.cpp:446`); this adds the
symmetric refusal for read-only mode at the command boundary so the user sees a
clean message instead of a prompt. This guard is mode-driven and also hardens
save for the existing `ReadOnly`/`Diff` modes.

### Help content assembly

A `buildHelpDocument()` function (help-owned, e.g. `src/runtime/help.cpp`)
returns the tab's text as a single UTF-8 string, composed of:

1. **Compiled-in prose** (string constants): a short overview, how to move around,
   and mouse usage (click to place cursor, drag to select, click a URL, click
   tabs/footer). Static; lives next to the builder.
2. **Generated: keybindings.** Iterate the live `KeymapViewState.bindings`
   (from the runtime's keymap), format each with `KeyCodec::formatSequence`, and
   pair each command id with its summary from the command catalog. Grouped/sorted
   for readability (e.g. by command-owner or alphabetically by key). Because it
   reads the live keymap, a user's `keymap.bind` customizations show up in help.
3. **Generated: configuration help.** A pointer to `init.lua` configuration with
   the current theme/keymap facts. At minimum it lists the `theme.set` and
   `keymap.bind` entry points and references `doc/config.md`; it MAY embed the
   generated command reference (`renderCommandReference`) so the config section
   is never stale. (Reviewer: decide how much config detail is generated vs. a
   short pointer, to avoid duplicating `doc/config.md` wholesale.)

Content is assembled at open time and re-assembled on every `help.open`
(re-focus refreshes), so generated sections always reflect current state.

### The `help.open` command

Registered in the runtime command catalog following the existing pattern
(`src/runtime/presentation.cpp` registers `theme.set` etc.; help gets a small
`registerHelpCommands` or an entry in an existing registrar). Properties:

- Id `help.open`, a mutating command (it changes the open-tab set), owner e.g.
  `help-system`, summary "Open Help".
- Handler: `text = buildHelpDocument(runtime); openReadOnlyTab(TabKind::ReadOnlyOutput,
  "help:main", "Help", text);` inside a runtime transaction.
- Catalog wiring: the command must appear in the compiled catalog, so
  `doc/commands.md` regenerates (`SSG_UPDATE_DOCS=1 ./build/test_commands`) and
  the catalog-parity / required-command tests must include it. Whether it is
  Lua/init-script surfaced: NOT init-script (it is an interactive UI command, not
  a configuration action), so it does not need a `doc/config.md` mention.

### Default binding and footer hint

- **Default binding.** Add `bind(seq({"Alt+KeyH"}), "help.open", "*")` to the
  default keymap (`src/EditorRuntime.cpp`, alongside the other `Alt+Key*`
  binds). `Alt+H` is currently unbound (verified against the default keymap
  list). If a user rebinds or unbinds it, the footer hint follows (below).
- **Footer hint.** A persistent, right-aligned footer action showing the help
  key and label, e.g. `Alt+H  Help`, clickable to dispatch `help.open`. It is
  appended to `request.footerActions` in `shellView()`
  (`src/runtime/snapshot.cpp:152`), AFTER the transient status-queue actions, so
  transient status actions take precedence and the hint yields space when the
  footer is crowded (footer actions already right-pack and drop when they do not
  fit, `src/ShellState.cpp:544-556`).
  - The key label is derived at runtime: `KeymapMatcher::preferredBinding("help.open")`
    -> `formatSequence`. If `help.open` is unbound, the hint shows just `Help`
    (or is omitted -- reviewer's call).
  - **Clickability (must not bypass status-action freshness).** Status-queue
    footer actions are dispatched through `status.invoke_action` with a
    `StatusActionInvocation{statusId, actionId, generation}` payload
    (`src/runtime/presentation.cpp:224`, `src/StatusQueue.cpp:89`): the
    `generation` is a freshness gate that rejects a click on an action after the
    selected status entry changed. Making footer-action nodes carry a raw
    `commandId` that is dispatched directly would BYPASS that gate and could run
    a stale status action. Therefore: status-queue footer actions STAY on the
    `status.invoke_action` path unchanged. The help hint is a distinct, static,
    non-generational footer action; ONLY it gets direct-command clickability. Model
    this as a separate hint node (its own `ShellNodeKind` or an explicit
    `commandId` set only on the hint node), so hit-testing dispatches `help.open`
    for the hint while status actions continue through their invocation path with
    freshness intact. Do not widen all footer-action nodes to a raw command id.
  - **Display prettifying: DEFERRED.** `formatSequence` yields `Alt+KeyH`; the
    user wrote `Alt+H`. Stripping the `Key`/`Digit` prefix is presentation polish
    orthogonal to the help abstraction and would touch every keybinding surface;
    it is out of scope for this spec. The hint shows the canonical
    `formatSequence` output (`Alt+KeyH`) unless a separate, later spec introduces
    a shared keybinding-display policy.

## Ownership and relationships

- `EditorRuntime::Impl` owns the new `openReadOnlyTab` seam (it already owns
  `Workspace` and `TabManager`). No new ownership.
- `src/runtime/help.cpp` (new) owns `buildHelpDocument` and the `help.open`
  handler registration; it depends on the keymap view, the command catalog, and
  the `openReadOnlyTab` seam. It does not touch documents/tabs directly -- only
  through the seam -- so help has no privileged access.
- Rendering, mouse, save, and autosave are unchanged in ownership; help rides
  existing paths. The only rendering addition is the mode-driven read-only tab
  marker, owned where tab titles are composed.

## Invariants

- A `ReadOnly` (or any non-`Edit`) document never enters `autosaveCandidates`
  and never has a draft/scratch file. (Already true; a test pins it for the help
  tab specifically.)
- `Document::apply` is the ONLY edit path, so a read-only tab cannot be mutated
  by any command, ever. No feature may bypass it.
- `file.save` on a non-`Edit` active document fails gracefully (status message),
  never prompts Save-As, never writes a file.
- The footer help hint's key label always equals the current binding for
  `help.open` (or the hint degrades to label-only when unbound). It is never a
  hardcoded `Alt+H` string.
- Status-queue footer actions continue to dispatch through
  `status.invoke_action` with their generation freshness gate; the help hint's
  direct-command click path does not touch or bypass that gate.
- Content of a read-only tab changes ONLY by constructing a fresh document
  (remove+recreate); no code path mutates a `ReadOnly` document in place, so
  `Document::apply`'s read-only rejection has no bypass.
- Re-running `help.open` focuses the single existing help tab and refreshes its
  generated content (via remove+recreate); it never opens a second help tab.

## Considerations

- **No Markdown rendering.** Help content is Markdown SOURCE shown as plain text.
  This is deliberate scope control; a Markdown renderer is a separate, larger
  feature. URLs still linkify via the existing mechanism, which covers the main
  "mouse input on prose" need.
- **Generated config help vs. `doc/config.md` duplication.** The config section
  should point to `doc/config.md` and surface live facts (entry points, current
  keymap), not re-embed the whole config document, to avoid a second copy that
  drifts.
- **Footer crowding.** The hint is lowest-priority footer content; on a narrow
  terminal or during a status message it yields. It must never push out status
  actions or fields.
- **The read-only marker is mode-driven, not help-specific.** It also labels the
  existing binary/decode-failure `ReadOnly` tabs and live-diff `Diff` tabs
  consistently; confirm that is acceptable (it is an improvement, but it changes
  existing tab titles).

## Acceptance (Definition of Done)

- Observable: `help.open` (and `Alt+H`, and a footer-hint click) opens a Help
  tab showing the prose plus a keybindings section that reflects the live keymap
  (including a test that a custom `keymap.bind` appears in the generated help);
  running it again focuses the same tab and refreshes content rather than opening
  a second.
- Read-only: typing / delete / paste / an edit command on the help tab leaves the
  buffer unchanged (revision unchanged); `file.save` / `Alt+S` reports a
  read-only message and neither prompts Save-As nor writes any file; no
  scratch/draft file is ever created for the help document (asserted against the
  autosave-candidates set and the scratch root).
- Footer: the hint renders bottom-right with the key label derived from the
  binding; unbinding `help.open` removes the key from the hint; the hint yields
  space to status actions when the footer is crowded.
- Gates: `bash scripts/check.sh` green (0 warnings, all tests); `doc/commands.md`
  regenerated with `help.open`; catalog-parity/required-command tests updated.
- Docs: `doc/config.md` (if help.open is mentioned anywhere users configure) and
  a short mention of the help command where user-facing commands are documented;
  a feature doc `doc/features/help.md` describing the read-only-tab seam and the
  content sections.

## Plan

| # | Step | Files | Gate |
|---|------|-------|------|
| 1 | Add the `openReadOnlyTab` seam on `EditorRuntime::Impl` (open ReadOnly virtual doc + ReadOnlyOutput tab + activate; refresh by remove+recreate keyed on content identity, mirroring live-diff) | `src/runtime/editor_runtime_internal.h`, `src/EditorRuntime.cpp` | unit test: opens one tab, second call refreshes via remove+recreate not duplicate; doc is ReadOnly + untitled + not an autosave candidate; no in-place edit path exists |
| 2 | Per-mode read-only tab marker in tab-title composition (ReadOnly gets `(readonly)`; Diff keeps liveDiffPrefix; Edit unchanged) | wherever `composedTabTitle` lives, `src/*` | test: a ReadOnly tab's title carries the marker; a Diff tab keeps its diff affordance; Edit tabs unchanged |
| 3 | `file.save` guard: non-`Edit` active document fails gracefully, no Save-As, no write | `src/runtime/files.cpp` | test: save on a read-only tab returns failure/status, opens no prompt, writes nothing |
| 4 | `buildHelpDocument` (static prose + generated keybindings from live keymap + config pointer) | `src/runtime/help.cpp` (new), `include/ssg/*` as needed | test: output contains a known prose anchor and a live binding line; a custom bind appears |
| 5 | Register `help.open` command; regenerate `doc/commands.md`; update catalog-parity/required-command tests | `src/runtime/help.cpp` or `presentation.cpp`, `doc/commands.md`, `tests/*` | test_commands + catalog-parity green |
| 6 | Default `Alt+KeyH -> help.open` binding | `src/EditorRuntime.cpp` | test: default keymap binds help.open |
| 7 | Footer hint: right-aligned, key label from `preferredBinding`+`formatSequence`; clickable via a hint-only direct-command node (dedicated node/commandId set only on the hint), leaving status-queue actions on the `status.invoke_action` freshness path unchanged. Includes the `HitTester`/pointer-routing seam that dispatches the hint click | `src/runtime/snapshot.cpp`, `src/ShellState.cpp`, `src/HitTester.cpp` | test: hint present with derived label; unbinding removes label; hint click dispatches help.open; a status action's generation-freshness rejection still holds |
| 8 | Docs: `doc/features/help.md`; user-facing command mention | `doc/features/help.md`, `doc/*` | doc tests green |
| 9 | Full gate + code review | - | check.sh green |
