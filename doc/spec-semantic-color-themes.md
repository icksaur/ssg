# spec-semantic-color-themes

Status: DRAFT (spec only — review before implementing)

Supersedes the palette-indirection model in `doc/spec-color.md` and retires
`doc/spec-background-tint-adjust.md` (the HSV dimming it specifies is no longer
needed). `doc/spec-color.md`'s color-DEPTH adaptation half (`ColorDepth`,
`resolveColor`, `src/color.cpp`) is unchanged and still authoritative.

## Goals

Editing a theme is direct: every UI semantic role, every syntax scope, and every
highlight has its **own** color, set by name from `init.lua`. Changing the
header background, or the active-tab background, or one syntax color, touches
exactly that one thing and nothing else. Arbitrary shades/tints for all UI are
possible without HSV dimming or a shared 16-slot palette. A user who WANTS
16-color discipline achieves it themselves (define 16 Lua locals, reuse them).

After this change: no `semanticIndices`/`syntaxIndices` indirection, no
`kThemePaletteSize` cap, no `DiffTints`/`selectionFill` derivation, no
`BackgroundTintAdjustments`, no co-visible-distinctness rejection. One flat map
from name → sRGB color is the whole theme.

## Design

### The theme is a direct name→color map

`ThemeSnapshot` becomes:

- `roleColors : array<SrgbColor, kSemanticRoleCount>` — one color per
  `SemanticRole`, indexed by the enum. A role is a single color; whether it is
  used as foreground or background is the render site's existing choice (e.g.
  `StatusWarning` is a bg for the notice bar, a fg for a status field).
- `syntaxColors : array<SrgbColor, kSyntaxScopeCount>` — one color per
  `SyntaxScope`.

Removed from `ThemeSnapshot`: `palette[16]`, `semanticIndices`, `syntaxIndices`,
`diffTints`, `selectionFill`, `backgroundTints`.

`defaultTheme()` (`src/DefaultTheme.cpp`) stays the single compiled-in literal
theme: it now assigns an sRGB color per role and per scope directly, seeded from
the current palette+index values so the shipped appearance is unchanged (each
`role(X, i)` becomes `role(X, palette[i])`).

### Diff washes and selection are plain roles

The derived washes disappear; the renderer reads role colors directly:

- Diff added/removed/modified row **and** word washes = the `DiffAdded` /
  `DiffRemoved` / `DiffModified` role colors. (The old row/word split and the
  `modifiedWord`-reuses-added rule were artifacts of derivation; a theme that
  wants a distinct modified-word color can be given one later by adding a role,
  not by deriving.)
- Selection fill = the `Selection` role color.

`deriveDiffTints`, `deriveSelectionFill`, `DiffTints`, `TintAdjustment`,
`BackgroundTintAdjustments`, `BackgroundTintTarget`, and `theme.background` are
deleted. `spec-background-tint-adjust.md` is archived.

### Cells index a per-role color table (mechanism decision)

Today `CellGridCell` carries `uint8 foreground/background` palette indices, and
`CellGrid` carries the 16-color `palette`; the terminal client resolves
`palette[index] → resolveColor(depth)`.

**Chosen:** the theme is a direct per-role/per-scope color map
(`roleColors`/`syntaxColors`), and `CellGrid` carries a flat `colors` table that
is exactly those role colors followed by the scope colors (size
`kSemanticRoleCount + kSyntaxScopeCount`). A cell still stores a compact `uint8`
index, but the index space is now one-slot-per-role/scope, so the painful shared
16-slot indirection is gone: every role is independently colorable and
`theme.set` writes `roleColors[role]` directly. `semanticIndex(theme,role)` /
`syntaxIndex(theme,scope)` keep their `uint8` return type — they become the
identity/offset into `colors` — so the renderer's ~35 call sites,
`put`/`fillRect`/`paintText`, and the client's index-lookup path are UNCHANGED
in shape; only the table they index into changes size and meaning.

Rejected alternative: widen every `CellGridCell` to two `SrgbColor`s and drop
the index entirely. It is marginally purer but cascades a type change through
~28 files (every cell read, the client emit path, and every test asserting a
cell index) for no user-visible difference — the remaining index is an internal
render detail, not the theme-editing indirection the user objected to. The
smaller change is preferred; if a future need arises for a cell to carry a color
with no role identity, revisit.

Wire/perf: the render grid is produced CLIENT-SIDE (`ssg::Renderer{}.render` in
`apps/ssg_main.cpp`); `CellGrid`/`CellGridCell` are terminal-rendering internals
and are NOT protocol types. The only protocol impact is `ThemeSnapshot` losing
its palette + index arrays and gaining `roleColors`/`syntaxColors`; the client's
`CellGrid.palette` becomes `CellGrid.colors`.

### Lua API

Retire `theme.define` (16 ANSI-slot names) and `theme.background`. Add ONE
command:

- `theme.set{ <name> = "#rrggbb", ... }` — set any semantic-role or syntax-scope
  color by its snake_case name (the existing `semanticRoleName` /
  `syntaxScopeName` strings, e.g. `header_background`, `tab_active`,
  `diff_added`, `comment`, `keyword`). Partial table (omitted names keep their
  current color), validated whole, applied all-or-nothing: an unknown name or
  malformed `#rrggbb` rejects the entire call with no partial mutation. No
  derivation runs — the set color is the final color.

Name collisions between the role and scope namespaces: none today (role names
and scope names are disjoint sets); the validator rejects a name found in
neither. If a future name appears in both, the spec must disambiguate then.

`ThemeSetArguments{ std::unordered_map<std::string,std::string> colors }`
replaces `ThemeDefineArguments`/`ThemeBackgroundArguments`. `applyThemeSet`
replaces `applyThemeDefine`/`applyThemeBackground`.

**`init.lua` dispatch is hard-coded, not registry-driven.** `ScriptHost`
(`src/ScriptHost.cpp:104-121`) recognizes a fixed set of command ids
(`theme.define`, `theme.background`, `style.define`, `keymap.bind`, ...) and
rejects anything else — runtime command registration alone does NOT make a
command callable from init.lua. So the `ScriptHost` dispatch block MUST be
edited: remove the `theme.define`/`theme.background` branches and add a
`theme.set` branch that forwards `ThemeSetArguments{*invocation.arguments}`.
The config-doc coverage test (`tests/test_config_doc.cpp`) also requires every
init-script command to appear in `doc/config.md`, so `theme.set` must be
documented there.

## Invariants

- **I22 (color authority)** — `Theme` is still the only source of color; clients
  consume its colors and never mint one. `src/DefaultTheme.cpp` remains the one
  compiled-in literal home; `tests/test_theme.cpp`'s
  `sourceAndConfigHaveNoIndependentColorSources` scan still guards every other
  file. This invariant is UNCHANGED and load-bearing.
- **Exhaustive coverage** — every `SemanticRole` and every `SyntaxScope` resolves
  to exactly one color; a role/scope added without a color fails to compile
  (array size tied to the `kSemanticRoleCount`/`kSyntaxScopeCount` `static_assert`s)
  or fails a completeness test. Replaces old I8 (palette cardinality 16), which
  is RETIRED.
- **Depth adaptation unchanged** — `resolveColor` still maps any theme color to
  the terminal's depth; it operates on `SrgbColor`, so it is agnostic to this
  change.

Retired invariants: I8 (exactly 16 indexed colors); co-visible-role
distinctness (`kCoVisibleRolePairs` — a theme may now make any two roles equal,
the user's call); derived-tint purity.

**Retiring `kCoVisibleRolePairs` requires test-fixture migration, not just
deletion.** The gate enforces it directly: `tests/test_theme.cpp:166-209`
(`sharedFixtureRolesAreDistinctForEveryTheme`) reads a shared theme-roles
fixture (`SSG_THEME_ROLES_PATH`), reconstructs the expected co-visible classes,
asserts parity with `kCoVisibleRolePairs`, and asserts a colliding theme is
rejected by the `Theme` constructor. Removing the constraint MUST also remove
that test (and the fixture's `co_visible_role_classes` coupling) and drop the
constructor's rejection path, or the build fails. This is an explicit Plan step,
not a side effect.

## Considerations

- **Co-visible safety net loss.** Removing `kCoVisibleRolePairs` means a theme
  can make caret==selection or tab-active==tab-inactive and lose a distinction.
  This is the accepted cost of direct control; document it, do not re-add a
  constraint. (A future optional lint could warn, out of scope.)
- **Wire size.** Per-cell color triples ~3× the per-cell color bytes. Acceptable:
  grids are viewport-sized (tens of KB), deltas already diff per-cell, and the
  snapshot sheds the palette+indices. Confirm the protocol golden regen and that
  no test asserts a fixed cell byte width.
- **Every color call site.** `semanticIndex()`/`syntaxIndex()` and every
  `uint8` color parameter in `src/Renderer.cpp` (and any other consumer:
  `ShellState` diff overlay, `Viewport`, `Selection`) must move to `SrgbColor`.
  This is a wide but mechanical sweep; the render golden/snapshot tests are the
  oracle that appearance is preserved.
- **Migration of the derived washes.** The current diff overlay applies
  `DiffTints` (row vs word) and a per-cell `DiffTint` enum. Collapsing to the
  three `Diff*` role colors must reproduce today's appearance for the shipped
  theme — pin with the existing diff-overlay render test before deleting the
  derivation.
- **`init.lua` compat break.** Existing `theme.define{...}`/`theme.background{...}`
  scripts stop working. This is a deliberate, documented break (the feature is
  new enough to have few users); `doc/config.md` and any sample init.lua must be
  updated. No silent alias.

## Risks and Mitigations

- **Appearance regression during the sweep** → land the direct-color model with
  `defaultTheme()` seeded from today's exact values FIRST, gated by the render
  golden tests (byte-identical grids), before touching the Lua API.
- **Missed color source** → the I22 scan already fails the build on a stray
  literal; keep it green throughout.
- **Protocol drift** → regenerate `session_snapshot.hex`/`session_delta.hex`;
  a round-trip test proves encode/decode of the new `ThemeSnapshot` + cells.

## Acceptance (Definition of Done)

- Observable (needs signoff): the shipped default theme looks identical
  before/after the model swap (render a representative frame, diff the grid). A
  user can then, from init.lua, set `header_background`, `tab_active`, and
  `comment` to three arbitrary distinct colors and see exactly those three change
  with nothing else affected.
- Budgets: no per-frame render-time regression beyond the in-memory color-type
  widening (client-side render only; the grid does not cross the wire); n/a
  otherwise.
- Gates: `bash scripts/check.sh` green (0 warnings), `SSG_TREESITTER` ON and OFF.
- Oracles (write before code):
  - Default-theme parity: a render of a fixed snapshot produces a grid whose
    per-cell resolved colors are byte-identical to the pre-refactor grid
    (golden). Master safety net for the UI-chrome sweep.
  - **Diff-overlay parity (separate, mandatory)**: a render of a snapshot with a
    live diff (added/removed/modified rows AND inline word marks) is byte-identical
    before/after. The current diff path uses three channels — the per-cell
    `DiffTint` enum, `CellGrid.diffTints` (row vs word, `modifiedWord`-reuses-added),
    and `selectionFill` — with precedence in `src/Renderer.cpp:859-919` and
    `apps/ssg_terminal.cpp:347-352`. The generic frame golden does NOT exercise
    these, so collapsing the washes to the `Diff*`/`Selection` role colors MUST be
    pinned by this diff-specific golden before any derivation is deleted; a wash
    regression can otherwise pass the generic parity oracle.
  - Completeness: every `SemanticRole` and every `SyntaxScope` has a color in
    `defaultTheme()` (no default-constructed black hole).
  - `theme.set` direct application: setting `header_background="#123456"` makes
    exactly the header band cells resolve to `#123456` and leaves every other
    role's color unchanged (per-role isolation — the property the old shared
    palette violated).
  - `theme.set` all-or-nothing: an unknown name or malformed hex rejects the
    whole call and mutates nothing (round-trip the snapshot equals the prior).
  - `theme.set` from init.lua: a `ScriptHost` test proves an init-script
    `theme.set{...}` invocation dispatches (not "unknown script command").
  - I22 scan stays green: no new independent color source outside the exempt set.
  - Protocol round-trip: encode→decode of the direct-color `ThemeSnapshot`
    equals the original; canonical hex goldens regenerated. (Cells are not
    protocol types; no cell codec.)

## Plan

| # | Step | Files | Oracle | Invariants |
|---|------|-------|--------|------------|
| 1 | Pin TWO render goldens BEFORE any change: (a) a generic frame, (b) a diff-overlay frame (added/removed/modified rows + inline word marks + a selection), each a grid of resolved colors | tests/test_render.cpp, tests/test_renderer_diff_overlay.cpp, tests/fixtures/ | golden: pre-refactor generic + diff grids | I22 |
| 2 | Replace `ThemeSnapshot` internals with `roleColors`/`syntaxColors`; delete palette/indices/diffTints/selectionFill/backgroundTints; `Theme`/`Theme.h` API returns `SrgbColor` per role/scope; drop the co-visible rejection from the `Theme` constructor | include/ssg/Theme.h, src/Theme.cpp | completeness test | exhaustive coverage |
| 3 | Remove the co-visible constraint's tests + fixture coupling (`sharedFixtureRolesAreDistinctForEveryTheme`, `kCoVisibleRolePairs`, `co_visible_role_classes` in the theme-roles fixture) | tests/test_theme.cpp, tests/fixtures/ (theme roles), include/ssg/Theme.h | build green (constraint gone, no dangling refs) | co-visible retired |
| 4 | Reseed `defaultTheme()` to direct colors from today's palette+index values (byte-identical) | src/DefaultTheme.cpp | oracle #1a parity | I22 |
| 5 | Move cells + renderer to `SrgbColor`: `CellGridCell.fg/bg`, drop `CellGrid.palette`, `themeColor(role)` replaces `semanticIndex`, `put`/`fillRect`/`paintText` take `SrgbColor`; collapse diff washes to the `Diff*` roles and selection to `Selection` | include/ssg/Renderer.h, src/Renderer.cpp, src/ShellState.cpp, src/Viewport.cpp, src/Selection.cpp | oracle #1a + #1b parity (diff frame) | I22 |
| 6 | Update the client render→emit path to `resolveColor(cell.fg/bg, depth)` with no palette lookup | apps/ssg_main.cpp, apps/ssg_terminal.cpp | terminal-parity test; manual TUI signoff | depth adaptation |
| 7 | Protocol: encode/decode direct-color `ThemeSnapshot` (roleColors/syntaxColors); regenerate goldens. NOTE: cells are client-side internals, NOT protocol types — no cell codec | src/Protocol.cpp, tests/fixtures/protocol/*.hex, tests/test_protocol.cpp | ThemeSnapshot round-trip + golden | - |
| 8 | Replace runtime command: `theme.set` + `applyThemeSet`; retire `theme.define`/`theme.background` + their args/derivations | include/ssg/Theme.h, src/Theme.cpp, src/runtime/*.cpp (command reg) | per-role isolation; all-or-nothing | I22 |
| 9 | Wire `theme.set` into init-script dispatch: remove the `theme.define`/`theme.background` branches, add a `theme.set` branch forwarding `ThemeSetArguments` | src/ScriptHost.cpp | ScriptHost test: `theme.set{...}` dispatches | - |
| 10 | Docs: rewrite `spec-color.md` theme half; archive `spec-background-tint-adjust.md`; update `doc/config.md` (document `theme.set`, remove old commands) + sample init.lua; regen `doc/commands.md` | doc/spec-color.md, doc/spec-background-tint-adjust.md, doc/config.md, doc/commands.md | config-doc test green | DONE |

## Rationale (optional)

The 16-color palette was adopted to bound theme-design decisions, but the
indirection it forces — every UI element borrowing a shared slot, then HSV
dimming to un-share backgrounds from foregrounds — cost more than the cap saved
(see the chrome-background work: three new "*Background" roles just to stop the
active tab, inactive tab, and header sharing one slot). Direct per-role color is
the simpler model: the theme states each color, nothing is derived or searched,
and the "limit yourself to 16" discipline becomes a user choice expressed in
their own init.lua rather than a system-imposed constraint that leaks into every
color decision. The retirements in `spec-color.md`'s History section
(derived/muted tints, readability search, separate selection fill) were already
steps in this direction; this finishes it by removing the last derivation and
the index layer itself.
