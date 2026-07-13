#include "tui_fixture.h"

#include <ssg/layout.h>
#include <ssg/syntax.h>

#include <algorithm>
#include <any>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace ssg::tui {
namespace {

std::uint8_t semantic_index(ThemeSnapshot const& theme, SemanticRole role) {
    auto const role_index = static_cast<std::size_t>(role);
    if (role_index >= theme.semantic_indices.size()) {
        throw std::invalid_argument{"screen contains an unknown semantic role"};
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
        throw std::invalid_argument{"screen contains an unknown syntax scope"};
    }
    auto const palette_index = theme.syntax_indices[scope_index];
    if (palette_index >= theme_palette_size) {
        throw std::invalid_argument{
            "syntax scope references a color outside the 16-color palette"};
    }
    return palette_index;
}

std::any command_payload(SemanticInputArguments const& arguments) {
    return std::visit(
        [](auto const& value) -> std::any {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, std::monostate>) {
                return {};
            } else {
                return value;
            }
        },
        arguments);
}

bool starts_with(KeySequence const& sequence, KeySequence const& prefix) {
    return prefix.size() <= sequence.size() &&
           std::equal(prefix.begin(), prefix.end(), sequence.begin());
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
                encoded << "\\x" << std::hex << std::setw(2)
                        << std::setfill('0') << static_cast<unsigned>(byte);
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

void put(ScreenSnapshot& screen, int x, int y, std::string text,
         std::uint8_t foreground, std::uint8_t background, SemanticRole role,
         bool continuation = false) {
    if (x < 0 || y < 0 || x >= screen.size.columns || y >= screen.size.rows) {
        return;
    }
    screen.cells[static_cast<std::size_t>(y * screen.size.columns + x)] = {
        std::move(text), foreground, background, role, continuation};
}

void paint_label(ScreenSnapshot& screen, AccessibilityNode const& node,
                 ThemeSnapshot const& theme, std::uint8_t background) {
    if (node.kind == ShellNodeKind::pane ||
        node.kind == ShellNodeKind::scrollbar || node.rect.height <= 0) {
        return;
    }
    auto foreground = semantic_index(theme, node.role);
    auto run = compute_cell_run(node.label);
    int column = node.rect.x;
    for (auto const& span : run.spans) {
        if (column >= node.rect.right()) break;
        auto text = node.label.substr(span.byte_offset, span.byte_len);
        if (span.kind == CellKind::tab) text = " ";
        if (span.kind == CellKind::control ||
            span.kind == CellKind::invalid_utf8) {
            text = "\xef\xbf\xbd";
        }
        auto const width = std::max<std::uint32_t>(span.cell_width, 1);
        put(screen, column, node.rect.y, text, foreground, background,
            node.role);
        for (std::uint32_t offset = 1; offset < width; ++offset) {
            put(screen, column + static_cast<int>(offset), node.rect.y, "",
                foreground, background, node.role, true);
        }
        column += static_cast<int>(width);
    }
}

void paint_document(ScreenSnapshot& screen, SessionSnapshot const& snapshot,
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
            auto text = std::string{
                line.text.substr(span.byte_offset, span.byte_len)};
            if (span.kind == CellKind::tab) {
                text.assign(span.cell_width, ' ');
            } else if (span.kind == CellKind::control ||
                       span.kind == CellKind::invalid_utf8) {
                text = "\xef\xbf\xbd";
            }
            auto const document_offset =
                line.document_offset + span.byte_offset;
            auto const scope = scope_at(snapshot.sections().syntax,
                                        ByteOffset{document_offset});
            auto const foreground = syntax_index(theme, scope);
            auto const width = std::max<std::uint32_t>(span.cell_width, 1);
            put(screen, column, content.y + static_cast<int>(row_index),
                std::move(text), foreground, background,
                SemanticRole::foreground);
            for (std::uint32_t offset = 1;
                 offset < width && column + static_cast<int>(offset) <
                                       content.right();
                 ++offset) {
                put(screen, column + static_cast<int>(offset),
                    content.y + static_cast<int>(row_index), "", foreground,
                    background, SemanticRole::foreground, true);
            }
            column += static_cast<int>(width);
        }
    }
}

void paint_text(ScreenSnapshot& screen, int x, int y, int right_limit,
                std::string_view text, std::uint8_t foreground,
                std::uint8_t background, SemanticRole role) {
    auto run = compute_cell_run(text);
    int column = x;
    for (auto const& span : run.spans) {
        if (column >= right_limit) break;
        auto piece = std::string{text.substr(span.byte_offset, span.byte_len)};
        if (span.kind == CellKind::tab) {
            piece.assign(span.cell_width, ' ');
        } else if (span.kind == CellKind::control ||
                   span.kind == CellKind::invalid_utf8) {
            piece = "\xef\xbf\xbd";
        }
        auto const width = std::max<std::uint32_t>(span.cell_width, 1);
        put(screen, column, y, std::move(piece), foreground, background, role);
        for (std::uint32_t offset = 1;
             offset < width && column + static_cast<int>(offset) < right_limit;
             ++offset) {
            put(screen, column + static_cast<int>(offset), y, "", foreground,
                background, role, true);
        }
        column += static_cast<int>(width);
    }
}

// Paints the active filesystem provider's visible nodes into the panel: one
// node per row with depth indentation, a twisty for expandable directories,
// and the node label.  Selection highlighting arrives with tree navigation.
void paint_panel_tree(ScreenSnapshot& screen, Rect const& panel,
                      TreeViewState const& tree, ThemeSnapshot const& theme,
                      std::uint8_t background) {
    if (tree.providers.empty() || panel.width <= 0) return;
    auto const& provider = tree.providers.front();
    auto const foreground = semantic_index(theme, SemanticRole::foreground);
    auto const directory = semantic_index(theme, SemanticRole::panel_active);
    int const right_limit = panel.right();
    for (std::size_t index = 0; index < provider.nodes.size(); ++index) {
        if (static_cast<int>(index) >= panel.height) break;
        auto const& view = provider.nodes[index];
        std::string line(view.depth * 2, ' ');
        if (view.node.expandable) {
            line += view.expanded ? "\xe2\x96\xbe " : "\xe2\x96\xb8 ";  // v / >
        }
        line += view.node.label;
        auto const color =
            view.node.kind == TreeNodeKind::directory ? directory : foreground;
        paint_text(screen, panel.x, panel.y + static_cast<int>(index),
                   right_limit, line, color, background,
                   SemanticRole::foreground);
    }
}

void paint_scrollbar(ScreenSnapshot& screen, PaneGeometry const& pane,
                     ViewportViewState const& viewport,
                     ThemeSnapshot const& theme, std::uint8_t background) {
    auto const track =
        semantic_index(theme, SemanticRole::scrollbar_track);
    auto const thumb =
        semantic_index(theme, SemanticRole::scrollbar_thumb);
    for (int row = 0; row < pane.scrollbar.height; ++row) {
        auto const is_thumb =
            row >= static_cast<int>(viewport.scrollbar.thumb_start) &&
            row < static_cast<int>(viewport.scrollbar.thumb_start +
                                   viewport.scrollbar.thumb_size);
        put(screen, pane.scrollbar.x, pane.scrollbar.y + row,
            is_thumb ? "#" : "|", is_thumb ? thumb : track, background,
            is_thumb ? SemanticRole::scrollbar_thumb
                     : SemanticRole::scrollbar_track);
    }
}

}  // namespace

std::optional<SemanticCommand> TerminalInputCapture::capture(
    CommittedText const& text, KeymapViewState const&, std::string_view) {
    reset();
    return semantic_input(text);
}

std::optional<SemanticCommand> TerminalInputCapture::capture(
    KeyStroke const& stroke, KeymapViewState const& keymap,
    std::string_view context) {
    pending_.push_back(stroke);
    bool prefix = false;
    for (auto const& binding : keymap.bindings) {
        if (binding.context != context ||
            !starts_with(binding.sequence, pending_)) {
            continue;
        }
        prefix = true;
        if (binding.sequence == pending_) {
            reset();
            return SemanticCommand{binding.command_id, {}};
        }
    }
    if (!prefix) reset();
    return std::nullopt;
}

std::optional<SemanticCommand> TerminalInputCapture::capture(
    SemanticHitTarget const& target, KeymapViewState const&,
    std::string_view) {
    reset();
    return activate_hit_target(target);
}

void TerminalInputCapture::reset() noexcept { pending_.clear(); }

ScreenCell const& ScreenSnapshot::at(int column, int row) const {
    if (column < 0 || row < 0 || column >= size.columns || row >= size.rows) {
        throw std::out_of_range{"screen cell is outside the grid"};
    }
    return cells[static_cast<std::size_t>(row * size.columns + column)];
}

std::string ScreenSnapshot::canonical() const {
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
            if (cell.text == " " && cell.foreground == 0 &&
                cell.background == 1 &&
                cell.role == SemanticRole::background &&
                !cell.continuation) {
                continue;
            }
            output << "cell " << column << ' ' << row << ' '
                   << static_cast<unsigned>(cell.foreground) << ' '
                   << static_cast<unsigned>(cell.background) << ' '
                   << static_cast<unsigned>(cell.role) << ' '
                   << (cell.continuation ? "~"
                                         : '"' + escaped(cell.text) + '"')
                   << '\n';
        }
    }
    return output.str();
}

ScreenSnapshot render_screen(SessionSnapshot const& snapshot) {
    auto const& shell = snapshot.sections().shell;
    auto const& theme = snapshot.sections().theme;
    if (shell.viewport.columns <= 0 || shell.viewport.rows <= 0) {
        throw std::invalid_argument{"screen viewport must be positive"};
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

    auto const foreground =
        semantic_index(theme, SemanticRole::foreground);
    auto const background =
        semantic_index(theme, SemanticRole::background);
    ScreenSnapshot screen{
        shell.viewport, theme.palette,
        std::vector<ScreenCell>(
            static_cast<std::size_t>(shell.viewport.columns *
                                     shell.viewport.rows),
            ScreenCell{" ", foreground, background,
                       SemanticRole::background, false})};
    for (auto const& node : shell.accessibility_nodes) {
        paint_label(screen, node, theme, background);
    }
    if (shell.panel) {
        paint_panel_tree(screen, *shell.panel, snapshot.sections().tree, theme,
                         background);
    }
    if (!shell.panes.empty()) {
        paint_document(screen, snapshot, shell.panes.front().content, theme,
                       background);
        paint_scrollbar(screen, shell.panes.front(),
                        snapshot.client().viewport, theme, background);
    }
    return screen;
}

TuiClient::TuiClient(EditorSession& session, InvocationPrincipal principal,
                     ViewId view_id, SnapshotProvider snapshot_provider)
    : session_{&session},
      principal_{std::move(principal)},
      view_id_{view_id},
      snapshot_provider_{std::move(snapshot_provider)} {
    if (!snapshot_provider_) {
        throw std::invalid_argument{"TUI snapshot provider is required"};
    }
    auto attached = session_->attach(principal_, view_id_);
    if (!attached.accepted()) throw std::invalid_argument{attached.message};
    try {
        refresh();
    } catch (...) {
        (void)session_->detach(principal_.client_id());
        throw;
    }
}

TuiClient::~TuiClient() {
    if (session_) (void)session_->detach(principal_.client_id());
}

CommandResult TuiClient::submit(SemanticCommand const& command) {
    return submit(command.command_id, command_payload(command.arguments));
}

CommandResult TuiClient::submit(std::string command_id, std::any payload) {
    auto result = session_->dispatch(
        principal_.client_id(),
        {std::move(command_id), snapshot_->revision(), std::move(payload)});
    if (result.accepted()) refresh();
    return result;
}

void TuiClient::refresh() {
    auto next = snapshot_provider_();
    if (next.client().client_id != principal_.client_id() ||
        next.client().view_id != view_id_ ||
        next.revision() != session_->revision()) {
        throw std::logic_error{
            "TUI snapshot provider returned a different attachment or revision"};
    }
    snapshot_ = std::move(next);
}

}  // namespace ssg::tui
