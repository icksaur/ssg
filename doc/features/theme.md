# spec-theme

## Goals

Let a user script replace individual palette colors on the running theme
without hand-authoring a full theme (all 16 palette slots, every semantic
role, every syntax scope) just to change a couple of colors.

## Design

`Theme` already models a fixed 16-slot indexed palette
(`kThemePaletteSize`) plus role/syntax index mappings into it
(`RoleMapping`/`SyntaxMapping`); `ThemeSnapshot` is the flat, renderer-facing
projection of a `Theme` (palette + role/syntax indices + derived
`DiffTints`/`selectionFill`).

`theme.define` takes a table keyed by the 16 classic ANSI palette-slot
names (`black`, `red`, `green`, `yellow`, `blue`, `magenta`, `cyan`,
`white`, and their `bright*` counterparts), each mapped to a `"#rrggbb"`
hex string. A name absent from the table keeps the CURRENT active theme's
color for that slot -- the table may be partial. `theme.define` replaces
ONLY those palette entries; it never touches which semantic role or
syntax scope points at which slot, so overriding one or two colors does
not require re-specifying all 32 role/11 syntax-scope assignments. Diff
tints and the selection fill are pure functions of palette + role/syntax
indices and are recomputed from the new palette after replacement.

The command is all-or-nothing: an unknown slot name or a malformed hex
string rejects the whole call before any slot is replaced.

Normative command owned by this feature:

- `theme.define`

## Invariants

- Literal colors remain forbidden outside `Theme` (I19 from `doc/spec.md`)
  -- `theme.define`'s hex-string parsing and the one place raw RGB
  channels are ever constructed from user input live in `Theme.cpp`, the
  same file already responsible for every other color literal in the
  system.
- Role/syntax mappings are never mutated by this command; only the 16
  palette RGB values can change.

## Considerations

- Palette-slot naming (16 classic ANSI names) was chosen over
  `SemanticRole` names because it matches how terminal color-scheme
  configs already name things and fits the palette's own 16-slot shape
  exactly, without requiring all 32 semantic roles to be specified to
  produce one legal palette.

## Acceptance (Definition of Done)

- Observable: a table replacing all 16 slots with the current theme's own
  values is a byte-identical no-op; a partial table changes only the
  named slots and leaves every other slot exactly equal to the prior
  theme's values; an unknown key or malformed hex string is rejected with
  the prior theme unchanged.
- Gates: project build and theme tests are green.
- Oracles: full/partial/invalid table cases against `ThemeSnapshot`
  equality (see `doc/spec-config.md`).
