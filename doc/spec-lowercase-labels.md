# Spec: normalize UI chrome labels to lowercase

## Problem

SSG's UI chrome labels are Title-Cased ("Files", "Git", "Symbols", "Help",
"Find", "Replace", "Settings", "Path", "Branch", "Follow edits", "Use disk",
"Dismiss", "Diff"). The desired house style is lowercase chrome ("files",
"git", "help", …), matching the already-lowercase footer follow-mode text and
the general terminal-native aesthetic. Document/tab filenames and document
content are NOT chrome and must keep their original casing.

## Goal

Every user-visible UI **chrome** label renders lowercase. The change is made
at the source of each label (so "the label is lowercase" is a real, greppable
fact), not as a render-time `toLower` transform that would leave the source
Title-Cased and hide the change from anyone reading the code.

## Non-goals

- Do not lowercase document tab titles (filenames), document content, path
  breadcrumbs' path text, git branch *values*, or help-body prose. Only the
  fixed chrome captions.
- No new settings or runtime toggles; the casing is fixed.
- No functional behavior change.

## Label inventory (source of truth)

Pure-display chrome labels (lowercase at source):
- `data/ui/status_fields.json`: `accessible_label` "Path"→"path",
  "Branch"→"branch", "Follow edits"→"follow edits", "Status"→"status" (audit
  the file for all `accessible_label` values in header/footer regions).
- `src/runtime/help.cpp`: the "Help" tab title → "help".
- `src/runtime/snapshot.cpp`: the "Help" footer hint → "help"; draft-notice
  actions "Diff"→"diff", "Use disk"→"use disk", "Dismiss"→"dismiss".
- `src/runtime/editing.cpp`: prompt titles "Find"→"find", "Replace"→"replace".
- `src/runtime/presentation.cpp`: "Settings"→"settings" prompt title;
  "Dismiss"→"dismiss" status action.
- `src/runtime/language_services.cpp`: "Dismiss" completion/hover labels →
  "dismiss".

Prompt captions and controls (lowercase at source) — these are the fixed
prompt chrome the prompt surface renders, and were the biggest gap:
- `src/FileCommands.cpp` path-prompt captions: "Open file"→"open file",
  "Open directory"→"open directory", "Save file as"→"save file as",
  "Rename file"→"rename file", "New directory path"→"new directory path".
- `src/runtime/navigation.cpp` goto-line prompt: title "Go to line"→"go to
  line", control label "Line number"→"line number".
- `src/runtime/editing.cpp` find/replace prompt: title "Find"→"find"; controls
  "Find query"→"find query", "Replace with"→"replace with", "Case"→"case",
  "Word"→"word", "Regex"→"regex", match-count label "Match count"→"match
  count".
- `src/runtime/presentation.cpp`: "Settings"→"settings" prompt title.
- Audit `src/FileCommands.cpp`, `src/runtime/editing.cpp`,
  `src/runtime/navigation.cpp`, and `src/runtime/presentation.cpp` for any
  further prompt title / `PromptControl` / `PromptMatchCount` label literal and
  lowercase it; the render seam is `src/Renderer.cpp`
  (`control.accessibleLabel + promptLabelSeparator + value`).

Double-use labels — **provider identity AND display** (see Design decision):
- `src/EditorRuntime.cpp`: `shell{{"Files", "Git", "Symbols"}}`.
- Compared as identifiers in `src/runtime/navigation.cpp`
  (`panelProviderTreeId`: `label == "Files"` → `TreeProviderId{"filesystem"}`,
  "Git"→"git", "Symbols"→"symbols") and `src/runtime/presentation.cpp`
  (`label == "Files"/"Git"/"Symbols"`, `showPanelProvider("Files"/"Git")`).
- `ShellState::activePanelProvider()` returns this label; the snapshot
  publishes it as the panel provider label; it is asserted across tests
  (test_ui_layout, test_hit_test, test_runtime_presentation, test_tabs,
  test_tui_fixture, session_snapshot_builder) and encoded in protocol/layout
  goldens.

## Design decision: provider identity vs. display label

The provider labels "Files"/"Git"/"Symbols" today serve double duty as both
the rendered caption and the lookup key used to resolve the provider. Two ways
to make them render lowercase:

**Option A (chosen): lowercase the label at its source and update the handful
of `==` comparison sites and tests to the lowercase spelling.** The label stays
the identity, just lowercase everywhere. Fully greppable; no hidden
display/identity split; smallest change that keeps the invariant true in
source. Ripple: `EditorRuntime.cpp` seed list, `navigation.cpp` (3
comparisons), `presentation.cpp` (5 comparisons/calls), and every test/golden
that spells the provider label — all mechanically renamed "Files"→"files",
"Git"→"git", "Symbols"→"symbols". `TreeProviderId` values ("filesystem",
"git", "symbols") are internal and unchanged.

**Option B (rejected for now): decouple identity from display** — give each
provider a stable id plus a separate display label, lowercasing only the label.
This removes the pre-existing label==identity coupling but is a broader
ShellState refactor touching the provider model and all its callers. Recorded
as a follow-up if the reviewer deems the coupling worth breaking now; this spec
does not adopt it to keep the cosmetic change proportionate.

The label==identity coupling is a pre-existing smell; Option A does not worsen
it (the string is still both), it only recases it. Reviewer: confirm Option A
is acceptable, or direct Option B.

## Oracles / tests

- Update the existing label assertions to the lowercase spellings
  (test_ui_layout, test_hit_test, test_runtime_presentation, test_prompt_status
  where it asserts chrome titles, test_tabs, test_tui_fixture,
  session_snapshot_builder).
- **Comprehensive seam-level lowercase oracle (closes the coverage gap).** A
  single helper `assertAllChromeLabelsAreLowercase(snapshot)` collects every
  chrome-label string a snapshot exposes and asserts each equals its
  lowercase form (comparing only cased letters, so digits/spaces/punctuation
  are ignored):
  - the panel provider label (`activePanelProvider` / published provider
    label);
  - every header/footer status field label;
  - every footer hint label;
  - draft-notice action labels;
  - when a prompt is active: the prompt title, every `PromptControl`
    `accessibleLabel`, and the `PromptMatchCount` label.
  It explicitly EXCLUDES document tab titles (filenames) and value fields.
  Drive it end-to-end across the surfaces that carry chrome by taking a
  snapshot after opening each: the panel (Files/Git/Symbols), the Help tab, the
  Find prompt, the Replace prompt, the Settings prompt, the goto-line prompt,
  and each file path prompt (open/save-as/rename). Each snapshot is asserted by
  the helper, so any Title-Cased label at any covered seam fails green.
- Add the lowercase-invariant provider assertion: iterate the seeded shell
  providers, assert `label == toLower(label)` and `panelProviderTreeId(label)`
  is non-empty (guards against a future Title-Cased provider slipping in).
- Regenerate protocol fixtures, ui_layout golden, and any doc goldens.

## Plan

1. Add this spec; review; fold findings (esp. the Option A/B decision).
2. Lowercase pure-display labels at their sources (JSON, help.cpp, snapshot.cpp,
   editing.cpp, presentation.cpp, language_services.cpp, FileCommands.cpp,
   navigation.cpp) — including all prompt titles/controls/match-count labels.
3. Lowercase the provider seed list and update navigation/presentation
   comparisons to the lowercase spelling.
4. Update all test assertions and regenerate goldens.
5. Add the lowercase-invariant provider assertion.
6. Gate (`bash scripts/check.sh`); visual signoff (chrome is visual); code
   review; fold; merge.
