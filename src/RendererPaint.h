#pragma once

#include <ssg/Renderer.h>
#include <ssg/WidgetLayout.h>

namespace ssg {

[[nodiscard]] SemanticRole nodeForeground(const UiSchema& schema,
                                           std::string_view nodeId,
                                           SemanticRole fallback);
[[nodiscard]] SemanticRole nodeBackground(const UiSchema& schema,
                                           std::string_view nodeId,
                                           SemanticRole fallback);
[[nodiscard]] const UiNode* findUiNodeById(const UiNode& node,
                                           std::string_view nodeId);
[[nodiscard]] SemanticRole
chromeGlyphForeground(const UiSchema& schema, std::string_view nodeId,
                      SemanticRole resolvedWidgetRole);
[[nodiscard]] std::uint8_t semanticIndex(const ThemeSnapshot& theme,
                                         SemanticRole role);
[[nodiscard]] std::uint8_t syntaxIndex(const ThemeSnapshot& theme,
                                       SyntaxScope scope);

void put(CellGrid& grid, int x, int y, std::string text,
         std::uint8_t foreground, std::uint8_t background, SemanticRole role,
         bool continuation = false, DiffTint tint = DiffTint::None);
void fillRect(CellGrid& grid, const Rect& rect, std::uint8_t foreground,
              std::uint8_t background, SemanticRole role);
void paintText(CellGrid& grid, int x, int y, int right, std::string_view text,
               std::uint8_t foreground, std::uint8_t background,
               SemanticRole role, const Style& style);
void paintScrollGutter(CellGrid& grid, int x, int y, int height,
                       const ScrollbarMetrics& metrics,
                       const ThemeSnapshot& theme, std::uint8_t background,
                       const Style& style);

void paintExternalModification(
    CellGrid& grid, const ExternalModificationViewState& external,
    const SolvedExternalModificationSurface& solved,
    const ThemeSnapshot& theme, const Style& style,
    SemanticRole foregroundRole, SemanticRole backgroundRole);
void paintTabBar(CellGrid& grid, const SolvedTabBar& solved,
                 const ThemeSnapshot& theme, const Style& style,
                 SemanticRole backgroundRole, std::uint8_t bandForeground,
                 std::uint8_t documentBackground);
void paintUiRegion(CellGrid& grid, const SolvedUiRegion& surface,
                   const ThemeSnapshot& theme, const UiSchema& ui,
                   SemanticRole backgroundRole, const Style& style);

void paintPanelTree(CellGrid& grid, const SolvedPanelSurface& panel,
                    const ThemeSnapshot& theme, std::uint8_t background,
                    bool focused, const Style& style);
void paintPalette(CellGrid& grid, const PaletteReport& palette,
                  const SolvedPaletteSurface& solved,
                  const ThemeSnapshot& theme, std::uint8_t background,
                  const Style& style);
[[nodiscard]] std::optional<GridPosition>
paintPrompt(CellGrid& grid, const UiSchema& schema,
            const SolvedGridTree& layout, const ThemeSnapshot& theme,
            SemanticRole foregroundRole, SemanticRole backgroundRole,
            const Style& style);

void paintDiagnostics(CellGrid& grid, const GridPresentation& snapshot,
                      const Rect& content);
void paintHyperlinks(CellGrid& grid, const GridPresentation& snapshot,
                     const Rect& content);
void paintDocument(CellGrid& grid, const GridPresentation& snapshot,
                   const Rect& content, const ThemeSnapshot& theme,
                   std::uint8_t background, const Style& style,
                   LineLayoutCache& lineCache);
void paintScrollbar(CellGrid& grid, const SolvedDocumentSurface& document,
                    const ViewportViewState& viewport,
                    const ThemeSnapshot& theme, std::uint8_t background,
                    const Style& style);
void paintLineNumbers(CellGrid& grid, const GridPresentation& snapshot,
                      const SolvedDocumentSurface& document,
                      const ThemeSnapshot& theme);
[[nodiscard]] std::optional<GridPosition>
screenCellFor(const ViewportViewState& viewport, const Rect& content,
              std::uint64_t caretLine, std::uint64_t caretCell,
              ByteOffset caretByteOffset);

} // namespace ssg
