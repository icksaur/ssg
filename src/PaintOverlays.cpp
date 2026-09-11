#include "RendererPaint.h"

#include <ssg/GraphemeLayout.h>

#include <algorithm>

namespace ssg {

void paintPanelTree(CellGrid& grid, SolvedPanelSurface const& panel,
                      ThemeSnapshot const& theme,
                      std::uint8_t background, bool focused,
                      Style const& style) {
    auto const foreground = semanticIndex(theme, SemanticRole::Text);
    auto const directory = semanticIndex(theme, SemanticRole::PanelInactive);
    auto const selectedBg = semanticIndex(theme, SemanticRole::TreeFocus);
    const auto providerRole =
        focused ? SemanticRole::PanelActive : SemanticRole::PanelInactive;
    paintText(grid, panel.providerLabel.x, panel.providerLabel.y,
              panel.providerLabel.right(), panel.providerText,
              semanticIndex(theme, providerRole), background, providerRole,
              style);
    if (panel.query.height > 0) {
        paintText(grid, panel.query.x, panel.query.y, panel.query.right(),
                  panel.queryText,
                  semanticIndex(theme, SemanticRole::Prompt), background,
                  SemanticRole::Prompt, style);
        if (focused && panel.queryEditing && panel.query.width > 0) {
            auto const cursorOffset =
                std::min(panel.queryCursor, panel.queryText.size());
            auto const cells = computeCellRun(
                                    panel.queryText.substr(0, cursorOffset))
                                    .totalCells;
            grid.caret = GridPosition{
                std::min(panel.query.x + static_cast<int>(cells),
                         panel.query.right() - 1),
                panel.query.y};
        }
    }
    if (panel.status.height > 0) {
        paintText(grid, panel.status.x, panel.status.y, panel.status.right(),
                  panel.statusText,
                  semanticIndex(theme, SemanticRole::PanelInactive),
                  background, SemanticRole::PanelInactive, style);
    }
    for (const auto& row : panel.rows) {
        auto const rowBackground = row.selected ? selectedBg : background;
        if (row.selected) {
            fillRect(grid, row.rect, foreground, rowBackground,
                    SemanticRole::TreeFocus);
            if (focused) grid.caret = GridPosition{row.rect.x, row.rect.y};
        }
        const auto role =
            row.directory ? SemanticRole::PanelInactive : SemanticRole::Text;
        const auto color = row.directory ? directory : foreground;
        paintText(grid, row.rect.x, row.rect.y, row.rect.right(), row.text,
                  color, rowBackground, role, style);
    }
    if (panel.scrollbarGutter) {
        paintScrollGutter(grid, panel.scrollbarGutter->x,
                        panel.scrollbarGutter->y,
                        panel.scrollbarGutter->height, panel.scrollbar, theme,
                        background, style);
    }
}

// Projects the palette's ranked results into the active pane while the palette
// prompt is open.  The query and caret live in the header;
// this paints only the results window with the selected row highlighted.
void paintPalette(CellGrid& grid, PaletteReport const& palette,
                  SolvedPaletteSurface const& solved,
                  ThemeSnapshot const& theme, std::uint8_t background,
                  Style const& style) {
    if (solved.rect.width <= 0 || solved.rect.height <= 0) return;
    auto const foreground = semanticIndex(theme, SemanticRole::Text);
    auto const detailColor = semanticIndex(theme, SemanticRole::LineNumber);
    auto const selectedBg = semanticIndex(theme, SemanticRole::Selection);
    // `rows` is already the client's windowed subset; `selected`/`first_visible`
    // are absolute, so the selected row's screen index is selected-first_visible.
    for (const auto& solvedRow : solved.visibleRows) {
        auto const& row = palette.rows[solvedRow.windowIndex];
        const auto& rect = solvedRow.rect;
        bool const isSelected = solvedRow.selected;
        auto const rowBackground = isSelected ? selectedBg : background;
        auto const rowRole =
            isSelected ? SemanticRole::Selection : SemanticRole::Canvas;
        auto const labelRole =
            isSelected ? SemanticRole::Selection : SemanticRole::Text;
        auto const detailRole =
            isSelected ? SemanticRole::Selection : SemanticRole::LineNumber;
        fillRect(grid, rect, foreground, rowBackground,
                  rowRole);
        paintText(grid, rect.x, rect.y, rect.right(), row.label, foreground,
                   rowBackground, labelRole, style);
        if (!row.detail.empty()) {
            auto const run = computeCellRun(row.detail);
            int width = 0;
            for (auto const& span : run.spans) {
                width += static_cast<int>(std::max<std::uint32_t>(span.cellWidth, 1));
            }
            int const start = std::max(rect.x, rect.right() - width);
            paintText(grid, start, rect.y, rect.right(), row.detail, detailColor,
                       rowBackground, detailRole, style);
        }
    }
    // Paint the reserved gutter (blank when the ranked list fits).
    if (solved.scrollbar.width > 0 && solved.scrollbar.height > 0) {
        paintScrollGutter(grid, solved.scrollbar.x,
                            solved.scrollbar.y,
                            solved.scrollbar.height, palette.scrollbar,
                            theme, background, style);
    }
}

std::optional<GridPosition> paintPrompt(CellGrid& grid,
                                         const UiSchema& schema,
                                         SolvedGridTree const& layout,
                                         ThemeSnapshot const& theme,
                                         SemanticRole foregroundRole,
                                         SemanticRole backgroundRole,
                                         Style const& style) {
    auto const promptFg = semanticIndex(theme, foregroundRole);
    auto const promptBg = semanticIndex(theme, backgroundRole);
    const UiNode* prompt = findUiNodeById(
        schema.root, kFooterPromptNodeId);
    if (!prompt) return std::nullopt;
    std::optional<GridPosition> caret;
    const auto paint = [&](auto&& self, const UiNode& node) -> void {
        if (const auto* container = std::get_if<UiContainer>(&node.content)) {
            for (const auto& child : container->children) self(self, child);
            return;
        }
        const auto* schemaLeaf = std::get_if<UiLeaf>(&node.content);
        if (!schemaLeaf) return;
        if (!node.resolved) return;
        const auto& leaf = *node.resolved;
        const auto* solved = layout.find(node.id);
        if (!solved) return;
        const Rect& rect = solved->rect;
        std::string text;
        switch (schemaLeaf->widget.kind) {
            case WidgetKind::TextInput:
                text = textInputText(leaf.label,
                                    style.promptLabelSeparator,
                                    leaf.value);
                break;
            case WidgetKind::Label:
                text = leaf.value;
                break;
            case WidgetKind::Checkbox:
                text = checkboxText(leaf.checked.value_or(false),
                                    leaf.label,
                                    style.toggle);
                break;
            default:
                return;
        }
        // Clear the row region first so a shrinking value does not leave stale
        // glyphs behind, then paint the control text.
        for (int column = rect.x; column < rect.right(); ++column) {
            put(grid, column, rect.y, " ", promptFg, promptBg,
                SemanticRole::Prompt);
        }
        paintText(grid, rect.x, rect.y, rect.right(),
                   text, promptFg, promptBg, SemanticRole::Prompt, style);
        if (schemaLeaf->widget.kind == WidgetKind::TextInput &&
            leaf.active.value_or(false) && !caret) {
            auto const labelWidth = static_cast<int>(
                computeCellRun(textInputText(
                                   leaf.label, style.promptLabelSeparator, {}))
                    .totalCells);
            auto const cursorOffset =
                std::min(leaf.cursor.value_or(leaf.value.size()),
                        leaf.value.size());
            auto const beforeCursorWidth = static_cast<int>(
                computeCellRun(leaf.value.substr(0, cursorOffset)).totalCells);
            auto const cursorColumn = std::min(
                rect.x + labelWidth + beforeCursorWidth, rect.right() - 1);
            caret = GridPosition{cursorColumn, rect.y};
        }
    };
    paint(paint, *prompt);
    return caret;
}

} // namespace ssg
