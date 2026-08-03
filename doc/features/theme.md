# spec-theme

## Goals

Let a user script replace individual theme colors on the running theme without
hand-authoring a full theme (every UI role and every syntax scope) just to
change a couple of colors.

## Design

`ThemeSnapshot` is the whole of a theme: `roleColors` (one `SrgbColor` per
`SemanticRole`) and `syntaxColors` (one per `SyntaxScope`). There is no palette
and no index indirection -- a role or scope IS its color. See `doc/spec-color.md`
for the full color model.

`theme.set` takes a table keyed by role and syntax-scope names (the snake_case
`semanticRoleName`/`syntaxScopeName` strings, e.g. `background`, `foreground`,
`selection`, `tab_active`, `diff_added`, `comment`, `keyword`), each mapped to a
`"#rrggbb"` hex string. A name absent from the table keeps the CURRENT active
theme's color for that role or scope -- the table may be partial. Role and scope
names share one flat namespace (they are disjoint sets). The set color is the
final color: there is no derivation, so setting `diff_added` immediately and
exactly changes the diff-added background.

The command is all-or-nothing: an unknown name or a malformed hex string rejects
the whole call before any color is replaced.

Normative command owned by this feature:

- `theme.set`

## Invariants

- Literal colors remain forbidden outside the theme (I22 from `doc/spec.md`) --
  `theme.set`'s hex-string parsing lives in `Theme.cpp`, and the one place raw
  RGB channels are stated as final colors is `DefaultTheme.cpp`.
- `theme.set` is total: an accepted call replaces only the named colors; a
  rejected call mutates nothing.

## Considerations

- Role/scope names were chosen over any palette-slot scheme because with one
  color per role there is nothing to index -- naming the role directly is the
  whole point, and it lets a user set exactly the element they mean without
  re-specifying every other role.

## Acceptance (Definition of Done)

- Observable: a table naming every role and scope with the current theme's own
  values is a byte-identical no-op; a partial table changes only the named
  colors and leaves every other color exactly equal to the prior theme's values;
  an unknown name or malformed hex string is rejected with the prior theme
  unchanged.
- Gates: project build and theme tests are green.
- Oracles: full/partial/invalid table cases against `ThemeSnapshot` equality
  (see `doc/spec-config.md`).
