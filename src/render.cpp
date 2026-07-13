#include <ssg/render.h>

#include <ssg/layout.h>
#include <ssg/syntax.h>

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace ssg {
namespace {

std::uint8_t semantic_index(ThemeSnapshot const& theme, SemanticRole role) {
    auto const role_index = static_cast<std::size_t>(role);
    if (role_index >= theme.semantic_indices.size()) {
        throw std::invalid_argument{"grid contains an unknown semantic role"};
    }
    auto const palette_index = theme.semantic_indices[role_index];
    if (palette_index >= theme_palette_size) {
        throw std::invalid_argument{
            "semantic role references a color outside the 16-color palette"};
    }
    return palette_index;
}

std::uint8_t syntax_index(ThemeSnapshot const& theme, SyntaxScope scope) {
    auto const scope_index = static_cast<std::size_t>(scope);
    if (scope_index >= theme.syntax_indices.size()) {
        throw std::invalid_argument{"grid contains an unknown syntax scope"};
    }
    auto const palette_index = theme.syntax_indices[scope_index];
    if (palette_index >= theme_palette_size) {
        throw std::invalid_argument{
            "syntax scope references a color outside the 16-color palette"};
    }
    return palette_index;
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
    std::uint64_t document_offset;
    CellRun cells;
};

std::vector<LogicalLine> logical_lines(std::string const& text) {
    std::vector<LogicalLine> result;
    std::size_t begin = 0;
    for (;;) {
        auto const end = text.find_first_of("\r\n", begin);
        auto const length =
            end == std::string::npos ? text.size() - begin : end - begin;
        auto line = std::string_view{text}.substr(begin, length);
        result.push_back({line, begin, compute_cell_run(line)});
        if (end == std::string::npos) break;
        begin = end + 1;
        if (text[end] == '\r' && begin < text.size() && text[begin] == '\n') {
            ++begin;
        }
        if (begin == text.size()) {
            result.push_back({{}, begin, compute_cell_run({})});
            break;
        }
    }
    return result;
}

void put(CellGrid& grid, int x, int y, std::string text, std::uint8_t foreground,
         std::uint8_t background, SemanticRole role, bool continuation = false) {
    if (x < 0 || y < 0 || x >= grid.size.columns || y >= grid.size.rows) return;
    grid.cells[static_cast<std::size_t>(y * grid.size.columns + x)] = {
        std::move(text), foreground, background, role, continuation};
}

void fill_rect(CellGrid& grid, Rect const& rect, std::uint8_t foreground,
               std::uint8_t background, SemanticRole role) {
    for (int y = rect.y; y < rect.bottom(); ++y) {
        for (int x = rect.x; x < rect.right(); ++x) {
            put(grid, x, y, " ", foreground, background, role);
        }
    }
}

// Paints one row of text clipped to [x, right).  Content that does not fit is
// truncated and the last visible cell shows an ellipsis in the same role.
void paint_text(CellGrid& grid, int x, int y, int right, std::string_view text,
                std::uint8_t foreground, std::uint8_t background,
                SemanticRole role) {
    if (right <= x) return;
    auto run = compute_cell_run(text);
    int column = x;
    bool truncated = false;
    for (auto const& span : run.spans) {
        auto const width = std::max<std::uint32_t>(span.cell_width, 1);
        if (column + static_cast<int>(width) > right) {
            truncated = true;
            break;
        }
        auto piece = std::string{text.substr(span.byte_offset, span.byte_len)};
        if (span.kind == CellKind::tab) {
            piece.assign(span.cell_width, ' ');
        } else if (span.kind == CellKind::control ||
                   span.kind == CellKind::invalid_utf8) {
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

void paint_shell_leaves(CellGrid& grid, ShellViewState const& shell,
                        ThemeSnapshot const& theme, std::uint8_t background,
                        std::uint8_t panel_background) {
    for (auto const& node : shell.accessibility_nodes) {
        std::uint8_t node_background = background;
        switch (node.kind) {
        case ShellNodeKind::panel_provider:
            node_background = panel_background;
            [[fallthrough]];
        case ShellNodeKind::header_field:
        case ShellNodeKind::footer_field:
        case ShellNodeKind::footer_action:
        case ShellNodeKind::tab:
        case ShellNodeKind::empty_state:
            if (!node.content.empty()) {
                paint_text(grid, node.rect.x, node.rect.y, node.rect.right(),
                           node.content, semantic_index(theme, node.role),
                           node_background, node.role);
            }
            break;
        default:
            break;  // Containers, panes, and scrollbars are painted elsewhere.
        }
    }
}

// Paints the active filesystem provider's visible nodes below the provider row.
void paint_panel_tree(CellGrid& grid, Rect const& panel,
                      TreeViewState const& tree, ThemeSnapshot const& theme,
                      std::uint8_t background, bool focused) {
    if (tree.providers.empty() || panel.width <= 0) return;
    auto const& provider = tree.providers.front();
    auto const foreground = semantic_index(theme, SemanticRole::foreground);
    auto const directory = semantic_index(theme, SemanticRole::panel_active);
    auto const selected_bg = semantic_index(theme, SemanticRole::tree_focus);
    int const top = panel.y + 1;  // Row 0 shows the provider name.
    int const rows = panel.height - 1;
    for (std::size_t index = 0; index < provider.nodes.size(); ++index) {
        if (static_cast<int>(index) >= rows) break;
        auto const& view = provider.nodes[index];
        int const y = top + static_cast<int>(index);
        bool const is_selected =
            provider.selected && view.node.id == *provider.selected;
        auto const row_background = is_selected ? selected_bg : background;
        if (is_selected) {
            fill_rect(grid, {panel.x, y, panel.width, 1}, foreground,
                      row_background, SemanticRole::tree_focus);
            if (focused) grid.caret = GridPosition{panel.x, y};
        }
        std::string line(view.depth * 2, ' ');
        if (view.node.expandable) {
            line += view.expanded ? "\xe2\x96\xbe " : "\xe2\x96\xb8 ";  // v / >
        }
        line += view.node.label;
        auto const color =
            view.node.kind == TreeNodeKind::directory ? directory : foreground;
        paint_text(grid, panel.x, y, panel.right(), line, color, row_background,
                   SemanticRole::foreground);
    }
}

// Projects the palette's ranked results into the active pane while the palette
// prompt is open.  The query and caret live in the header (see spec-palette.md);
// this paints only the results window with the selected row highlighted.
void paint_palette(CellGrid& grid, PaletteProjection const& palette,
                   ThemeSnapshot const& theme, std::uint8_t background) {
    auto const& rect = palette.rect;
    if (rect.width <= 0 || rect.height <= 0) return;
    auto const foreground = semantic_index(theme, SemanticRole::foreground);
    auto const detail_color = semantic_index(theme, SemanticRole::line_number);
    auto const selected_bg = semantic_index(theme, SemanticRole::selection);
    for (std::size_t index = 0; index < palette.rows.size(); ++index) {
        if (static_cast<int>(index) >= rect.height) break;
        auto const& row = palette.rows[index];
        int const y = rect.y + static_cast<int>(index);
        bool const is_selected =
            palette.selected && *palette.selected == index;
        auto const row_background = is_selected ? selected_bg : background;
        auto const row_role =
            is_selected ? SemanticRole::selection : SemanticRole::background;
        fill_rect(grid, {rect.x, y, rect.width, 1}, foreground, row_background,
                  row_role);
        paint_text(grid, rect.x, y, rect.right(), row.label, foreground,
                   row_background, SemanticRole::foreground);
        if (!row.detail.empty()) {
            auto const run = compute_cell_run(row.detail);
            int width = 0;
            for (auto const& span : run.spans) {
                width += static_cast<int>(std::max<std::uint32_t>(span.cell_width, 1));
            }
            int const start = std::max(rect.x, rect.right() - width);
            paint_text(grid, start, y, rect.right(), row.detail, detail_color,
                       row_background, SemanticRole::line_number);
        }
    }
}

void paint_document(CellGrid& grid, SessionSnapshot const& snapshot,
                    Rect const& content, ThemeSnapshot const& theme,
                    std::uint8_t background) {
    auto lines = logical_lines(snapshot.sections().document.text);
    auto const& viewport = snapshot.client().viewport;
    for (std::size_t row_index = 0; row_index < viewport.visible_rows.size();
         ++row_index) {
        auto const& row = viewport.visible_rows[row_index];
        if (row.logical_line >= lines.size() ||
            row_index >= static_cast<std::size_t>(content.height)) {
            continue;
        }
        auto const& line = lines[row.logical_line];
        int column = content.x;
        auto const last_span = std::min<std::size_t>(
            line.cells.spans.size(),
            static_cast<std::size_t>(row.first_span) + row.span_count);
        for (std::size_t span_index = row.first_span;
             span_index < last_span && column < content.right(); ++span_index) {
            auto const& span = line.cells.spans[span_index];
            auto text =
                std::string{line.text.substr(span.byte_offset, span.byte_len)};
            if (span.kind == CellKind::tab) {
                text.assign(span.cell_width, ' ');
            } else if (span.kind == CellKind::control ||
                       span.kind == CellKind::invalid_utf8) {
                text = "\xef\xbf\xbd";
            }
            auto const document_offset = line.document_offset + span.byte_offset;
            auto const scope =
                scope_at(snapshot.sections().syntax, ByteOffset{document_offset});
            auto const foreground = syntax_index(theme, scope);
            auto const width = std::max<std::uint32_t>(span.cell_width, 1);
            put(grid, column, content.y + static_cast<int>(row_index),
                std::move(text), foreground, background,
                SemanticRole::foreground);
            for (std::uint32_t offset = 1;
                 offset < width &&
                 column + static_cast<int>(offset) < content.right();
                 ++offset) {
                put(grid, column + static_cast<int>(offset),
                    content.y + static_cast<int>(row_index), "", foreground,
                    background, SemanticRole::foreground, true);
            }
            column += static_cast<int>(width);
        }
    }
}

void paint_scrollbar(CellGrid& grid, PaneGeometry const& pane,
                     ViewportViewState const& viewport,
                     ThemeSnapshot const& theme, std::uint8_t background) {
    auto const track = semantic_index(theme, SemanticRole::scrollbar_track);
    auto const thumb = semantic_index(theme, SemanticRole::scrollbar_thumb);
    for (int row = 0; row < pane.scrollbar.height; ++row) {
        auto const is_thumb =
            row >= static_cast<int>(viewport.scrollbar.thumb_start) &&
            row < static_cast<int>(viewport.scrollbar.thumb_start +
                                   viewport.scrollbar.thumb_size);
        put(grid, pane.scrollbar.x, pane.scrollbar.y + row,
            is_thumb ? "#" : "|", is_thumb ? thumb : track, background,
            is_thumb ? SemanticRole::scrollbar_thumb
                     : SemanticRole::scrollbar_track);
    }
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
            if (cell.text == " " && cell.role == SemanticRole::background &&
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
    if (shell.viewport.columns <= 0 || shell.viewport.rows <= 0) {
        throw std::invalid_argument{"grid viewport must be positive"};
    }
    for (auto index : theme.semantic_indices) {
        if (index >= theme_palette_size) {
            throw std::invalid_argument{
                "semantic role references a color outside the 16-color palette"};
        }
    }
    for (auto index : theme.syntax_indices) {
        if (index >= theme_palette_size) {
            throw std::invalid_argument{
                "syntax scope references a color outside the 16-color palette"};
        }
    }

    auto const foreground = semantic_index(theme, SemanticRole::foreground);
    auto const background = semantic_index(theme, SemanticRole::background);
    CellGrid grid{
        shell.viewport, theme.palette,
        std::vector<CellGridCell>(
            static_cast<std::size_t>(shell.viewport.columns *
                                     shell.viewport.rows),
            CellGridCell{" ", foreground, background, SemanticRole::background,
                         false})};

    auto const panel_background =
        shell.panel ? semantic_index(theme, SemanticRole::tree_background)
                    : background;
    if (shell.panel) {
        fill_rect(grid, *shell.panel, foreground, panel_background,
                  SemanticRole::tree_background);
    }

    paint_shell_leaves(grid, shell, theme, background, panel_background);

    if (shell.panel) {
        paint_panel_tree(grid, *shell.panel, snapshot.sections().tree, theme,
                         panel_background, shell.focus == FocusTarget::panel);
    }
    if (!shell.panes.empty()) {
        if (shell.palette) {
            paint_palette(grid, *shell.palette, theme, background);
        } else {
            paint_document(grid, snapshot, shell.panes.front().content, theme,
                           background);
            paint_scrollbar(grid, shell.panes.front(), snapshot.client().viewport,
                            theme, background);

            // Place the primary caret at its screen cell so the client can position
            // a terminal cursor there, but only when the editor is focused.
            if (shell.focus == FocusTarget::editor) {
            auto const& content = shell.panes.front().content;
            auto const& viewport = snapshot.client().viewport;
            auto const& primary = snapshot.sections().selection.selections.primary();
            auto const caret_line = primary.active.line.value();
            auto const caret_cell = primary.active.cell.value();
            for (std::size_t index = 0; index < viewport.visible_rows.size();
                 ++index) {
                auto const& row = viewport.visible_rows[index];
                if (row.logical_line != caret_line) continue;
                auto const start = row.start_cell.value();
                auto const end = start + row.content_cells;
                if (caret_cell < start || caret_cell > end) continue;
                int const column = content.x + static_cast<int>(caret_cell - start);
                int const screen_row = content.y + static_cast<int>(index);
                if (column >= content.x && column < content.right() &&
                    screen_row >= content.y && screen_row < content.bottom()) {
                    grid.caret = GridPosition{column, screen_row};
                }
                break;
            }
            }
        }
    }
    return grid;
}

}  // namespace ssg
