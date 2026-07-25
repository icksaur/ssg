# spec-color

The document of record for color and theming. Supersedes
`spec-color-depth-defaults.md` and `spec-diff-tint-hue-fidelity.md`, and takes
ownership of the color sections that were duplicated in
`spec-syntax-and-diffs.md` and `spec-terminal-robustness.md`.

Everything below describes the code as it stands. Where an earlier design was
retired, it is named under History, because those retirements are the parts most
likely to be reintroduced by accident.

## Goals

One place that answers: where does a color come from, how is a background tint
derived, what may a user change, and what happens on a terminal that cannot
display it.

## Design

### The palette is the only place RGB exists

A theme is exactly 16 sRGB colors (`kThemePaletteSize`). Nothing else in the
system holds a color value:

- 32 `SemanticRole`s (`Foreground`, `Selection`, `GitAdded`, `TabActive`, ...)
  each map to a palette INDEX, in `semanticIndices`.
- 11 `SyntaxScope`s (`Comment`, `Keyword`, `String`, ...) each map to a palette
  INDEX, in `syntaxIndices`.
- `DiffTints` and `selectionFill` are DERIVED from the palette (below) and
  carried on the snapshot so clients never recompute them.

`ThemeSnapshot` is the whole of a theme: palette, the two index arrays, the
derived tints, and the selection fill. `defaultTheme()` in `src/DefaultTheme.cpp`
is the one compiled-in theme; there is no data-file theme path.

### Background tints are flat palette colors

Both derivations are pure functions of the palette and the index arrays, and
both are FLAT -- the anchor role's palette color is used directly:

- `deriveDiffTints` -> `addedRow`/`addedWord` = `GitAdded`,
  `removedRow`/`removedWord` = `GitDeleted`, `modifiedRow` = `GitModified`.
  `modifiedWord` reuses `GitAdded`: a word mark inside a modified line says
  "here specifically" within an already-tinted row, and the inserted span reads
  as added. There is no fourth "modified word" shade.
- `deriveSelectionFill` -> the `Selection` role's palette color.

Flatness is the load-bearing property, not an implementation detail. A selected
tab (`TabActive`), a focused tree row (`TreeFocus`), and a text selection all
resolve the same palette entry the same way, so all three render as the SAME
highlight. A derived-and-muted text selection is what made them visibly
mismatch before.

### Co-visible roles must stay distinguishable

`kCoVisibleRolePairs` lists role pairs that appear side by side (Caret vs
Selection, the four Diagnostic levels against each other, the four Git states,
TabActive vs TabInactive, and so on). A theme whose palette makes any listed
pair identical is rejected: those pairs carry meaning only by being told apart.

### Color depth is detected, then colors are adapted

`ColorDepth` is `Truecolor`, `Indexed256`, or `Ansi16`, chosen once at startup
by `detect_color_depth` (`apps/ssg_terminal.cpp`) in strict precedence:

1. `SSG_COLOR_DEPTH` -- `truecolor`/`24bit`, `256`/`256color`/`indexed256`,
   `16`/`ansi16`, case-insensitive. An unrecognized value falls through rather
   than failing.
2. `COLORTERM` of `truecolor`/`24bit` -> Truecolor.
3. `TERM` of `dumb`, or unset/empty -> Ansi16. A hard floor, checked BEFORE the
   allowlist.
4. A known-truecolor `TERM_PROGRAM`/`TERM` allowlist.
5. Otherwise Truecolor: effectively every active terminal supports it, and
   assuming less penalizes SSH sessions that do not advertise.

`resolveColor(color, depth)` (`src/color.cpp`) then maps a theme color to
something the terminal can show:

- Truecolor: identity.
- Indexed256: nearest of the xterm 6x6x6 cube (16..231) and the 24-step gray
  ramp (232..255) by squared Euclidean distance in sRGB. Indices 0..15 are
  EXCLUDED because a terminal may re-theme them, which would make the mapping
  non-deterministic.
- Ansi16: nearest of the 16 standard xterm base colors by the same metric.
  Those 16 are assumed values a terminal may re-theme; accepted limitation.

Ties break to the lowest index, so `resolveColor` is a pure function with one
answer. This is adaptation, not authorship: it never mints a color the theme did
not already specify, which is why it does not violate color authority.

### What a user can change

`theme.define` (init.lua) takes a table of the 16 classic ANSI slot names, each
optionally mapped to `"#rrggbb"`. Omitted names keep their current value, so the
table may be partial. It is validated whole and applied all-or-nothing: an
unknown slot name or malformed hex rejects the entire call with no partial
mutation. `DiffTints` and `selectionFill` are RE-DERIVED over the new palette on
every accepted call, so a redefined `GitAdded` immediately changes the add tint.

Role and syntax mappings are never touched by `theme.define`; only the 16 raw
colors are configurable. There is one active theme -- no named themes, no
switching.

## Invariants

- **I22 (color authority, `doc/spec.md`)** -- `Theme` is the only source of
  color values. Clients and extensions consume its 16 colors and semantic
  indices through the API; they never mint or substitute a color. A literal RGB
  constant used AS A FINAL COLOR is a violation; a numeric tuning parameter
  (a threshold, a multiplier) is not.
- **I8 (palette cardinality, `doc/spec.md`)** -- every accepted theme has
  exactly 16 indexed colors, and every visible semantic role resolves to one.
- Every `SemanticRole` and every `SyntaxScope` maps to exactly one palette index
  -- enforced exhaustively, so adding a role without mapping it fails.
- Derived values (`DiffTints`, `selectionFill`) are pure functions of the
  palette and index arrays. Same palette in, same tints out, on every client.

### How I22 is enforced

`tests/test_theme.cpp`'s `sourceAndConfigHaveNoIndependentColorSources` scans
the repository for independent color sources: hex literals, `rgb(`/`hsl(`,
`lighten|darken|shade|tint|blend|gradient(`, and `SrgbColor{`/`SrgbColor(`.
`src/Theme.cpp` additionally may not contain a bare RGB triple.

Exempt: `include/ssg/Theme.h`, `include/ssg/color.h`, `src/color.cpp`,
`tests/test_color.cpp`, `tests/test_theme.cpp`, plus `doc/`, `tasks/`,
`vendor/`, `.git/`, and `build*`.

`src/DefaultTheme.cpp` exists as its own translation unit for exactly this
reason: it holds the one compiled-in theme literal, and keeping it out of
`Theme.cpp` keeps that file's stricter no-literal rule intact.

**Consequence for future work:** new color math cannot live in `Theme.cpp` if it
needs literals or names a forbidden verb. It belongs in `src/color.cpp`, which
is the sanctioned home for color computation, or the scan's exemption list must
be extended deliberately.

## Considerations

- **Ansi16 collapses meaning.** At 16 colors, distinct tints can resolve to the
  same swatch, so guarantees about telling add from remove cannot hold. Accepted,
  and the same category of limitation as 16-color syntax highlighting.
- **The 16 base colors are not ours.** At Ansi16 the terminal owns what index 4
  actually looks like. Anything depending on a specific appearance is only true
  at Truecolor.
- **Redefining one slot moves everything mapped to it.** Roles are indices, so
  `theme.define{blue=...}` changes every role pointing at blue -- including
  derived backgrounds. That is the intended coupling, not a leak.

## History (retired designs, recorded so they are not reintroduced)

- **Derived, muted diff tints.** Row and word tints were once
  `blend(Background, desaturate(anchor, s), a)` with different weights per tier
  (~18% row, ~45% word). Retired in favor of the flat anchor color.
- **A readability search over tints.** A contrast gate
  (`contrast(tint, fg) >= max(kFloorContrast, kRetainContrast * contrast(bg, fg))`,
  with `kFloorContrast`/`kRetainContrast`) once selected the strongest readable
  tint per theme. Those constants NO LONGER EXIST in the code; specs referencing
  them describe a superseded design.
- **A separately derived selection fill.** `selectionFill` was once desaturated
  to 60% and blended toward Background at up to 40% weight, with its own
  readability search. That made text selection visibly dimmer than the flat
  `TabActive`/`TreeFocus` highlight drawn from the same palette entry -- the
  mismatch the current flat model fixes.
- **Hue-fidelity guarantees at Indexed256.** Inter-kind distinctness and hue
  fidelity were specified with per-kind deltaE thresholds, then descoped to
  Truecolor only: the 6x6x6 cube is too coarse to hold them, so a joint search
  over all tints could not satisfy the constraints simultaneously.

The through-line: each retirement replaced a computed, per-theme-searched color
with a flat one the theme states directly. That is what makes the current model
predictable, and it is why the tuning knobs specified separately operate as
explicit multipliers rather than as a search.
