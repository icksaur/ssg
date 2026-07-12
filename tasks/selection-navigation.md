# selection-navigation

- Spec: `doc/features/core-editing.md`, Plan 4
- Depends: `document-transactions`, `viewport-wrap-scrollbar`
- Branch: `selection-navigation-task`

## Scope

Implement normalized selections, semantic cursor/range commands, word/line/page
movement, extend selection, multi-cursor creation including next occurrence,
matching-bracket navigation and selection, and explicit reveal/center-caret
commands. Text transforms are owned by later tasks.

The immutable `SelectionNavigationCommandSet` contains every command owned by
`selection-navigation` in `data/required-commands.json`: the 13 `cursor.*`
commands, the 20 `select.*` commands, `goto.matching_bracket`,
`view.reveal_caret`, and `view.center_caret`.

Positions use byte offsets plus zero-based logical line and display-cell
coordinates. Horizontal movement follows grapheme boundaries. Vertical and
page movement preserve the primary active endpoint's desired display cell,
clamping only on shorter lines; page movement uses the viewport's visible-row
count. Bracket pairs are resolved settings injected into the operation, so this
task does not depend on `settings-model`. Matching-bracket commands scan the
injected pairs with nesting: `goto.matching_bracket` moves to the mate, while
`select.to_matching_bracket` extends through the mate.

Every accepted movement or selection command minimally reveals the primary
active endpoint. `view.reveal_caret` performs the same minimal reveal and
`view.center_caret` centers when scroll bounds permit. An already visible caret
does not scroll.

## Files

`include/ssg/selection.h`, `src/selection.cpp`,
`tests/test_selection.cpp`, `cmake/components/selection-navigation.cmake`

## Oracle

Reference-editor command scripts plus viewport intersection properties after
every movement or selection transition. Hand-computed page and nested-bracket
fixtures independently check commands absent from the reference editor. Reveal
properties check both visibility and minimality: no movement when already
visible and the smallest scroll otherwise. Reference comparisons use ASCII
fixtures where byte and display-cell columns coincide; tab, combining, and wide
grapheme fixtures independently verify display-cell vertical movement.

## Done

All 36 owned command IDs are present, reference and hand-authored command
oracles pass, I13/I23 gates are green, and no text transform command is added.
