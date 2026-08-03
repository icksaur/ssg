# spec-help

The in-editor help page and the read-only in-memory tab primitive it is built
on. Implements `doc/spec-help-system.md`.

## Goals

`help.open` opens a read-only, in-memory tab of generated help: compiled-in
prose plus the live keybinding list and configuration help. The tab cannot be
edited or saved and never persists a draft. A persistent `Alt+H  Help` hint sits
at the bottom-right of the footer.

## Design

### The read-only in-memory tab primitive

`EditorRuntime::Impl::openReadOnlyTab(kind, contentIdentity, label, text)` is the
reusable seam (help is its first caller). It opens `text` as a
`DocumentMode::ReadOnly` virtual document (untitled -> excluded from autosave,
cannot be saved) in a `TabKind::ReadOnlyOutput` tab deduped by `contentIdentity`,
and activates it. The backing document is tracked in the
`readOnlyTabDocuments` side map (mirroring `liveDiffDocuments`), so
`activeDocumentId()` resolves a read-only-output tab to its document.

Refresh is by REMOVE+RECREATE: re-opening the same identity drops the existing
document and constructs a fresh one with the new text. A `ReadOnly` document
rejects `Document::apply`, so there is deliberately no in-place text setter --
rebuilding is the only path, and it keeps the read-only edit invariant with no
backdoor. Closing a `ReadOnlyOutput` tab drops its document and map entry
directly, skipping the recovery/scratch journaling entirely (the content is
ephemeral and regenerable).

Reused unchanged: `Document::apply`'s `ReadOnly` rejection is the single edit
chokepoint, so no command can mutate the tab; `autosaveCandidates()` already
excludes any non-`Edit` document, so the help document is never drafted.

### Read-only affordance

`composedTabTitle` applies a per-mode marker: a `ReadOnly` tab gets the
`style.tab.readOnlySuffix` (default `" (readonly)"`, restylable via
`style.define`'s `tab_read_only_suffix`); a `LiveDiff` tab keeps its
`liveDiffPrefix`; an editable tab is unadorned. `file.save` on any non-`Edit`
active document fails gracefully ("this document is read-only and cannot be
saved") before the untitled -> Save-As redirect, so a help tab reports a clean
message rather than opening a naming prompt or writing.

### Help content

`buildHelpDocument(runtime)` (`src/runtime/help.cpp`) assembles the tab text on
every open: compiled-in prose (overview, movement, mouse) + the live keybinding
table (formatted from `runtime.keymap` via `KeyCodec::formatSequence`, labelled
from the command catalog, so a user's `keymap.bind` customizations appear) +
configuration help + the generated command reference
(`renderCommandReference`). Content is Markdown SOURCE shown as plain text (no
Markdown renderer); URLs in it still linkify through the document's existing
URL-run detection.

### `help.open` and the binding

`help.open` is a `.lua()` mutating command registered in `bindRuntimeHelp`. The
default keymap binds `Alt+KeyH -> help.open`. Its display form is `Alt+H`
(`formatSequence` already strips the `Key` prefix).

### Footer hint

`shellView()` populates `ShellLayoutRequest::footerHint` with a `ShellFooterHint`
whose label is the live `help.open` binding + `"  Help"` (label-only when
unbound) and whose `commandId` is `help.open`. `computeShellLayout` places it as
a `ShellNodeKind::FooterHint` node packed to the LEFT of the status-queue
actions, so status actions stay rightmost and the hint yields (drops) first when
the footer is crowded; in the common case (no status actions) the hint is the
rightmost footer element. `HitTester` dispatches a click on the hint node
directly as its command id -- a path DISTINCT from `FooterAction`/
`status.invoke_action`, so status-action generation-freshness is never bypassed.

## Invariants

- `Document::apply` is the only edit path; a `ReadOnly` help tab cannot be
  mutated by any command.
- A non-`Edit` document is never an autosave candidate and never persists a
  draft.
- `file.save` on a non-`Edit` active document fails gracefully, never prompts
  Save-As, never writes.
- Read-only-tab content changes only by remove+recreate; nothing edits a
  `ReadOnly` document in place.
- Re-running `help.open` focuses the single existing help tab and refreshes its
  content; it never opens a second help tab.
- The footer hint's key label always equals the live `help.open` binding (or
  label-only when unbound); it is never a hardcoded key string.

## Acceptance (Definition of Done)

Covered by `tests/runtime/test_help.cpp`: opens a `ReadOnlyOutput`/`ReadOnly`
"Help" tab; the document has the prose, the live default binding, and a custom
`keymap.bind`; re-open is idempotent and refreshes; edits are rejected leaving
the buffer unchanged; `file.save` refuses without a prompt; no draft is
persisted; the tab title carries the read-only marker; the footer hint shows the
live key and yields to label-only when unbound. Gate: `bash scripts/check.sh`
green; `doc/commands.md` regenerated with `help.open`; protocol goldens
regenerated for the added `Style` glyph and `ShellNodeKind::FooterHint`.
