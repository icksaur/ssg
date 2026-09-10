#include "RendererPaint.h"

namespace ssg {

void paintExternalModification(
    CellGrid& grid, const ExternalModificationViewState& external,
    const SolvedExternalModificationSurface& solved,
    ThemeSnapshot const& theme, Style const& style,
    SemanticRole foregroundRole, SemanticRole backgroundRole) {
    const auto foreground = semanticIndex(theme, foregroundRole);
    const auto background = semanticIndex(theme, backgroundRole);
    fillRect(grid, solved.rect, foreground, background, backgroundRole);
    paintText(grid, solved.header.x, solved.header.y, solved.header.right(),
              external.message, foreground, background, backgroundRole, style);
    for (const auto& row : solved.rows) {
        const auto role =
            row.selected ? SemanticRole::Selection : backgroundRole;
        const auto rowBackground = semanticIndex(theme, role);
        fillRect(grid, row.rect, foreground, rowBackground, role);
        paintText(grid, row.rect.x, row.rect.y, row.rect.right(), row.text,
                  foreground, rowBackground, role, style);
        for (const auto& action : row.actions) {
            paintText(grid, action.rect.x, action.rect.y,
                      action.rect.right(), action.text, foreground,
                      rowBackground, role, style);
        }
    }
}

void paintTabBar(CellGrid& grid, const SolvedTabBar& solved,
                 ThemeSnapshot const& theme, Style const& style,
                 SemanticRole backgroundRole,
                 std::uint8_t bandForeground,
                 std::uint8_t documentBackground) {
    const auto bandBackground = semanticIndex(theme, backgroundRole);
    fillRect(grid, solved.rect, bandForeground, bandBackground,
             backgroundRole);
    for (const auto& separator : solved.separators) {
        paintText(grid, separator.rect.x, separator.rect.y,
                  separator.rect.right(), separator.text,
                  semanticIndex(theme, SemanticRole::TabInactive),
                  bandBackground, SemanticRole::TabInactive, style);
    }
    for (const auto& tab : solved.tabs) {
        const auto role =
            tab.active ? SemanticRole::TabActive : SemanticRole::TabInactive;
        const auto background =
            tab.active ? documentBackground : bandBackground;
        fillRect(grid, tab.rect, semanticIndex(theme, role), background, role);
        paintText(grid, tab.rect.x, tab.rect.y, tab.rect.right(), tab.text,
                  semanticIndex(theme, role), background, role, style);
    }
}

void paintUiRegion(CellGrid& grid, const SolvedUiRegion& surface,
                        ThemeSnapshot const& theme, const UiSchema& ui,
                        SemanticRole backgroundRole, const Style& style) {
    const auto background = semanticIndex(theme, backgroundRole);
    for (const auto& item : surface.items) {
        if (item.content.empty()) continue;
        const auto foregroundRole =
            chromeGlyphForeground(ui, item.id, item.role);
        paintText(grid, item.rect.x, item.rect.y, item.rect.right(),
                  item.content, semanticIndex(theme, foregroundRole),
                  background, foregroundRole, style);
    }
    if (!surface.input) return;
    paintText(grid, surface.input->query.x, surface.input->query.y,
              surface.input->query.right(), surface.input->queryText,
              semanticIndex(theme, SemanticRole::Prompt), background,
              SemanticRole::Prompt, style);
    if (surface.input->ghost) {
        paintText(grid, surface.input->ghost->x, surface.input->ghost->y,
                  surface.input->ghost->right(),
                  surface.input->ghostText,
                  semanticIndex(theme, SemanticRole::LineNumber),
                  background, SemanticRole::LineNumber, style);
    }
}

} // namespace ssg
