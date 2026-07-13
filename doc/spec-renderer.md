# Spec: server-owned cell renderer

## Goals

Make the library the single authority for what every client draws, as a
deterministic monospace **cell grid**, and make the renderer correct: each shell
region shows its real content (field values, tab titles, tree labels, document
text) instead of accessibility strings, with no overlapping text.

Today `render_screen` lives in the TUI example fixture
(`examples/tui/tui_fixture.cpp`) and paints each `AccessibilityNode.label` as
visible text. Those labels are accessibility strings ("Status header", "Open
tabs", "Side panel", "Workspace"), not display content, and container and leaf
nodes share rects, so text overlaps (e.g. `Workspaceader`, `▸ appsanel`). The
actual display values (cwd path, footer status, encoding, tab titles) are
assembled into `ShellLayoutRequest` in `src/runtime/snapshot.cpp` but dropped:
`ShellViewState` exposes rects plus accessibility labels only, never the values.

This spec (1) promotes the renderer into the library as the authoritative
snapshot→grid transform every client uses, and (2) extends the shell view model
so the grid can show real content. It intentionally covers both because the
renderer cannot be correct without the content, and the content change is inert
without the renderer. The view-model content change (Plan step 1) stays in this
spec rather than being split out: it is tightly coupled to renderer correctness
and inert on its own, so splitting would add review churn without reducing
design risk.

## Design

### Cell grid contract

The library exposes a pure function

```
ssg::CellGrid ssg::render(SessionSnapshot const& snapshot);
```

in `include/ssg/render.h` / `src/render.cpp`. `CellGrid` is the current
`ScreenSnapshot` shape moved into namespace `ssg`:

- `GridSize size;`
- `std::array<SrgbColor, theme_palette_size> palette;` (copied from the theme)
- `std::vector<CellGridCell> cells;` row-major, `size.columns * size.rows`

`CellGridCell` keeps `{ std::string text; std::uint8_t foreground; std::uint8_t
background; SemanticRole role; bool continuation; }`. `foreground`/`background`
are palette indices; `continuation` marks the trailing columns of a wide glyph.
The function is deterministic and side-effect free: equal snapshots yield
byte-identical grids (the `canonical()` serialization is retained for goldens).

`render` is the only place snapshot geometry and content become cells. Clients
(TUI now, browser later) consume `CellGrid` and only translate it to their
medium (ANSI, DOM); they add no layout, content, or color.

### Shell view model carries content

`AccessibilityNode` gains a `std::string content` field: the display text for
that node (empty for pure containers and panes). `add_fields`/`add_node` in
`src/ui_layout.cpp` set `content` from the corresponding
`StatusField.value` / `TabLabel.title` / `panel_provider_label`; `label`
remains the accessibility string. `ShellLayoutRequest` already carries these
values, so this is plumbing, not new state. If `AccessibilityNode` crosses the
protocol, its codec and round-trip corpus are updated.

Node content sources after this change:

| Region (ShellNodeKind) | Background fill role | Painted text |
|---|---|---|
| header, footer, tab_bar, panel, pane | region role fill; no text | — |
| header_field, footer_field, footer_action | field role | node `content` (field value / action label) |
| tab | tab_active/tab_inactive | node `content` (tab title), dirty marker |
| panel_provider | panel_active/inactive | provider name |
| empty_state | background | empty-state text |
| scrollbar | scrollbar_track | track `|` and thumb `#` from `ViewportViewState.scrollbar` |

Non-shell content the renderer composes from domain sections: document text
from `DocumentViewState` with syntax colors (`SyntaxViewState`), selection and
caret from `SelectionViewState`, tree nodes from `TreeViewState` (indent,
twisty, label), prompt from `PromptStatusViewState`.

### Rendering order and overlap rule

1. Fill the whole grid with `background`.
2. Fill each container rect (header, footer, tab_bar, panel, pane, prompt) with
   its region background role.
3. Paint leaf content (fields, actions, tabs, provider, tree nodes, document,
   scrollbars, prompt, empty state), each clipped to its own rect.

Container nodes never paint text. Layout already gives leaf rects disjoint
horizontal spans within a region, so no two leaves overlap. Content wider than
its rect is truncated at the cell boundary; when truncated, the last cell of the
rect shows `…` in the leaf's own foreground role. In a one-cell-wide rect whose
content does not fit, that single cell shows `…`. Wide glyphs that would straddle
the right edge are replaced by a space.

### Colors

Every cell foreground and background is a palette index resolved from the active
`ThemeSnapshot` through a `SemanticRole` (or `SyntaxScope` for document text).
No literal or computed colors. Selection, caret, diagnostics, and dirty markers
use their existing roles.

## Invariants

- I7 (grid-owned UI): the grid is a deterministic monospace cell model with no
  pixel/DOM/escape behavior; `render` is that model's sole producer.
- I17 (server-owned product): layout and content originate in the library;
  clients contain no rendering logic beyond blitting `CellGrid`.
- I22 (color authority): every cell color is a theme palette index.
- R1 (no phantom text): the renderer's only text inputs are leaf-node `content`
  and domain-section data (document, tree, prompt, scrollbar). Accessibility
  `label` strings are never a paint input.
- R2 (leaf non-overlap): background and container fills may be overwritten by
  content, but no two leaf paints (fields, actions, tabs, provider, tree rows,
  document, prompt, scrollbar, empty state) write the same cell. Every non-space
  content cell belongs to exactly one leaf rect.

## Considerations

- `render` stays a free function, not a snapshot member, so the snapshot remains
  pure semantic state and the transform is independently testable.
- Keep `render` off the fast path only in that it is called per frame by the
  client; it allocates one grid and is O(cells).
- The a11y `label` remains for the eventual browser accessibility tree; adding
  `content` does not remove it.

## Risks and mitigations

- Golden churn: promoting and rewriting changes rendered output. Mitigate by
  re-authoring goldens deliberately and asserting the overlap/palette
  properties, not just byte goldens.
- Protocol surface: if `AccessibilityNode` is serialized, the new field must
  round-trip; add it to the malformed/round-trip corpus.

## Acceptance (Definition of Done)

- Observable: `ssg` shows the real cwd/path in the header, real status/encoding
  in the footer, real tab titles, and real tree labels, with no overlapping or
  accessibility-string text, in empty-workspace, open-file, panel-shown, and
  prompt-open states.
- The renderer lives in the library; the TUI example and the `ssg` app both call
  `ssg::render`; no rendering logic remains in the app.
- Gates: `ctest --preset dev` green; render goldens and property tests green.
- Oracles: hand-authored `CellGrid.canonical()` goldens for empty, open-file,
  panel-shown-and-expanded, prompt-open, and minimum-size viewports; property
  tests asserting every non-space content cell lies in exactly one leaf rect
  (R2), every color is a palette index (I22), and identical snapshots render
  byte-identical grids. R1 is enforced by provenance: `render` takes text only
  from `AccessibilityNode.content` and domain sections, never from `label`; a
  test drives a snapshot whose leaf `content` differs from its `label` and
  asserts the grid contains the `content`, not the `label`.

## Plan

| Step | Work | Files | Oracle |
|---|---|---|---|
| 1 | Add `content` to `AccessibilityNode`; populate field/tab/provider/empty values in layout; update protocol codec if serialized | `include/ssg/ui_layout.h`, `src/ui_layout.cpp`, `src/runtime/snapshot.cpp`, `src/protocol.cpp`, `tests/test_ui_layout.cpp`, `tests/test_protocol.cpp` | ui_layout tests assert leaf nodes carry display values; protocol round-trip |
| 2 | Create `ssg::CellGrid` + `ssg::render` in the library by moving the renderer out of the fixture | `include/ssg/render.h`, `src/render.cpp`, `cmake/components/render.cmake`, `tests/test_render.cpp` | migrate the existing final-screen golden into a library render test |
| 3 | Rewrite `render` to fill containers and paint leaf content/domain sections, with truncation and no phantom labels | `src/render.cpp`, `tests/test_render.cpp`, `tests/fixtures/render/*` | hand-authored goldens (empty/open-file/panel/prompt/min-size); R1/R2/I22/determinism property tests |
| 4 | Consume `ssg::render` from the app and the TUI example; delete `render_screen` from the fixture | `apps/ssg_main.cpp`, `apps/ssg_terminal.{h,cpp}`, `examples/tui/tui_fixture.{h,cpp}`, `cmake/components/ssg-app.cmake`, `tests/test_ssg_app.cpp` | `test_ssg_app` green; PTY frame shows real content |
