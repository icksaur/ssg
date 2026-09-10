#include <ssg/Renderer.h>

#include "RendererPaint.h"

#include <ssg/GraphemeLayout.h>
#include <ssg/WidgetLayout.h>

#include <algorithm>
#include <stdexcept>

namespace ssg {

SemanticRole nodeForeground(const UiSchema& schema, std::string_view nodeId,
                            SemanticRole fallback) {
    const auto style = resolveUiNodeStyle(schema, nodeId);
    return style && style->foreground ? *style->foreground : fallback;
}

SemanticRole nodeBackground(const UiSchema& schema, std::string_view nodeId,
                            SemanticRole fallback) {
    const auto style = resolveUiNodeStyle(schema, nodeId);
    return style && style->background ? *style->background : fallback;
}

const UiNode* findUiNodeById(const UiNode& node, std::string_view nodeId) {
    if (node.id.value() == nodeId) return &node;
    if (const auto* leaf = std::get_if<UiLeaf>(&node.content);
        leaf && leaf->widget.id == nodeId) {
        return &node;
    }
    const auto* container = std::get_if<UiContainer>(&node.content);
    if (!container) return nullptr;
    for (const auto& child : container->children) {
        if (const auto* found = findUiNodeById(child, nodeId)) return found;
    }
    return nullptr;
}

SemanticRole chromeGlyphForeground(const UiSchema& schema,
                                   std::string_view nodeId,
                                   SemanticRole resolvedWidgetRole) {
    const auto* node = findUiNodeById(schema.root, nodeId);
    if (!node) return resolvedWidgetRole;
    const auto* leaf = std::get_if<UiLeaf>(&node->content);
    if (leaf && leaf->widget.role) return resolvedWidgetRole;
    const auto style = resolveUiNodeStyle(schema, node->id.value());
    return style && style->foreground ? *style->foreground
                                      : resolvedWidgetRole;
}

// A cell stores a uint8 index into CellGrid.colors, which is the theme's role
// colors (slots 0..kSemanticRoleCount-1) followed by its scope colors. A role
// or scope IS its own slot, so these are a trivial identity/offset -- the shared
// palette indirection is gone.
std::uint8_t semanticIndex(ThemeSnapshot const& theme, SemanticRole role) {
    auto const roleIndex = static_cast<std::size_t>(role);
    if (roleIndex >= theme.roleColors.size()) {
        throw std::invalid_argument{"grid contains an unknown semantic role"};
    }
    return static_cast<std::uint8_t>(roleIndex);
}

std::uint8_t syntaxIndex(ThemeSnapshot const& theme, SyntaxScope scope) {
    auto const scopeIndex = static_cast<std::size_t>(scope);
    if (scopeIndex >= theme.syntaxColors.size()) {
        throw std::invalid_argument{"grid contains an unknown syntax scope"};
    }
    return static_cast<std::uint8_t>(kSemanticRoleCount + scopeIndex);
}

namespace {

// The flat render color table = role colors then scope colors, in enum order,
// so semanticIndex/syntaxIndex address it directly.
std::array<SrgbColor, kThemeColorSlotCount> themeColorTable(
    ThemeSnapshot const& theme) {
    std::array<SrgbColor, kThemeColorSlotCount> table{};
    for (std::size_t i = 0; i < kSemanticRoleCount; ++i) table[i] = theme.roleColors[i];
    for (std::size_t i = 0; i < kSyntaxScopeCount; ++i) {
        table[kSemanticRoleCount + i] = theme.syntaxColors[i];
    }
    return table;
}

// The diff washes are the theme's Diff* role colors directly (no derivation, no
// HSV): row and word share one color per kind, and a modified word reuses the
// added color (an inserted span reads as "added").
DiffTints themeDiffTints(ThemeSnapshot const& theme) {
    const auto added = theme.color(SemanticRole::DiffAdded);
    const auto removed = theme.color(SemanticRole::DiffRemoved);
    const auto modified = theme.color(SemanticRole::DiffModified);
    return {.addedRow = added,
            .removedRow = removed,
            .modifiedRow = modified,
            .addedWord = added,
            .removedWord = removed,
            .modifiedWord = added};
}

} // namespace

void put(CellGrid& grid, int x, int y, std::string text, std::uint8_t foreground,
         std::uint8_t background, SemanticRole role, bool continuation,
         DiffTint tint) {
    if (x < 0 || y < 0 || x >= grid.size.columns || y >= grid.size.rows) return;
    grid.cells[static_cast<std::size_t>(y * grid.size.columns + x)] = {
        std::move(text), foreground, background, role, continuation, tint};
}

void fillRect(CellGrid& grid, Rect const& rect, std::uint8_t foreground,
               std::uint8_t background, SemanticRole role) {
    for (int y = rect.y; y < rect.bottom(); ++y) {
        for (int x = rect.x; x < rect.right(); ++x) {
            put(grid, x, y, " ", foreground, background, role);
        }
    }
}

// Paints one row of text clipped to [x, right).  Content that does not fit is
// truncated and the last visible cell shows an ellipsis in the same role.
void paintText(CellGrid& grid, int x, int y, int right, std::string_view text,
                std::uint8_t foreground, std::uint8_t background,
                SemanticRole role, Style const& style) {
    if (right <= x) return;
    auto run = computeCellRun(text);
    int column = x;
    bool truncated = false;
    for (auto const& span : run.spans) {
        auto const width = std::max<std::uint32_t>(span.cellWidth, 1);
        if (column + static_cast<int>(width) > right) {
            truncated = true;
            break;
        }
        auto piece = std::string{text.substr(span.byteOffset, span.byteLen)};
        if (span.kind == CellKind::Tab) {
            piece.assign(span.cellWidth, ' ');
        } else if (span.kind == CellKind::Control ||
                   span.kind == CellKind::InvalidUtf8) {
            piece = style.unrenderable;
        }
        put(grid, column, y, std::move(piece), foreground, background, role);
        for (std::uint32_t offset = 1; offset < width; ++offset) {
            put(grid, column + static_cast<int>(offset), y, "", foreground,
                background, role, true);
        }
        column += static_cast<int>(width);
    }
    if (truncated) {
        put(grid, right - 1, y, style.truncation, foreground, background, role);
    }
}

// Paint a scrollbar into a reserved 1-column gutter from resolved metrics.  When
// the content fits, Style resolves the whole column to its gutter glyph (blank
// by default), so a thumb appearing or vanishing never changes the content width
//.  Which glyph each row gets, and how a short thumb
// degrades, is the style's rule -- this function only reconciles the metrics'
// conventions with Style's and maps the resolved kind onto a color role.
void paintScrollGutter(CellGrid& grid, int x, int y, int height,
                         ScrollbarMetrics const& metrics,
                         ThemeSnapshot const& theme, std::uint8_t background,
                         Style const& style) {
    auto const track = semanticIndex(theme, SemanticRole::ScrollbarTrack);
    auto const thumb = semanticIndex(theme, SemanticRole::ScrollbarThumb);
    // ScrollbarMetrics reports a FULL-VIEWPORT thumb when nothing scrolls
    // (`thumbSize == viewportRows`, `maximumFirstRow == 0`), not an absent one.
    // Style's contract is the clearer "size 0 means no thumb", so the two are
    // reconciled here rather than teaching Style the metrics' convention.
    int const thumbSize =
        metrics.maximumFirstRow > 0 ? static_cast<int>(metrics.thumbSize) : 0;
    for (int row = 0; row < height; ++row) {
        auto const cell = style.scrollbarCell(
            row, static_cast<int>(metrics.thumbStart), thumbSize, height);
        bool const isThumb = cell.kind == ScrollbarCellKind::Thumb;
        put(grid, x, y + row, std::string{cell.glyph},
            isThumb ? thumb : track, background,
            isThumb ? SemanticRole::ScrollbarThumb
                     : SemanticRole::ScrollbarTrack);
    }
}

namespace {

// Render the declined-layout ("terminal too small") screen: a placeholder grid
// of the terminal's size carrying a centered library-owned message.  The library
// owns this screen so a client contributes no cell content (M11-L).  Sized
// from the terminal dimensions the client
// viewport carries, since the shell layout was declined (viewport {0,0}).
CellGrid renderTooSmall(GridSize size, ThemeSnapshot const& theme,
                         Style const& style) {
    auto const foreground = semanticIndex(theme, SemanticRole::Text);
    auto const background = semanticIndex(theme, SemanticRole::Canvas);
    CellGrid grid{
        size, themeColorTable(theme),
        std::vector<CellGridCell>(
            static_cast<std::size_t>(std::max(0, size.columns) *
                                     std::max(0, size.rows)),
            CellGridCell{" ", foreground, background, SemanticRole::Canvas,
                         false})};
    grid.diffTints = themeDiffTints(theme);
    grid.selectionFill = theme.color(SemanticRole::Selection);
    if (size.columns <= 0 || size.rows <= 0) return grid;
    std::string_view const message = "terminal too small";
    auto const messageCells =
        static_cast<int>(computeCellRun(message).totalCells);
    int const row = size.rows / 2;
    int const start = std::max(0, (size.columns - messageCells) / 2);
    paintText(grid, start, row, size.columns, message, foreground, background,
               SemanticRole::Text, style);
    return grid;
}

} // namespace

CellGridCell const& CellGrid::at(int column, int row) const {
    if (column < 0 || row < 0 || column >= size.columns || row >= size.rows) {
        throw std::out_of_range{"cell is outside the grid"};
    }
    return cells[static_cast<std::size_t>(row * size.columns + column)];
}

CellGrid renderFrame(const GridPresentation& snapshot,
                     LineLayoutCache& lineCache) {
    auto const& theme = snapshot.theme;
    auto const& style = snapshot.style;
    const FocusTarget effectiveFocus =
        effectiveUiFocus(snapshot.uiTree);
    const auto* root =
        snapshot.layout.find(UiNodeId{std::string{kRootNodeId}});
    if (!root) {
        // The shell layout was declined (viewport below the 20x4 minimum): the
        // library renders the too-small placeholder, sized from the terminal
        // dimensions the client viewport carries (M11-L).
        auto const& dimensions = snapshot.viewport.dimensions;
        return renderTooSmall(
            GridSize{static_cast<int>(dimensions.columns),
                     static_cast<int>(dimensions.rows)},
            theme, style);
    }

    const auto& ui = snapshot.uiTree;
    const auto rootForeground =
        nodeForeground(ui, kRootNodeId, SemanticRole::Text);
    const auto rootBackground =
        nodeBackground(ui, kRootNodeId, SemanticRole::Canvas);
    const auto documentBackgroundRole =
        nodeBackground(ui, kDocumentNodeId, SemanticRole::Canvas);
    auto const foreground = semanticIndex(theme, rootForeground);
    auto const background = semanticIndex(theme, rootBackground);
    auto const documentBackground =
        semanticIndex(theme, documentBackgroundRole);
    const GridSize gridSize{root->rect.width, root->rect.height};
    CellGrid grid{
        gridSize, themeColorTable(theme),
        std::vector<CellGridCell>(
            static_cast<std::size_t>(gridSize.columns * gridSize.rows),
            CellGridCell{" ", foreground, background, SemanticRole::Canvas,
                         false})};
    grid.diffTints = themeDiffTints(theme);
    grid.selectionFill = theme.color(SemanticRole::Selection);

    auto const panelBackground = snapshot.panel
        ? semanticIndex(theme, nodeBackground(ui, kPanelNodeId,
                                             SemanticRole::TreeBackground))
        : background;
    if (snapshot.panel) {
        const auto panelBackgroundRole =
            nodeBackground(ui, kPanelNodeId, SemanticRole::TreeBackground);
        fillRect(grid, snapshot.panel->rect, foreground, panelBackground,
                  panelBackgroundRole);
    }

    // The header and footer are solid chrome bands distinct from the document,
    // so fill their whole rows first; the field text then paints on the band and
    // the gaps between fields carry the band colour rather than the document
    // background.
    if (const auto* header =
            snapshot.layout.find(UiNodeId{std::string{kHeaderNodeId}})) {
        const auto role =
            nodeBackground(ui, kHeaderNodeId, SemanticRole::HeaderBackground);
        fillRect(grid, header->rect, foreground,
                 semanticIndex(theme, role), role);
    }
    if (const auto* footer =
            snapshot.layout.find(UiNodeId{std::string{kFooterNodeId}})) {
        const auto role =
            nodeBackground(ui, kFooterNodeId, SemanticRole::FooterBackground);
        fillRect(grid, footer->rect, foreground,
                 semanticIndex(theme, role), role);
    }
    if (const auto* tabBar = snapshot.layout.find(
            UiNodeId{std::string{kTabBarNodeId}})) {
        const auto role =
            nodeBackground(ui, kTabBarNodeId,
                           SemanticRole::TabInactiveBackground);
        paintTabBar(
            grid,
            solveTabBar(snapshot.tabs, style.tab, tabBar->rect),
            theme, style, role, foreground, documentBackground);
    }

    if (snapshot.header) {
        paintUiRegion(
            grid, *snapshot.header, theme, ui,
            nodeBackground(ui, kHeaderNodeId,
                           SemanticRole::HeaderBackground),
            style);
    }
    if (snapshot.footer) {
        paintUiRegion(
            grid, *snapshot.footer, theme, ui,
            nodeBackground(ui, kFooterNodeId,
                           SemanticRole::FooterBackground),
            style);
    }
    if (snapshot.notice) {
        const auto* node =
            snapshot.layout.find(UiNodeId{std::string{kNoticeNodeId}});
        if (!node) {
            throw std::logic_error(
                "renderFrame: notice has no solved UI node");
        }
        paintNotice(grid, *snapshot.notice,
                    solveNoticeSurface(*snapshot.notice,
                                       node->rect),
                    theme, style,
                    node->style.foreground.value_or(SemanticRole::Canvas),
                    node->style.background.value_or(
                        SemanticRole::StatusWarning));
    }
    if (!snapshot.externalModification.files.empty()) {
        const auto* node = snapshot.layout.find(
            UiNodeId{std::string{kExternalModNodeId}});
        if (!node) {
            throw std::logic_error(
                "renderFrame: external modification has no solved UI node");
        }
        paintExternalModification(
            grid, snapshot.externalModification,
            solveExternalModificationSurface(
                snapshot.externalModification, node->rect),
            theme, style,
            node->style.foreground.value_or(SemanticRole::Canvas),
            node->style.background.value_or(
                SemanticRole::StatusWarning));
    }

    if (snapshot.panel) {
        paintPanelTree(grid, *snapshot.panel, theme, panelBackground,
                       effectiveFocus == FocusTarget::Panel, style);
    }
    if (snapshot.document ||
        snapshot.layout.find(
            UiNodeId{std::string{kFindResultsViewportNodeId}})) {
        if (const auto* palette = snapshot.layout.find(
                UiNodeId{std::string{kFindResultsViewportNodeId}})) {
            const auto paletteBackground = semanticIndex(
                theme, nodeBackground(ui, kFindResultsNodeId,
                                      SemanticRole::Canvas));
            const auto solved = solvePaletteSurface(
                snapshot.palette, palette->rect,
                style.dimensions.scrollbarGutterWidth);
            fillRect(grid, solved.rect, foreground, paletteBackground,
                     SemanticRole::Canvas);
            paintPalette(grid, snapshot.palette, solved, theme,
                         paletteBackground, style);
        } else if (snapshot.document) {
            const auto& document = *snapshot.document;
            fillRect(grid, document.content, foreground, documentBackground,
                     documentBackgroundRole);
            paintDocument(grid, snapshot, document.content, theme,
                           documentBackground, style, lineCache);
            if (snapshot.tabs.tabs.empty() &&
                snapshot.documentRevision == 0) {
                paintText(grid, document.content.x, document.content.y,
                          document.content.right(), "empty editor",
                          foreground, documentBackground,
                          documentBackgroundRole, style);
            }
            // After the document: a diagnostic underlines whatever the cell
            // already shows rather than replacing it.
            paintDiagnostics(grid, snapshot, document.content);
            paintHyperlinks(grid, snapshot, document.content);
            paintLineNumbers(grid, snapshot, document, theme);
            paintScrollbar(grid, document, snapshot.viewport,
                           theme, documentBackground, style);

            // Paint the reserved prompt rows (find/replace/settings) and place
            // the hardware cursor at the query when the prompt is focused.
            if (snapshot.promptStatus.activeKind &&
                promptFocusRegion(*snapshot.promptStatus.activeKind) ==
                    PromptRegion::Footer) {
                const auto promptForegroundRole =
                    nodeForeground(ui, kFooterPromptNodeId,
                                   SemanticRole::Prompt);
                const auto promptBackgroundRole =
                    nodeBackground(ui, kFooterPromptNodeId,
                                   SemanticRole::Canvas);
                auto promptCaret =
                    paintPrompt(grid, snapshot.uiTree,
                                snapshot.layout, theme,
                                promptForegroundRole, promptBackgroundRole,
                                style);
                if (effectiveFocus == FocusTarget::Prompt && promptCaret) {
                    grid.caret = *promptCaret;
                }
            }

            // A picker holds Prompt focus but reserves ZERO prompt rows -- its
            // query lives in the header -- so paintPrompt yields no caret here.
            // The input line's caret is published after this block, outside
            // both pane branches.

            // Place the primary caret at its screen cell so the client can position
            // a terminal cursor there, and paint any secondary carets as cells
            // (a terminal has one hardware cursor), but only when the editor is
            // focused.
            if (effectiveFocus == FocusTarget::Editor) {
                auto const& content = document.content;
                auto const& viewport = snapshot.viewport;
                auto const& selections =
                    snapshot.selections;
                auto const& primary = selections.primary();
                if (auto cell = screenCellFor(viewport, content,
                                                primary.active.line.value(),
                                                primary.active.cell.value())) {
                    grid.caret = *cell;
                }
                // The secondary caret is a block cursor that INVERTS the cell it
                // sits on: its glyph takes the cell's own background color and it
                // paints over the cell's own foreground color. Those two colors
                // are already legible against each other (the text under the
                // caret was readable), so the block cursor is legible in every
                // theme without relying on any cross-role distinctness rule --
                // there is no longer a co-visibility constraint to lean on. The
                // primary caret uses the hardware cursor.
                for (auto const& item : selections.items()) {
                    if (&item == &primary) continue;
                    // Every non-primary selection (ranged or a bare caret) has an
                    // active caret position that renders as a caret cell; only the
                    // primary uses the single hardware cursor.
                    auto cell = screenCellFor(viewport, content,
                                                item.active.line.value(),
                                                item.active.cell.value());
                    if (!cell) continue;
                    auto const& existing = grid.at(cell->column, cell->row);
                    put(grid, cell->column, cell->row,
                        existing.text.empty() ? std::string{" "} : existing.text,
                        existing.background, existing.foreground,
                        SemanticRole::Caret);
                }
            }
        }
    }

    // The input line's caret, published outside the pane branches above.  A
    // picker paints its results through the retained find-results branch, so a
    // caret placed beside the prompt rows in the `else` branch
    // would never be reached while a picker is open.  The cursor is the primary
    // way a user can tell a text input has focus, so it must
    // not depend on which pane branch ran.
    if (effectiveFocus == FocusTarget::Prompt &&
        snapshot.header && snapshot.header->input) {
        const auto& caret = snapshot.header->input->caret;
        grid.caret = GridPosition{caret.x, caret.y};
    }
    return grid;
}

} // namespace ssg
