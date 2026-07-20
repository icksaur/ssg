#include <ssg/render.h>

#include <ssg/grapheme_layout.h>
#include <ssg/syntax.h>

#include <algorithm>
#include <iomanip>
#include <span>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace ssg {
namespace {

thread_local std::uint64_t gRenderSegmentationCalls = 0;

std::uint8_t semanticIndex(ThemeSnapshot const& theme, SemanticRole role) {
    auto const roleIndex = static_cast<std::size_t>(role);
    if (roleIndex >= theme.semanticIndices.size()) {
        throw std::invalid_argument{"grid contains an unknown semantic role"};
    }
    auto const paletteIndex = theme.semanticIndices[roleIndex];
    if (paletteIndex >= kThemePaletteSize) {
        throw std::invalid_argument{
            "semantic role references a color outside the 16-color palette"};
    }
    return paletteIndex;
}

std::uint8_t syntaxIndex(ThemeSnapshot const& theme, SyntaxScope scope) {
    auto const scopeIndex = static_cast<std::size_t>(scope);
    if (scopeIndex >= theme.syntaxIndices.size()) {
        throw std::invalid_argument{"grid contains an unknown syntax scope"};
    }
    auto const paletteIndex = theme.syntaxIndices[scopeIndex];
    if (paletteIndex >= kThemePaletteSize) {
        throw std::invalid_argument{
            "syntax scope references a color outside the 16-color palette"};
    }
    return paletteIndex;
}

std::string escaped(std::string_view text) {
    std::string result;
    for (unsigned char byte : text) {
        switch (byte) {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (byte < 0x20 || byte == 0x7f) {
                std::ostringstream encoded;
                encoded << "\\x" << std::hex << std::setw(2) << std::setfill('0')
                        << static_cast<unsigned>(byte);
                result += encoded.str();
            } else {
                result.push_back(static_cast<char>(byte));
            }
        }
    }
    return result;
}

struct LogicalLine {
    std::string_view text;
    std::uint64_t documentOffset;
    CellRun cells;
};

// Segment ONLY the logical lines the viewport's visible rows reference, keyed by
// logical line index — O(visible rows) grapheme segmentation, not O(document)
// (M12 INV-render-projection).  Lines are located by a single '\n' byte scan
// (cheap memchr-class work) that stops once past the last referenced line; the
// '\n'-only split matches the viewport's line model (active_cell_runs /
// compute_viewport_unwrapped), so `row.logical_line` indexes the same lines the
// snapshot's viewport was built from.  A `row.logical_line` past the document's
// last line is simply absent (skipped by the caller), matching the old
// out-of-range guard.
std::unordered_map<std::uint32_t, LogicalLine> visibleLogicalLines(
    std::string const& text, std::span<const VisualRow> visibleRows) {
    std::unordered_map<std::uint32_t, LogicalLine> lines;
    if (visibleRows.empty()) return lines;
    std::unordered_set<std::uint32_t> referenced;
    std::uint32_t maxLine = 0;
    for (auto const& row : visibleRows) {
        referenced.insert(row.logicalLine);
        maxLine = std::max(maxLine, row.logicalLine);
    }
    std::uint32_t index = 0;
    std::size_t begin = 0;
    for (;;) {
        auto const end = text.find('\n', begin);
        auto const length =
            end == std::string::npos ? text.size() - begin : end - begin;
        if (referenced.contains(index)) {
            auto const line = std::string_view{text}.substr(begin, length);
            ++gRenderSegmentationCalls;
            lines.emplace(index, LogicalLine{line, begin, GraphemeLayout{}.computeRun(line)});
        }
        if (end == std::string::npos || index >= maxLine) break;
        begin = end + 1;
        ++index;
    }
    return lines;
}

void put(CellGrid& grid, int x, int y, std::string text, std::uint8_t foreground,
         std::uint8_t background, SemanticRole role, bool continuation = false) {
    if (x < 0 || y < 0 || x >= grid.size.columns || y >= grid.size.rows) return;
    grid.cells[static_cast<std::size_t>(y * grid.size.columns + x)] = {
        std::move(text), foreground, background, role, continuation};
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
                SemanticRole role) {
    if (right <= x) return;
    auto run = GraphemeLayout{}.computeRun(text);
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
            piece = "\xef\xbf\xbd";
        }
        put(grid, column, y, std::move(piece), foreground, background, role);
        for (std::uint32_t offset = 1; offset < width; ++offset) {
            put(grid, column + static_cast<int>(offset), y, "", foreground,
                background, role, true);
        }
        column += static_cast<int>(width);
    }
    if (truncated) {
        put(grid, right - 1, y, "\xe2\x80\xa6", foreground, background, role);
    }
}

void paintShellLeaves(CellGrid& grid, ShellViewState const& shell,
                        ThemeSnapshot const& theme, std::uint8_t background,
                        std::uint8_t panelBackground) {
    for (auto const& node : shell.accessibilityNodes) {
        std::uint8_t nodeBackground = background;
        switch (node.kind) {
        case ShellNodeKind::PanelProvider:
            nodeBackground = panelBackground;
            [[fallthrough]];
        case ShellNodeKind::HeaderField:
        case ShellNodeKind::FooterField:
        case ShellNodeKind::FooterAction:
        case ShellNodeKind::Tab:
        case ShellNodeKind::EmptyState:
            if (!node.content.empty()) {
                paintText(grid, node.rect.x, node.rect.y, node.rect.right(),
                           node.content, semanticIndex(theme, node.role),
                           nodeBackground, node.role);
            }
            break;
        default:
            break;  // Containers, panes, and scrollbars are painted elsewhere.
        }
    }
}

// Paint a scrollbar into a reserved 1-column gutter from resolved metrics.  When
// the content fits (`maximum_first_row == 0`) the gutter is left blank (the thumb
// is hidden), so a thumb appearing or vanishing never changes the content width
// (see doc/spec-scroll.md). Otherwise it draws a `|` track with a `#` thumb.
void paintScrollGutter(CellGrid& grid, int x, int y, int height,
                         ScrollbarMetrics const& metrics,
                         ThemeSnapshot const& theme, std::uint8_t background) {
    auto const track = semanticIndex(theme, SemanticRole::ScrollbarTrack);
    auto const thumb = semanticIndex(theme, SemanticRole::ScrollbarThumb);
    bool const scrollable = metrics.maximumFirstRow > 0;
    for (int row = 0; row < height; ++row) {
        if (!scrollable) {
            put(grid, x, y + row, " ", track, background,
                SemanticRole::ScrollbarTrack);
            continue;
        }
        bool const isThumb =
            row >= static_cast<int>(metrics.thumbStart) &&
            row < static_cast<int>(metrics.thumbStart + metrics.thumbSize);
        put(grid, x, y + row, isThumb ? "#" : "|", isThumb ? thumb : track,
            background,
            isThumb ? SemanticRole::ScrollbarThumb
                     : SemanticRole::ScrollbarTrack);
    }
}

void paintPanelTree(CellGrid& grid, Rect const& panel,
                      std::optional<Rect> const& panelScrollbar,
                      TreeViewState const& tree, ThemeSnapshot const& theme,
                      std::uint8_t background, bool focused) {
    if (tree.providers.empty() || panel.width <= 0) return;
    auto const& provider = tree.providers.front();
    auto const foreground = semanticIndex(theme, SemanticRole::Foreground);
    auto const directory = semanticIndex(theme, SemanticRole::PanelActive);
    auto const selectedBg = semanticIndex(theme, SemanticRole::TreeFocus);
    int const top = panel.y + 1;
    int const rows = panel.height - 1;
    // Content stops before the reserved scrollbar gutter so text width is stable.
    int const contentRight =
        panelScrollbar ? panelScrollbar->x : panel.right();
    // Window the visible nodes at the resolved scroll offset.
    for (int row = 0; row < rows; ++row) {
        std::size_t const index =
            static_cast<std::size_t>(provider.firstVisible) +
            static_cast<std::size_t>(row);
        if (index >= provider.nodes.size()) break;
        auto const& view = provider.nodes[index];
        int const y = top + row;
        bool const isSelected =
            provider.selected && view.node.id == *provider.selected;
        auto const rowBackground = isSelected ? selectedBg : background;
        if (isSelected) {
            fillRect(grid, {panel.x, y, contentRight - panel.x, 1}, foreground,
                      rowBackground, SemanticRole::TreeFocus);
            if (focused) grid.caret = GridPosition{panel.x, y};
        }
        std::string line(view.depth * 2, ' ');
        if (view.node.expandable) {
            line += view.expanded ? "\xe2\x96\xbe " : "\xe2\x96\xb8 ";
        }
        line += view.node.label;
        auto const color =
            view.node.kind == TreeNodeKind::Directory ? directory : foreground;
        paintText(grid, panel.x, y, contentRight, line, color, rowBackground,
                   SemanticRole::Foreground);
    }
    // Paint the reserved gutter (blank when the tree fits).
    if (panelScrollbar) {
        paintScrollGutter(grid, panelScrollbar->x, panelScrollbar->y,
                            panelScrollbar->height, provider.scrollbar, theme,
                            background);
    }
}

// Projects the palette's ranked results into the active pane while the palette
// prompt is open.  The query and caret live in the header (see spec-palette.md);
// this paints only the results window with the selected row highlighted.
void paintPalette(CellGrid& grid, PaletteProjection const& palette,
                   ThemeSnapshot const& theme, std::uint8_t background) {
    auto const& rect = palette.rect;
    if (rect.width <= 0 || rect.height <= 0) return;
    auto const foreground = semanticIndex(theme, SemanticRole::Foreground);
    auto const detailColor = semanticIndex(theme, SemanticRole::LineNumber);
    auto const selectedBg = semanticIndex(theme, SemanticRole::Selection);
    // `rows` is already the client's windowed subset; `selected`/`first_visible`
    // are absolute, so the selected row's screen index is selected-first_visible.
    for (std::size_t index = 0; index < palette.rows.size(); ++index) {
        if (static_cast<int>(index) >= rect.height) break;
        auto const& row = palette.rows[index];
        int const y = rect.y + static_cast<int>(index);
        bool const isSelected =
            palette.selected &&
            *palette.selected == palette.firstVisible + index;
        auto const rowBackground = isSelected ? selectedBg : background;
        auto const rowRole =
            isSelected ? SemanticRole::Selection : SemanticRole::Background;
        auto const labelRole =
            isSelected ? SemanticRole::Selection : SemanticRole::Foreground;
        auto const detailRole =
            isSelected ? SemanticRole::Selection : SemanticRole::LineNumber;
        fillRect(grid, {rect.x, y, rect.width, 1}, foreground, rowBackground,
                  rowRole);
        paintText(grid, rect.x, y, rect.right(), row.label, foreground,
                   rowBackground, labelRole);
        if (!row.detail.empty()) {
            auto const run = GraphemeLayout{}.computeRun(row.detail);
            int width = 0;
            for (auto const& span : run.spans) {
                width += static_cast<int>(std::max<std::uint32_t>(span.cellWidth, 1));
            }
            int const start = std::max(rect.x, rect.right() - width);
            paintText(grid, start, y, rect.right(), row.detail, detailColor,
                       rowBackground, detailRole);
        }
    }
    // Paint the reserved gutter (blank when the ranked list fits).
    if (palette.scrollbarRect.width > 0 && palette.scrollbarRect.height > 0) {
        paintScrollGutter(grid, palette.scrollbarRect.x,
                            palette.scrollbarRect.y,
                            palette.scrollbarRect.height, palette.scrollbar,
                            theme, background);
    }
}

// Whether a document byte offset falls inside any ranged (non-caret) selection.
// Caret selections (anchor == active) have no width and are not highlighted.
bool offsetInSelection(SelectionViewState const& selection,
                         std::uint64_t offset) {
    for (auto const& item : selection.selections.items()) {
        if (item.isCaret()) continue;
        auto const lo = item.lower().byteOffset.value();
        auto const hi = item.upper().byteOffset.value();
        if (offset >= lo && offset < hi) return true;
    }
    return false;
}

// The find-match role for a byte offset, honouring precedence: the active match
// wins over any other match.  Returns nullopt when the offset is outside every
// match or find is closed.  The active match reuses the selection role so the
// current hit reads like a selection; other matches use search_match.
std::optional<SemanticRole> findMatchRole(FindReplaceViewState const& find,
                                            std::uint64_t offset) {
    if (!find.open) return std::nullopt;
    for (std::size_t index = 0; index < find.matches.size(); ++index) {
        auto const& match = find.matches[index];
        auto const lo = match.begin.value();
        auto const hi = match.end.value();
        if (offset < lo || offset >= hi) continue;
        bool const active = find.activeMatch && *find.activeMatch == index;
        return active ? SemanticRole::Selection : SemanticRole::SearchMatch;
    }
    return std::nullopt;
}

// The screen cell for a document position (line, cell) within the content rect,
// or nullopt if it is not on a visible row.  Used to place the primary hardware
// cursor and to paint secondary caret cells.
std::optional<GridPosition> screenCellFor(ViewportViewState const& viewport,
                                            Rect const& content,
                                            std::uint32_t caretLine,
                                            std::uint32_t caretCell) {
    std::optional<GridPosition> boundary;  // A match landing at the row's edge.
    for (std::size_t index = 0; index < viewport.visibleRows.size(); ++index) {
        auto const& row = viewport.visibleRows[index];
        if (row.logicalLine != caretLine) continue;
        auto const start = row.startCell.value();
        auto const end = start + row.contentCells;
        if (caretCell < start || caretCell > end) continue;
        int const column = content.x + static_cast<int>(caretCell - start);
        int const screenRow = content.y + static_cast<int>(index);
        if (screenRow < content.y || screenRow >= content.bottom()) continue;
        if (column >= content.x && column < content.right()) {
            return GridPosition{column, screenRow};  // Fits on this row.
        }
        // At a wrap boundary the caret equals this row's inclusive end and lands
        // at content.right(); a later visual row of the same logical line hosts
        // it at column 0.  Remember this edge match but keep scanning for a
        // fitting row before falling back to it.
        if (column == content.right() && !boundary) {
            boundary = GridPosition{content.right() - 1, screenRow};
        }
    }
    return boundary;
}

void paintDocument(CellGrid& grid, SessionSnapshot const& snapshot,
                    Rect const& content, ThemeSnapshot const& theme,
                    std::uint8_t background) {
    auto const& viewport = snapshot.client().viewport;
    auto lines = visibleLogicalLines(snapshot.sections().document.text,
                                       viewport.visibleRows);
    auto const& selection = snapshot.sections().selection;
    auto const& findState = snapshot.sections().findReplace;
    // Find matches are byte offsets into a specific document revision; only paint
    // them when that revision still matches the document being rendered.  A
    // global undo/redo or tab switch during prompt focus moves the document out
    // from under stale offsets, which must not highlight unrelated cells.
    bool const findMatchesCurrent =
        findState.open &&
        findState.sourceRevision == snapshot.sections().document.revision;
    auto const matchRoleAt =
        [&](std::uint64_t offset) -> std::optional<SemanticRole> {
        if (!findMatchesCurrent) return std::nullopt;
        return findMatchRole(findState, offset);
    };
    auto const selectionBg = semanticIndex(theme, SemanticRole::Selection);
    auto const searchMatchBg = semanticIndex(theme, SemanticRole::SearchMatch);
    for (std::size_t rowIndex = 0; rowIndex < viewport.visibleRows.size();
         ++rowIndex) {
        auto const& row = viewport.visibleRows[rowIndex];
        auto const lineIt = lines.find(row.logicalLine);
        if (lineIt == lines.end() ||
            rowIndex >= static_cast<std::size_t>(content.height)) {
            continue;
        }
        auto const& line = lineIt->second;
        int column = content.x;
        auto const lastSpan = std::min<std::size_t>(
            line.cells.spans.size(),
            static_cast<std::size_t>(row.firstSpan) + row.spanCount);
        for (std::size_t spanIndex = row.firstSpan;
             spanIndex < lastSpan && column < content.right(); ++spanIndex) {
            auto const& span = line.cells.spans[spanIndex];
            auto text =
                std::string{line.text.substr(span.byteOffset, span.byteLen)};
            if (span.kind == CellKind::Tab) {
                text.assign(span.cellWidth, ' ');
            } else if (span.kind == CellKind::Control ||
                       span.kind == CellKind::InvalidUtf8) {
                text = "\xef\xbf\xbd";
            }
            auto const documentOffset = line.documentOffset + span.byteOffset;
            auto const scope =
                scopeAt(snapshot.sections().syntax, ByteOffset{documentOffset});
            auto const foreground = syntaxIndex(theme, scope);
            auto const selected = offsetInSelection(selection, documentOffset);
            auto cellBg = selected ? selectionBg : background;
            auto cellRole =
                selected ? SemanticRole::Selection : SemanticRole::Foreground;
            // Find matches take precedence over the text selection so the query
            // hits stay visible; the active match reuses the selection role.
            if (auto matchRole = matchRoleAt(documentOffset)) {
                cellRole = *matchRole;
                cellBg = *matchRole == SemanticRole::Selection ? selectionBg
                                                                 : searchMatchBg;
            }
            auto const width = std::max<std::uint32_t>(span.cellWidth, 1);
            put(grid, column, content.y + static_cast<int>(rowIndex),
                std::move(text), foreground, cellBg, cellRole);
            for (std::uint32_t offset = 1;
                 offset < width &&
                 column + static_cast<int>(offset) < content.right();
                 ++offset) {
                put(grid, column + static_cast<int>(offset),
                    content.y + static_cast<int>(rowIndex), "", foreground,
                    cellBg, cellRole, true);
            }
            column += static_cast<int>(width);
        }
        // A selection spanning into the next line highlights this line's
        // end-of-line: the newline byte at the line's end offset lies inside the
        // selection range, so fill the remaining columns with the selection role.
        // Only the FINAL visual row of a wrapped logical line owns the newline,
        // so gate on this row having painted the line's last span; interior wrap
        // rows must not fill their trailing padding.
        bool const isFinalVisualRow =
            lastSpan >= line.cells.spans.size();
        auto const lineEnd = line.documentOffset + line.text.size();
        // A find match (or text selection) that spans the newline highlights the
        // end-of-line: fill the trailing columns, giving find-role precedence.
        if (isFinalVisualRow) {
            auto const eolMatchRole = matchRoleAt(lineEnd);
            bool const eolSelected = offsetInSelection(selection, lineEnd);
            if (eolMatchRole || eolSelected) {
                auto const role = eolMatchRole ? *eolMatchRole
                                                 : SemanticRole::Selection;
                auto const fillBg =
                    role == SemanticRole::SearchMatch ? searchMatchBg
                                                       : selectionBg;
                auto const foreground =
                    semanticIndex(theme, SemanticRole::Foreground);
                for (int fill = column; fill < content.right(); ++fill) {
                    put(grid, fill, content.y + static_cast<int>(rowIndex), " ",
                        foreground, fillBg, role);
                }
            }
        }
    }
}

void paintScrollbar(CellGrid& grid, PaneGeometry const& pane,
                     ViewportViewState const& viewport,
                     ThemeSnapshot const& theme, std::uint8_t background) {
    paintScrollGutter(grid, pane.scrollbar.x, pane.scrollbar.y,
                        pane.scrollbar.height, viewport.scrollbar, theme,
                        background);
}

// Paint the reserved prompt rows (find/replace/settings/command_argument).  The
// palette is excluded: it renders its query in the header and reserves no rows.
// Returns the screen cell for the text cursor at the end of the first input, so
// the caller can place the hardware cursor when the prompt is focused.
std::optional<GridPosition> paintPrompt(CellGrid& grid,
                                         PromptViewState const& prompt,
                                         ThemeSnapshot const& theme,
                                         std::uint8_t background) {
    auto const promptFg = semanticIndex(theme, SemanticRole::Prompt);
    auto const promptBg = semanticIndex(theme, SemanticRole::Background);
    std::optional<GridPosition> caret;
    for (auto const& control : prompt.controls) {
        std::string text;
        switch (control.kind) {
            case PromptControlKind::Input:
                text = control.accessibleLabel + ": " + control.value;
                break;
            case PromptControlKind::Count:
                text = control.value;
                break;
            case PromptControlKind::Toggle:
                text = std::string{control.checked ? "[x] " : "[ ] "} +
                       control.accessibleLabel;
                break;
        }
        // Clear the row region first so a shrinking value does not leave stale
        // glyphs behind, then paint the control text.
        for (int column = control.rect.x; column < control.rect.right(); ++column) {
            put(grid, column, control.rect.y, " ", promptFg, promptBg,
                SemanticRole::Prompt);
        }
        paintText(grid, control.rect.x, control.rect.y, control.rect.right(),
                   text, promptFg, promptBg, SemanticRole::Prompt);
        // Place the hardware cursor on the editable input: the replacement row
        // for a replace prompt (its query row is display-only), otherwise the
        // first input.
        bool const activeInput =
            prompt.kind == PromptKind::Replace
                ? control.id == "replace.replacement"
                : !caret;
        if (control.kind == PromptControlKind::Input && activeInput && !caret) {
            auto const labelWidth =
                static_cast<int>(GraphemeLayout{}.computeRun(control.accessibleLabel + ": ")
                                     .totalCells);
            auto const valueWidth =
                static_cast<int>(GraphemeLayout{}.computeRun(control.value).totalCells);
            auto const cursorColumn =
                std::min(control.rect.x + labelWidth + valueWidth,
                         control.rect.right() - 1);
            caret = GridPosition{cursorColumn, control.rect.y};
        }
    }
    return caret;
}

// Render the declined-layout ("terminal too small") screen: a placeholder grid
// of the terminal's size carrying a centered library-owned message.  The library
// owns this screen so a client contributes no cell content (M11-L,
// doc/spec-library-contract.md).  Sized from the terminal dimensions the client
// viewport carries, since the shell layout was declined (viewport {0,0}).
CellGrid renderTooSmall(GridSize size, ThemeSnapshot const& theme) {
    auto const foreground = semanticIndex(theme, SemanticRole::Foreground);
    auto const background = semanticIndex(theme, SemanticRole::Background);
    CellGrid grid{
        size, theme.palette,
        std::vector<CellGridCell>(
            static_cast<std::size_t>(std::max(0, size.columns) *
                                     std::max(0, size.rows)),
            CellGridCell{" ", foreground, background, SemanticRole::Background,
                         false})};
    if (size.columns <= 0 || size.rows <= 0) return grid;
    std::string_view const message = "terminal too small";
    auto const messageCells =
        static_cast<int>(GraphemeLayout{}.computeRun(message).totalCells);
    int const row = size.rows / 2;
    int const start = std::max(0, (size.columns - messageCells) / 2);
    paintText(grid, start, row, size.columns, message, foreground, background,
               SemanticRole::Foreground);
    return grid;
}

}  // namespace

CellGridCell const& CellGrid::at(int column, int row) const {
    if (column < 0 || row < 0 || column >= size.columns || row >= size.rows) {
        throw std::out_of_range{"cell is outside the grid"};
    }
    return cells[static_cast<std::size_t>(row * size.columns + column)];
}

std::string CellGrid::canonical() const {
    std::ostringstream output;
    output << "size " << size.columns << ' ' << size.rows << '\n';
    output << "palette";
    for (auto const& color : palette) {
        output << ' ' << std::hex << std::setw(2) << std::setfill('0')
               << static_cast<unsigned>(color.red) << std::setw(2)
               << static_cast<unsigned>(color.green) << std::setw(2)
               << static_cast<unsigned>(color.blue);
    }
    output << std::dec << '\n';
    for (int row = 0; row < size.rows; ++row) {
        for (int column = 0; column < size.columns; ++column) {
            auto const& cell = at(column, row);
            if (cell.text == " " && cell.role == SemanticRole::Background &&
                !cell.continuation) {
                continue;
            }
            output << "cell " << column << ' ' << row << ' '
                   << static_cast<unsigned>(cell.foreground) << ' '
                   << static_cast<unsigned>(cell.background) << ' '
                   << static_cast<unsigned>(cell.role) << ' '
                   << (cell.continuation ? "~" : '"' + escaped(cell.text) + '"')
                   << '\n';
        }
    }
    return output.str();
}

CellGrid render(SessionSnapshot const& snapshot) {
    auto const& shell = snapshot.sections().shell;
    auto const& theme = snapshot.sections().theme;
    for (auto index : theme.semanticIndices) {
        if (index >= kThemePaletteSize) {
            throw std::invalid_argument{
                "semantic role references a color outside the 16-color palette"};
        }
    }
    for (auto index : theme.syntaxIndices) {
        if (index >= kThemePaletteSize) {
            throw std::invalid_argument{
                "syntax scope references a color outside the 16-color palette"};
        }
    }
    if (shell.viewport.columns <= 0 || shell.viewport.rows <= 0) {
        // The shell layout was declined (viewport below the 20x4 minimum): the
        // library renders the too-small placeholder, sized from the terminal
        // dimensions the client viewport carries (M11-L).
        auto const& dimensions = snapshot.client().viewport.dimensions;
        return renderTooSmall(
            GridSize{static_cast<int>(dimensions.columns),
                     static_cast<int>(dimensions.rows)},
            theme);
    }

    auto const foreground = semanticIndex(theme, SemanticRole::Foreground);
    auto const background = semanticIndex(theme, SemanticRole::Background);
    CellGrid grid{
        shell.viewport, theme.palette,
        std::vector<CellGridCell>(
            static_cast<std::size_t>(shell.viewport.columns *
                                     shell.viewport.rows),
            CellGridCell{" ", foreground, background, SemanticRole::Background,
                         false})};

    auto const panelBackground =
        shell.panel ? semanticIndex(theme, SemanticRole::TreeBackground)
                    : background;
    if (shell.panel) {
        fillRect(grid, *shell.panel, foreground, panelBackground,
                  SemanticRole::TreeBackground);
    }

    paintShellLeaves(grid, shell, theme, background, panelBackground);

    if (shell.panel) {
        paintPanelTree(grid, *shell.panel, shell.panelScrollbar,
                         snapshot.sections().tree, theme,
                         panelBackground, shell.focus == FocusTarget::Panel);
    }
    if (!shell.panes.empty()) {
        if (shell.palette) {
            paintPalette(grid, *shell.palette, theme, background);
        } else {
            paintDocument(grid, snapshot, shell.panes.front().content, theme,
                           background);
            paintScrollbar(grid, shell.panes.front(), snapshot.client().viewport,
                            theme, background);

            // Paint the reserved prompt rows (find/replace/settings) and place
            // the hardware cursor at the query when the prompt is focused.
            auto const& prompt = snapshot.sections().promptStatus.prompt;
            if (prompt) {
                auto promptCaret = paintPrompt(grid, *prompt, theme, background);
                if (shell.focus == FocusTarget::Prompt && promptCaret) {
                    grid.caret = *promptCaret;
                }
            }

            // Place the primary caret at its screen cell so the client can position
            // a terminal cursor there, and paint any secondary carets as cells
            // (a terminal has one hardware cursor), but only when the editor is
            // focused.
            if (shell.focus == FocusTarget::Editor) {
                auto const& content = shell.panes.front().content;
                auto const& viewport = snapshot.client().viewport;
                auto const& selections =
                    snapshot.sections().selection.selections;
                auto const& primary = selections.primary();
                if (auto cell = screenCellFor(viewport, content,
                                                primary.active.line.value(),
                                                primary.active.cell.value())) {
                    grid.caret = *cell;
                }
                auto const caretBg = semanticIndex(theme, SemanticRole::Caret);
                // Draw the secondary caret glyph in the selection role: the theme
                // co-visibility constraint already guarantees caret != selection,
                // so the block cursor is legible in every valid theme without a
                // new constraint (the primary caret uses the hardware cursor).
                auto const caretFg = semanticIndex(theme, SemanticRole::Selection);
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
                        caretFg, caretBg, SemanticRole::Caret);
                }
            }
        }
    }
    return grid;
}

std::uint64_t renderSegmentationCalls() { return gRenderSegmentationCalls; }
void resetRenderSegmentationCalls() { gRenderSegmentationCalls = 0; }

}  // namespace ssg
