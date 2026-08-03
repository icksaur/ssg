# spec-prune-theme-roles

> **Status: implemented.** All six plan steps landed; the dead-color-role guard
> test (`everyNonCaretSemanticRoleIsColorConsumedByTheRenderer` in
> `tests/test_render.cpp`) is the standing invariant.

Prune the semantic roles that are assigned a color in the built-in theme but
never read by any renderer, and rename the two ambiguously-named catch-all
roles. Follows `doc/spec-semantic-color-themes.md` (the direct per-role color
model); this is a cleanup of that role set, not a model change.

## Goals

A `theme.set` name must correspond to something that actually paints. Today 10
role names accept a color and change nothing on screen -- a silent pit of
failure. Remove them. Separately, `foreground`/`background` are the two
heaviest-used roles but their names do not say what they color; rename them to
`text`/`canvas`.

## Motivation (why these ten)

Traced every `SemanticRole` from assignment in `src/DefaultTheme.cpp` to
consumption in `src/Renderer.cpp`/`src/ShellState.cpp`:

- `DiagnosticError`, `DiagnosticWarning`, `DiagnosticInfo`, `DiagnosticHint` --
  no diagnostics rendering is wired anywhere; nothing reads them.
- `GitAdded`, `GitModified`, `GitDeleted`, `GitConflict` -- the diff overlay
  reads the separate `DiffAdded`/`DiffRemoved`/`DiffModified` roles; nothing
  reads the `Git*` roles.
- `StatusError` -- only `StatusInfo` and `StatusWarning` are consumed.
- `ActiveLineNumber` -- the gutter only ever reads `LineNumber`; there is no
  active-line distinction.

Setting any of these in a `theme.set` table succeeds and paints nothing.

## Design

### Prune

Remove these 10 enumerators from `SemanticRole` (`include/ssg/Theme.h`), their
entries in `kAllSemanticRoles` and `src/Theme.cpp`'s `kSemanticNames`, and their
`role(...)` assignments in `src/DefaultTheme.cpp`. `kSemanticRoleCount` drops
from 35 to 25 (the `static_assert` on `kAllSemanticRoles.size()` pins it).

`kThemeColorSlotCount` becomes 25 + 11 = 36; the render color table and every
`syntaxIndex` offset (`kSemanticRoleCount + position(scope)`) follow
automatically because they are expressed in terms of `kSemanticRoleCount`.

### Keep `Caret`

`Caret` is NOT pruned. Although its *color* is no longer read (the primary caret
is the terminal hardware cursor and the secondary caret inverts the underlying
cell), `SemanticRole::Caret` is still used as a cell role TAG
(`src/Renderer.cpp` marks secondary-caret cells with it; `tests/test_render.cpp`
and `doc/spec-ux.md` rely on that tag). It keeps its slot and its default color
(harmless, unread). This is the one role that is a live tag but a dead color;
call it out rather than remove it and break the tag.

### Rename

`SemanticRole::Foreground` -> `SemanticRole::Text`, name string
`"foreground"` -> `"text"`. `SemanticRole::Background` ->
`SemanticRole::Canvas`, name string `"background"` -> `"canvas"`. These are the
default document text color and the editor/document canvas fill respectively.
Update every call site in `src/`, `apps/`, and `tests/`, plus the built-in
theme assignment and the docs. No backward-compatible alias: `theme.set` is new
enough (shipped this cycle, unreleased) that an old name is a plain error, which
is the pit-of-success behavior for an unknown name.

## Invariants

- Every `SemanticRole` after this change is read by a renderer for either its
  color OR (for `Caret` alone) its cell tag. No role both accepts a `theme.set`
  color and paints nothing. The color half is enforced by the guard test below.
- **Appearance is unchanged.** The pruned roles painted nothing, and the renamed
  roles keep their exact default colors (`text` = old `foreground` = tone 1;
  `canvas` = old `background` = tone 0). The rendered default screen is
  pixel-identical to before.
- **Protocol fixtures are intentionally updated** (not "unchanged"): shrinking
  `kSemanticRoleCount` changes the serialized `role_colors` array length, so the
  committed `session_snapshot.hex`/`session_delta.hex` are deliberately
  regenerated. This is a byte change to the fixtures, distinct from the
  appearance-parity invariant above.
- **No cross-version wire compatibility.** The `ThemeSnapshot` codec is
  same-version only; a shorter `role_colors` array is a breaking wire change and
  that is acceptable -- SSG ships client and server together, there is no
  persisted or cross-version snapshot to decode. Do not infer additive/forward
  compatibility from "goldens regenerate."
- I22 (color authority) is untouched: `DefaultTheme.cpp` remains the one literal
  home.

## Acceptance (Definition of Done)

- Observable: `theme.set` rejects `foreground`, `background`, and each of the 10
  pruned names as unknown; accepts `text` and `canvas`; the rendered default
  screen is pixel-identical to before (chrome band, canvas, text, diff,
  selection all unchanged).
- **Dead-color-role guard test** (the load-bearing oracle -- must prove
  role-name surface == actually-color-consumed surface, with `Caret` the sole
  exception, without being a tautology). Mechanism:
  1. Build a `ThemeSnapshot` whose every role gets a DISTINCT sentinel color
     (e.g. role at index `i` -> `SrgbColor{i, 0, 0}`), so a color uniquely
     identifies its role.
  2. Build a representative snapshot that activates every paint surface: an open
     document with a selection and a search match, a diff overlay (so
     `grid.diffTints` is populated), a file-tree panel, multiple tabs
     (active + inactive), header, footer, a scrollbar, and an open prompt.
     Coverage of surfaces is what gives the test its teeth; the test must fail
     to build/exercise a surface loudly rather than silently skip it.
  3. `render()` it, then collect the set of colors the renderer actually
     emitted: every cell's resolved foreground and background
     (`grid.colors[cell.foreground]` / `[cell.background]`), plus the six
     `grid.diffTints` colors and `grid.selectionFill` (the diff/selection washes
     travel there, not in a cell fg/bg slot).
  4. Assert that for EVERY `SemanticRole` except `Caret`, that role's sentinel
     color is in the emitted set. A role that accepts a `theme.set` color but
     paints nothing has its sentinel absent and fails the assertion. `Caret` is
     explicitly excluded with a comment stating it is a live cell-role TAG whose
     color is intentionally unread.
  This is not a tautology: it renders through the real paint paths and checks
  observed output, so adding a future dead color role (or dropping a surface
  from coverage) breaks it.
- Gates: `bash scripts/check.sh` green (0 warnings, all tests), protocol goldens
  regenerated for the shrunk `roleColors` array.
- Docs: `doc/config.md` role-name list updated (drop the 10, rename the 2);
  `doc/spec-color.md` role-count references updated; `doc/features/theme.md`
  example names updated if they use a pruned/renamed name.

## Plan

Rename blast radius (for a fresh implementer): `Foreground`/`Background` are
encoded not only in `src/Renderer.cpp`, `src/ShellState.cpp`, and `apps/`, but
also in test SNAPSHOT BUILDERS and fixtures that hand-construct roles --
`tests/session_snapshot_builder.h`, `tests/test_end_to_end.cpp`,
`tests/test_tui_fixture.cpp`, `tests/test_render.cpp`,
`tests/test_renderer_diff_overlay.cpp`, and `tests/runtime/test_runtime_presentation.cpp`
all reference `SemanticRole::Background` or `kSemanticRoleCount` directly. Grep
`SemanticRole::Foreground|SemanticRole::Background` across `src/`, `apps/`, and
`tests/` before declaring step 2/3 done.

| # | Step | Files | Gate |
|---|------|-------|------|
| 1 | Remove 10 enumerators + names + default assignments; rename Foreground/Background to Text/Canvas across enum, kAllSemanticRoles, name table, DefaultTheme | include/ssg/Theme.h, src/Theme.cpp, src/DefaultTheme.cpp | compiles |
| 2 | Update all consumers to the renamed roles (grep-driven, not just the obvious files) | src/Renderer.cpp, src/ShellState.cpp, apps/* | ssg + ssg_app build green |
| 3 | Fix tests + snapshot builders referencing renamed roles; add the dead-color-role guard test | tests/*, tests/session_snapshot_builder.h | test build green |
| 4 | Regenerate protocol goldens | tests/fixtures/protocol/* | test_protocol green |
| 5 | Docs: config.md, spec-color.md, features/theme.md | doc/* | config-doc + commands tests green |
| 6 | Full gate + code review | - | check.sh green |
