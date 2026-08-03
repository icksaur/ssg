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

### Every role and scope holds its own color

A theme is a flat table of sRGB colors, one per UI role and one per syntax
scope. There is no shared palette and no indirection: a role IS its color.

- 35 `SemanticRole`s (`Foreground`, `Selection`, `GitAdded`, `TabActive`, ...)
  each have their own `SrgbColor`, in `ThemeSnapshot::roleColors`.
- 11 `SyntaxScope`s (`Comment`, `Keyword`, `String`, ...) each have their own
  `SrgbColor`, in `ThemeSnapshot::syntaxColors`.

`ThemeSnapshot` is the whole of a theme: `roleColors` and `syntaxColors`,
nothing else. `themeColor(snapshot, role)` and `themeColor(snapshot, scope)`
read a color out. `defaultTheme()` in `src/DefaultTheme.cpp` is the one
compiled-in theme; there is no data-file theme path.

### Diff and selection backgrounds are just roles

Diff-added, diff-removed, diff-modified, and selection backgrounds are the
`DiffAdded`, `DiffRemoved`, `DiffModified`, and `Selection` roles read
directly -- there is no derivation, no muting, no readability search. The
renderer copies these role colors into the render grid's diff/selection wash
slots. A word-level mark inside a modified line reuses the added color: a word
mark says "here specifically" within an already-tinted row, and the inserted
span reads as added. There is no fourth "modified word" color.

Because a highlight is just a role color used directly, a selected tab
(`TabActive`), a focused tree row (`TreeFocus`), and a text selection each read
their own role and render exactly the color the theme states -- no two of them
can silently diverge through a derivation.

### The render grid is a flat color table

A rendered cell stores two `uint8` indices (foreground, background) into the
grid's `colors` table, which is `roleColors` followed by `syntaxColors`
(`kThemeColorSlotCount` entries). This keeps cells small and the client's job a
plain table lookup; the theme still gives each role and scope its own slot, so
nothing is shared and every color is set directly.

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

`theme.set` (init.lua) takes a table of role and syntax-scope names (the
snake_case `semanticRoleName`/`syntaxScopeName` strings, e.g. `background`,
`selection`, `tab_active`, `comment`, `keyword`), each optionally mapped to
`"#rrggbb"`. Omitted names keep their current value, so the table may be
partial. It is validated whole and applied all-or-nothing: an unknown name or
malformed hex rejects the entire call with no partial mutation. The set color is
the final color -- there is no derivation step, so setting `diff_added`
immediately and exactly changes the add background.

Role and scope names share one flat namespace (they are disjoint sets). There is
one active theme -- no named themes, no switching, no separate wash-intensity
control (set a darker hex directly if a background is too strong).

## Invariants

- **I22 (color authority, `doc/spec.md`)** -- the theme snapshot is the only
  source of color values. Clients and extensions consume its role and scope
  colors through the API; they never mint or substitute a color. A literal RGB
  constant used AS A FINAL COLOR is a violation; a numeric tuning parameter
  (a threshold, a multiplier) is not.
- Every `SemanticRole` and every `SyntaxScope` has exactly one color -- the
  snapshot's arrays are sized `kSemanticRoleCount`/`kSyntaxScopeCount`, so
  adding a role without a color fails to compile.
- `theme.set` is total: an accepted call replaces only the named colors; a
  rejected call mutates nothing.

### How I22 is enforced

`tests/test_theme.cpp`'s `sourceAndConfigHaveNoIndependentColorSources` scans
the repository for independent color sources: hex literals, `rgb(`/`hsl(`,
`lighten|darken|shade|tint|blend|gradient(`, and `SrgbColor{`/`SrgbColor(`.
`src/Theme.cpp` additionally may not contain a bare RGB triple.

Exempt: `include/ssg/Theme.h`, `include/ssg/color.h`, `src/color.cpp`,
`src/DefaultTheme.cpp`, `tests/test_color.cpp`, `tests/test_theme.cpp`, plus
`doc/`, `tasks/`, `vendor/`, `.git/`, and `build*`.

`src/DefaultTheme.cpp` exists as its own translation unit for exactly this
reason: it holds the one compiled-in theme literal, and keeping it out of
`Theme.cpp` keeps that file's stricter no-literal rule intact.

**Consequence for future work:** new color math cannot live in `Theme.cpp` if it
needs literals or names a forbidden verb. It belongs in `src/color.cpp`, which
is the sanctioned home for color computation, or the scan's exemption list must
be extended deliberately.

## Considerations

- **Ansi16 collapses meaning.** At 16 colors, distinct colors can resolve to the
  same swatch, so guarantees about telling add from remove cannot hold. Accepted,
  and the same category of limitation as 16-color syntax highlighting.
- **The 16 base colors are not ours.** At Ansi16 the terminal owns what index 4
  actually looks like. Anything depending on a specific appearance is only true
  at Truecolor.
- **Distinctness is the theme author's job.** Nothing forces two co-visible
  roles to differ any more; if a theme sets `caret` and `selection` to the same
  color they render identically. This freedom is the point -- the author has one
  color per role and full control -- but it moves the responsibility for
  legibility onto whoever writes the `theme.set` table.

## History (retired designs, recorded so they are not reintroduced)

- **A 16-color palette with role/scope INDICES.** A theme was once exactly 16
  sRGB colors (`kThemePaletteSize`), and every role and scope mapped to a
  palette index (`semanticIndices`/`syntaxIndices`). Redefining a slot moved
  every role pointing at it. Retired: each role and scope now holds its own
  color directly, removing the indirection, the 16-color cap, and the shared-slot
  coupling. `theme.define` (16 ANSI slot names) and `theme.background` (wash
  intensity multipliers) were the commands for that model; both are replaced by
  the single `theme.set`.
- **Co-visible distinctness enforcement.** `kCoVisibleRolePairs` listed role
  pairs that appear side by side, and a theme making any pair identical was
  rejected. Retired with the palette: with one color per role, distinctness is
  the author's choice, not a validated constraint.
- **Derived background tints and selection fill.** `DiffTints`/`selectionFill`
  were once derived from the palette. Earlier still they were computed and muted:
  row/word tints as `blend(Background, desaturate(anchor, s), a)` (~18% row, ~45%
  word), a contrast/readability search selecting the strongest readable tint per
  theme, and a selection fill desaturated to 60% and blended toward Background at
  up to 40%. All retired: the diff and selection backgrounds are now the
  `DiffAdded`/`DiffRemoved`/`DiffModified`/`Selection` roles used flat.
- **HSV background-tint adjustment.** `theme.background` scaled the derived
  washes' brightness/saturation in HSL without touching the palette
  (`adjustBackgroundTint`, `BackgroundTintAdjustments`). Retired: with a color
  per role there is no derived wash to scale -- set the role's hex directly.
  `doc/spec-background-tint-adjust.md` is archived.
- **Hue-fidelity guarantees at Indexed256.** Inter-kind distinctness and hue
  fidelity were specified with per-kind deltaE thresholds, then descoped to
  Truecolor only: the 6x6x6 cube is too coarse to hold them.

The through-line: each retirement replaced a computed, per-theme-searched or
indexed color with a flat one the theme states directly. That is what makes the
current model predictable -- one named color per role and scope, set and drawn
without transformation.
