#include <ssg/Renderer.h>

#include <ssg/GraphemeLayout.h>
#include <ssg/SyntaxModel.h>
#include <ssg/Widget.h>

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

const UiNode* findUiNode(const UiNode& node, std::string_view nodeId) {
    if (node.id.value() == nodeId) return &node;
    if (const auto* leaf = std::get_if<UiLeaf>(&node.content);
        leaf && leaf->widget.id == nodeId) {
        return &node;
    }
    const auto* container = std::get_if<UiContainer>(&node.content);
    if (!container) return nullptr;
    for (const auto& child : container->children) {
        if (const auto* found = findUiNode(child, nodeId)) return found;
    }
    return nullptr;
}

SemanticRole chromeGlyphForeground(const UiSchema& schema,
                                   std::string_view nodeId,
                                   SemanticRole resolvedWidgetRole) {
    const auto* node = findUiNode(schema.root, nodeId);
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
    std::string const& text, std::span<const VisualRow> visibleRows,
    LineLayoutCache* cache) {
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
            if (cache) {
                lines.emplace(index, LogicalLine{line, begin, cache->run(line, 4)});
            } else {
                ++gRenderSegmentationCalls;
                lines.emplace(index, LogicalLine{line, begin,
                                                 GraphemeLayout{}.computeRun(line)});
            }
        }
        if (end == std::string::npos || index >= maxLine) break;
        begin = end + 1;
        ++index;
    }
    return lines;
}

// A separate pass over already-painted cells rather than a parameter threaded
// through painting: a diagnostic is a DECORATION over whatever the cell already
// shows, so it must not disturb syntax colour, selection, find highlighting or
// a diff tint -- all of which can apply to the same cell.
//
// Only the ACTIVE document's diagnostics are painted, and only when the server
// produced them for the revision on screen: diagnostic ranges are byte offsets
// into a specific revision, and painting stale ones underlines whatever text has
// since moved into those positions.
void paintDiagnostics(CellGrid& grid, SessionSnapshot const& snapshot,
                       Rect const& content) {
    auto const& lsp = snapshot.sections().lspSync;
    if (lsp.documents.empty()) return;
    auto const& document = snapshot.sections().document;
    auto const& viewport = snapshot.presentation()->viewport;

    // Highest severity wins per cell, so an error is never hidden by a hint
    // that happens to be painted after it.
    auto const rank = [](CellUnderline underline) {
        switch (underline) {
        case CellUnderline::Error: return 3;
        case CellUnderline::Warning: return 2;
        case CellUnderline::Info: return 1;
        case CellUnderline::None: return 0;
        }
        return 0;
    };
    auto const underlineFor = [](std::optional<LspDiagnosticSeverity> severity) {
        // An absent severity means the server did not say; LSP treats that as an
        // error, and under-reporting a real error is the worse mistake.
        if (!severity) return CellUnderline::Error;
        switch (*severity) {
        case LspDiagnosticSeverity::Error: return CellUnderline::Error;
        case LspDiagnosticSeverity::Warning: return CellUnderline::Warning;
        case LspDiagnosticSeverity::Information:
        case LspDiagnosticSeverity::Hint: return CellUnderline::Info;
        }
        return CellUnderline::Error;
    };

    for (auto const& file : lsp.documents) {
        if (file.revision != document.revision) continue;
        for (auto const& diagnostic : file.diagnostics) {
            auto const begin =
                lspPositionToByteOffset(document.text, diagnostic.range.start);
            auto const end =
                lspPositionToByteOffset(document.text, diagnostic.range.end);
            if (!begin.accepted() || !end.accepted()) continue;
            auto const from = begin.offset.value();
            // A zero-width diagnostic still marks something: give it the one
            // cell at its position, or it would be invisible.
            auto const to = std::max(end.offset.value(), from + 1);
            auto const underline = underlineFor(diagnostic.severity);
            for (auto const& target : viewport.hitTargets) {
                if (target.byteOffset < from || target.byteOffset >= to) continue;
                int const x = content.x + static_cast<int>(target.viewportColumn);
                int const y = content.y + static_cast<int>(target.viewportRow);
                if (x < 0 || y < 0 || x >= grid.size.columns ||
                    y >= grid.size.rows) {
                    continue;
                }
                auto& cell =
                    grid.cells[static_cast<std::size_t>(y * grid.size.columns + x)];
                if (rank(underline) > rank(cell.underline)) {
                    cell.underline = underline;
                }
            }
        }
    }
}

// Find URLs in the visible document text and record them as clickable runs.
//
// Detected from the TEXT rather than from syntax, deliberately: a URL is a URL
// in a comment, a string, a markdown link or a plain note, and coupling this to
// one grammar's captures would make it work in markdown and nowhere else.  That
// is the "cheap for any language" the feature asks for.
void paintHyperlinks(CellGrid& grid, SessionSnapshot const& snapshot,
                      Rect const& content) {
    auto const& text = snapshot.sections().document.text;
    auto const& viewport = snapshot.presentation()->viewport;
    if (text.empty() || viewport.hitTargets.empty()) return;

    // Where a URL stops.  Whitespace and the C0 range always end it; the closing
    // bracket family and quotes are excluded so a link written inside markdown's
    // (...) or in a string does not swallow the delimiter.
    auto const terminates = [](unsigned char byte) {
        return byte <= 0x20 || byte == 0x7f || byte == '"' || byte == '\'' ||
               byte == '<' || byte == '>' || byte == ')' || byte == ']' ||
               byte == '}' || byte == '`';
    };
    // What may precede a URL.  A DIFFERENT set: an OPENING bracket or quote
    // starts one (markdown's `](https://...)`, a quoted URL in code), while a
    // letter or digit means the scheme is part of a longer word and not a link.
    auto const opensAUrl = [](unsigned char byte) {
        return byte <= 0x20 || byte == 0x7f || byte == '"' || byte == '\'' ||
               byte == '<' || byte == '(' || byte == '[' || byte == '{' ||
               byte == '`' || byte == ',' || byte == ';' || byte == ':';
    };
    auto const trailingPunctuation = [](unsigned char byte) {
        return byte == '.' || byte == ',' || byte == ';' || byte == ':' ||
               byte == '!' || byte == '?';
    };

    // Byte offset -> the cell showing it, for the cells actually on screen.
    std::unordered_map<std::uint64_t, CellHitTarget const*> onScreen;
    onScreen.reserve(viewport.hitTargets.size());
    for (auto const& target : viewport.hitTargets) {
        onScreen.emplace(target.byteOffset, &target);
    }

    for (auto const scheme : {std::string_view{"https://"},
                              std::string_view{"http://"}}) {
        for (std::size_t at = text.find(scheme); at != std::string::npos;
             at = text.find(scheme, at + 1)) {
            // A scheme immediately after a word character is part of that word,
            // not the start of a URL.
            if (at > 0 && !opensAUrl(static_cast<unsigned char>(text[at - 1]))) {
                continue;
            }
            std::size_t end = at + scheme.size();
            while (end < text.size() &&
                   !terminates(static_cast<unsigned char>(text[end]))) {
                ++end;
            }
            while (end > at + scheme.size() &&
                   trailingPunctuation(static_cast<unsigned char>(text[end - 1]))) {
                --end;
            }
            if (end <= at + scheme.size()) continue;  // Scheme with no host.

            // Emit one run per contiguous span of on-screen cells: a URL that
            // wraps or is scrolled half off the screen still linkifies the part
            // the user can see.
            std::optional<CellHyperlink> run;
            for (std::size_t offset = at; offset < end; ++offset) {
                auto const found = onScreen.find(offset);
                if (found == onScreen.end()) {
                    if (run) grid.hyperlinks.push_back(std::move(*run));
                    run.reset();
                    continue;
                }
                int const x = content.x + static_cast<int>(found->second->viewportColumn);
                int const y = content.y + static_cast<int>(found->second->viewportRow);
                if (run && run->row == y && run->column + run->width == x) {
                    ++run->width;
                    continue;
                }
                if (run) grid.hyperlinks.push_back(std::move(*run));
                run = CellHyperlink{y, x, 1, text.substr(at, end - at)};
            }
            if (run) grid.hyperlinks.push_back(std::move(*run));
        }
    }
}

void put(CellGrid& grid, int x, int y, std::string text, std::uint8_t foreground,
         std::uint8_t background, SemanticRole role, bool continuation = false,
         DiffTint tint = DiffTint::None) {
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

// The input line's caret cell: one column past the last typed character, so it
// marks where the next keystroke lands.  Derived from the SAME published node
// geometry and the same grapheme measurement that positioned the text, so the
// two cannot drift.
//
// The offset is display WIDTH, not byte count: a query may hold multi-byte or
// wide characters, and `size()` would misplace the caret for any of them.
//
// When the query exactly fills its node this lands on the ghost's first cell,
// which is intended -- the ghost is unaccepted suggestion text and the insertion
// point belongs on top of it.  The caret is therefore bounded by the HEADER row,
// not by the query node.
std::optional<GridPosition> inputLineCaret(ShellViewState const& shell) {
    if (!shell.header) return std::nullopt;
    for (auto const& node : shell.accessibilityNodes) {
        if (node.id != "input_line.query") continue;
        // Walk the same spans paintText walks, stopping where it stops: the
        // caret must sit after the last character actually DRAWN, not after the
        // last character in the string, or an over-long query would push it off
        // the end of the rendered text.
        auto const run = GraphemeLayout{}.computeRun(node.content);
        int column = node.rect.x;
        for (auto const& span : run.spans) {
            auto const width =
                static_cast<int>(std::max<std::uint32_t>(span.cellWidth, 1));
            if (column + width > node.rect.right()) break;
            column += width;
        }
        // The caret sits one past the last drawn character. Layout holds a
        // column back for it, so this normally needs no clamping; the bound is
        // a guard against a degenerate header rather than routine behavior.
        column = std::min(column, static_cast<int>(shell.viewport.columns) - 1);
        return GridPosition{std::max(column, 0), node.rect.y};
    }
    return std::nullopt;
}

void paintShellLeaves(CellGrid& grid, ShellViewState const& shell,
                        ThemeSnapshot const& theme, const UiSchema& ui,
                        std::uint8_t background, std::uint8_t panelBackground,
                        std::uint8_t documentBackground, Style const& style) {
    auto const headerBackground = semanticIndex(
        theme,
        nodeBackground(ui, kHeaderNodeId, SemanticRole::HeaderBackground));
    auto const footerBackground = semanticIndex(
        theme,
        nodeBackground(ui, kFooterNodeId, SemanticRole::FooterBackground));
    auto const tabInactiveBackground = semanticIndex(
        theme, nodeBackground(ui, kTabBarNodeId,
                              SemanticRole::TabInactiveBackground));
    auto const noticeFg = semanticIndex(
        theme, nodeForeground(ui, kNoticeNodeId, SemanticRole::Canvas));
    auto const noticeBg = semanticIndex(
        theme,
        nodeBackground(ui, kNoticeNodeId, SemanticRole::StatusWarning));
    auto const externalFg = semanticIndex(
        theme, nodeForeground(ui, kExternalModNodeId, SemanticRole::Canvas));
    auto const externalBg = semanticIndex(
        theme,
        nodeBackground(ui, kExternalModNodeId, SemanticRole::StatusWarning));
    for (auto const& node : shell.accessibilityNodes) {
        std::uint8_t nodeBackground = background;
        switch (node.kind) {
        case ShellNodeKind::PanelProvider:
            nodeBackground = panelBackground;
            [[fallthrough]];
        case ShellNodeKind::HeaderField:
        case ShellNodeKind::FooterField:
        case ShellNodeKind::FooterAction:
        case ShellNodeKind::FooterHint:
        case ShellNodeKind::Tab:
        case ShellNodeKind::TabSeparator:
        case ShellNodeKind::EmptyState:
            // Chrome backgrounds (M-theme): header/footer fields sit on their
            // distinct band; an inactive tab is a light chip, while the active
            // tab keeps the shared Background so it merges into the document.
            if (node.kind == ShellNodeKind::HeaderField) {
                nodeBackground = headerBackground;
            } else if (node.kind == ShellNodeKind::FooterField ||
                       node.kind == ShellNodeKind::FooterAction ||
                       node.kind == ShellNodeKind::FooterHint) {
                nodeBackground = footerBackground;
            } else if (node.kind == ShellNodeKind::Tab ||
                       node.kind == ShellNodeKind::TabSeparator) {
                nodeBackground = node.role == SemanticRole::TabInactive
                                     ? tabInactiveBackground
                                     : documentBackground;
                // Fill the whole chip so the background reads as a solid tab, not
                // just behind the text; the separator fills its gap the same way.
                fillRect(grid, node.rect, semanticIndex(theme, node.role),
                         nodeBackground, node.role);
            }
            if (!node.content.empty()) {
                auto foregroundRole = node.role;
                if (node.kind == ShellNodeKind::HeaderField ||
                    node.kind == ShellNodeKind::FooterField ||
                    node.kind == ShellNodeKind::FooterHint) {
                    foregroundRole =
                        chromeGlyphForeground(ui, node.id, node.role);
                }
                paintText(grid, node.rect.x, node.rect.y, node.rect.right(),
                           node.content, semanticIndex(theme, foregroundRole),
                           nodeBackground, foregroundRole, style);
            }
            break;
        case ShellNodeKind::NoticeBar: {
            // A full-width yellow bar (StatusWarning bg, dark text) painted
            // before its action nodes so their bracketed labels sit on top.
            fillRect(grid, node.rect, noticeFg, noticeBg,
                     SemanticRole::StatusWarning);
            if (!node.content.empty()) {
                paintText(grid, node.rect.x, node.rect.y, node.rect.right(),
                           node.content, noticeFg, noticeBg,
                           SemanticRole::StatusWarning, style);
            }
            break;
        }
        case ShellNodeKind::NoticeAction: {
            paintText(grid, node.rect.x, node.rect.y, node.rect.right(),
                       node.content, noticeFg, noticeBg,
                       SemanticRole::StatusWarning, style);
            break;
        }
        case ShellNodeKind::ExternalModificationBar:
        case ShellNodeKind::ExternalModificationRow: {
            // The header and each file row fill the bar. A normal row uses the
            // StatusWarning band (like the notice); the SELECTED row is painted
            // with the Selection role so it stands out. The action labels sit on
            // top, painted after (their nodes follow this one).
            auto const bg = node.role == SemanticRole::Selection
                                ? semanticIndex(theme, SemanticRole::Selection)
                                : externalBg;
            fillRect(grid, node.rect, externalFg, bg, node.role);
            if (!node.content.empty()) {
                paintText(grid, node.rect.x, node.rect.y, node.rect.right(),
                           node.content, externalFg, bg, node.role, style);
            }
            break;
        }
        case ShellNodeKind::ExternalModificationAction: {
            auto const bg = node.role == SemanticRole::Selection
                                ? semanticIndex(theme, SemanticRole::Selection)
                                : externalBg;
            paintText(grid, node.rect.x, node.rect.y, node.rect.right(),
                       node.content, externalFg, bg, node.role, style);
            break;
        }
        default:
            break;  // Containers, panes, and scrollbars are painted elsewhere.
        }
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

void paintPanelTree(CellGrid& grid, Rect const& panel,
                      std::optional<Rect> const& panelScrollbar,
                      TreeViewState const& tree, TreeWindow const& window,
                      ThemeSnapshot const& theme,
                      std::uint8_t background, bool focused,
                      Style const& style) {
    if (tree.providers.empty() || panel.width <= 0) return;
    auto const& provider = tree.providers.front();
    auto const foreground = semanticIndex(theme, SemanticRole::Text);
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
            static_cast<std::size_t>(window.firstVisible) +
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
        std::string line(view.depth * style.tree.indentPerDepth, ' ');
        if (view.node.expandable) {
            line += view.expanded ? style.tree.expanded : style.tree.collapsed;
        }
        line += view.node.label;
        auto const color =
            view.node.kind == TreeNodeKind::Directory ? directory : foreground;
        paintText(grid, panel.x, y, contentRight, line, color, rowBackground,
                   SemanticRole::Text, style);
    }
    // Paint the reserved gutter (blank when the tree fits).
    if (panelScrollbar) {
        paintScrollGutter(grid, panelScrollbar->x, panelScrollbar->y,
                            panelScrollbar->height, window.scrollbar, theme,
                            background, style);
    }
}

// Projects the palette's ranked results into the active pane while the palette
// prompt is open.  The query and caret live in the header;
// this paints only the results window with the selected row highlighted.
void paintPalette(CellGrid& grid, PaletteProjection const& palette,
                   ThemeSnapshot const& theme, std::uint8_t background,
                   Style const& style) {
    auto const& rect = palette.rect;
    if (rect.width <= 0 || rect.height <= 0) return;
    auto const foreground = semanticIndex(theme, SemanticRole::Text);
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
            isSelected ? SemanticRole::Selection : SemanticRole::Canvas;
        auto const labelRole =
            isSelected ? SemanticRole::Selection : SemanticRole::Text;
        auto const detailRole =
            isSelected ? SemanticRole::Selection : SemanticRole::LineNumber;
        fillRect(grid, {rect.x, y, rect.width, 1}, foreground, rowBackground,
                  rowRole);
        paintText(grid, rect.x, y, rect.right(), row.label, foreground,
                   rowBackground, labelRole, style);
        if (!row.detail.empty()) {
            auto const run = GraphemeLayout{}.computeRun(row.detail);
            int width = 0;
            for (auto const& span : run.spans) {
                width += static_cast<int>(std::max<std::uint32_t>(span.cellWidth, 1));
            }
            int const start = std::max(rect.x, rect.right() - width);
            paintText(grid, start, y, rect.right(), row.detail, detailColor,
                       rowBackground, detailRole, style);
        }
    }
    // Paint the reserved gutter (blank when the ranked list fits).
    if (palette.scrollbarRect.width > 0 && palette.scrollbarRect.height > 0) {
        paintScrollGutter(grid, palette.scrollbarRect.x,
                            palette.scrollbarRect.y,
                            palette.scrollbarRect.height, palette.scrollbar,
                            theme, background, style);
    }
}

// Whether a document byte offset falls inside any ranged (non-caret) selection.
// Caret selections (anchor == active) have no width and are not highlighted.
bool offsetInSelection(SelectionSet const& selection,
                         std::uint64_t offset) {
    for (auto const& item : selection.items()) {
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
                    std::uint8_t background, Style const& style,
                    LineLayoutCache* lineCache) {
    auto const& viewport = snapshot.presentation()->viewport;
    auto const activeDiff =
        snapshot.sections().diff.fileForDocument(snapshot.sections().document);
    std::unordered_map<std::size_t, DiffTint> rowTints;
    std::unordered_map<std::size_t, const DiffLineChange*> targetChanges;
    if (activeDiff) {
        for (auto const& change : activeDiff->get().changedLines) {
            if (!change.targetLine) continue;
            targetChanges[*change.targetLine] = &change;
            switch (change.kind) {
            case DiffLineKind::Added:
                rowTints[*change.targetLine] = DiffTint::AddedRow;
                break;
            case DiffLineKind::Modified:
                rowTints[*change.targetLine] = DiffTint::ModifiedRow;
                break;
            case DiffLineKind::Removed: break;
            }
        }
    }
    auto lines = visibleLogicalLines(snapshot.sections().document.text,
                                       viewport.visibleRows, lineCache);
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
        if (rowIndex >= static_cast<std::size_t>(content.height)) continue;
        auto const& row = viewport.visibleRows[rowIndex];
        auto const projected = viewport.projectedRow(
           static_cast<std::uint32_t>(rowIndex));
        if (auto const* phantom = std::get_if<PhantomRow>(&projected)) {
           auto const foreground =
               semanticIndex(theme, SemanticRole::Text);
           auto const cells = GraphemeLayout{}.computeRun(phantom->text);
           int column = content.x;
           std::size_t firstSpan = 0;
           std::uint32_t startCell = 0;
           for (; firstSpan < cells.spans.size(); ++firstSpan) {
               if (startCell >= viewport.firstVisualColumn) break;
               startCell += cells.spans[firstSpan].cellWidth;
           }
           for (std::size_t spanIndex = firstSpan;
                spanIndex < cells.spans.size(); ++spanIndex) {
               if (column >= content.right()) break;
               auto const& span = cells.spans[spanIndex];
               auto text = std::string{
                   std::string_view{phantom->text}.substr(span.byteOffset,
                                                          span.byteLen)};
               if (span.kind == CellKind::Tab) {
                   text.assign(span.cellWidth, ' ');
               } else if (span.kind == CellKind::Control ||
                          span.kind == CellKind::InvalidUtf8) {
                   text = style.unrenderable;
               }
               // A Modified pair's baseline text is only ever visible here
               // (its target/edited line is the real row below); mark the
               // specific removed words more strongly than the flat
               // RemovedRow wash, mirroring how AddedWord/ModifiedWord
               // outrank ModifiedRow on the real row.
               auto const spanEnd = span.byteOffset + span.byteLen;
               auto const marked = std::any_of(
                   phantom->removedWordRanges.begin(),
                   phantom->removedWordRanges.end(),
                   [&](const DiffWordRange& range) {
                       auto const rangeEnd = range.byteStart + range.byteLength;
                       return span.byteOffset < rangeEnd &&
                              range.byteStart < spanEnd;
                   });
               auto const cellTint =
                   marked ? DiffTint::RemovedWord : DiffTint::RemovedRow;
               auto const width = std::max<std::uint32_t>(span.cellWidth, 1);
               put(grid, column, content.y + static_cast<int>(rowIndex),
                   std::move(text), foreground, background,
                   SemanticRole::Text, false, cellTint);
               for (std::uint32_t offset = 1;
                    offset < width &&
                    column + static_cast<int>(offset) < content.right();
                    ++offset) {
                   put(grid, column + static_cast<int>(offset),
                       content.y + static_cast<int>(rowIndex), "", foreground,
                       background, SemanticRole::Text, true,
                       cellTint);
               }
               column += static_cast<int>(width);
           }
           for (; column < content.right(); ++column) {
               put(grid, column, content.y + static_cast<int>(rowIndex), " ",
                   foreground, background, SemanticRole::Text, false,
                   DiffTint::RemovedRow);
           }
           continue;
        }
        auto const& real = std::get<RealRow>(projected);
        auto const tintIt = rowTints.find(real.bufferLine);
        auto const rowTint =
           tintIt == rowTints.end() ? DiffTint::None : tintIt->second;
        auto const changeIt = targetChanges.find(real.bufferLine);
        auto const* lineChange =
            changeIt == targetChanges.end() ? nullptr : changeIt->second;
        auto const lineIt = lines.find(row.logicalLine);
        if (lineIt == lines.end()) continue;
        auto const& line = lineIt->second;
        auto const lastSpan = std::min<std::size_t>(
            line.cells.spans.size(),
            static_cast<std::size_t>(row.firstSpan) + row.spanCount);
        // A Modified line rendered as ONE merged inline row (git-diff
        // --word-diff style: baseline-removed and target-added/changed words
        // shown inline, red-then-green, instead of a separate phantom row
        // above a plain target row). Viewport is the sole decider of WHICH
        // rows merge and the sole owner of hit-testing/caret/selection byte
        // mapping for the ghost (Removed/Separator) segments this
        // introduces (see RealRow::mergedSegments); this branch only paints
        // the segments Viewport already computed, recomputing a
        // GraphemeLayout run over the SAME merged text purely to know where
        // to draw each cell -- not to decide layout or byte offsets.
        // Word wrap, selection, and find-match highlighting are not painted
        // on a merged row (selection/find BYTE ranges still resolve
        // correctly via Viewport's hit targets; only the visual highlight
        // wash is not drawn here yet) -- a non-goal for this increment,
        // matching the spec's explicit unwrapped-only scope.
        if (!real.mergedSegments.empty()) {
            std::string mergedText;
            struct SegmentBounds {
                std::size_t start;
                std::size_t end;
                InlineWordSegment::Kind kind;
            };
            std::vector<SegmentBounds> bounds;
            bounds.reserve(real.mergedSegments.size());
            for (auto const& segment : real.mergedSegments) {
                const auto start = mergedText.size();
                mergedText += segment.text;
                bounds.push_back({start, mergedText.size(), segment.kind});
            }
            auto const foreground = semanticIndex(theme, SemanticRole::Text);
            auto const cells = GraphemeLayout{}.computeRun(mergedText);
            int column = content.x;
            std::size_t firstSpan = 0;
            std::uint32_t startCell = 0;
            for (; firstSpan < cells.spans.size(); ++firstSpan) {
                if (startCell >= viewport.firstVisualColumn) break;
                startCell += cells.spans[firstSpan].cellWidth;
            }
            for (std::size_t spanIndex = firstSpan;
                 spanIndex < cells.spans.size(); ++spanIndex) {
                if (column >= content.right()) break;
                auto const& span = cells.spans[spanIndex];
                auto text = std::string{
                    std::string_view{mergedText}.substr(span.byteOffset,
                                                        span.byteLen)};
                if (span.kind == CellKind::Tab) {
                    text.assign(span.cellWidth, ' ');
                } else if (span.kind == CellKind::Control ||
                           span.kind == CellKind::InvalidUtf8) {
                    text = style.unrenderable;
                }
                auto segmentKind = InlineWordSegment::Kind::Unchanged;
                for (auto const& bound : bounds) {
                    if (span.byteOffset >= bound.start && span.byteOffset < bound.end) {
                        segmentKind = bound.kind;
                        break;
                    }
                }
                auto const cellTint =
                    segmentKind == InlineWordSegment::Kind::Removed
                        ? DiffTint::RemovedWord
                    : segmentKind == InlineWordSegment::Kind::Added
                        ? DiffTint::AddedWord
                        : DiffTint::ModifiedRow;
                auto const width = std::max<std::uint32_t>(span.cellWidth, 1);
                put(grid, column, content.y + static_cast<int>(rowIndex),
                    std::move(text), foreground, background,
                    SemanticRole::Text, false, cellTint);
                for (std::uint32_t offset = 1;
                     offset < width &&
                     column + static_cast<int>(offset) < content.right();
                     ++offset) {
                    put(grid, column + static_cast<int>(offset),
                        content.y + static_cast<int>(rowIndex), "", foreground,
                        background, SemanticRole::Text, true, cellTint);
                }
                column += static_cast<int>(width);
            }
            for (; column < content.right(); ++column) {
                put(grid, column, content.y + static_cast<int>(rowIndex), " ",
                    foreground, background, SemanticRole::Text, false,
                    DiffTint::ModifiedRow);
            }
            continue;
        }
        int column = content.x;
        for (std::size_t spanIndex = row.firstSpan;
             spanIndex < lastSpan && column < content.right(); ++spanIndex) {
            auto const& span = line.cells.spans[spanIndex];
            auto text =
                std::string{line.text.substr(span.byteOffset, span.byteLen)};
            if (span.kind == CellKind::Tab) {
                text.assign(span.cellWidth, ' ');
            } else if (span.kind == CellKind::Control ||
                       span.kind == CellKind::InvalidUtf8) {
                text = style.unrenderable;
            }
            auto const documentOffset = line.documentOffset + span.byteOffset;
            auto const scope =
                snapshot.sections().syntax.scopeAt(ByteOffset{documentOffset});
            auto const foreground = syntaxIndex(theme, scope);
            auto const selected = offsetInSelection(selection, documentOffset);
            auto cellBg = selected ? selectionBg : background;
            auto cellRole =
                selected ? SemanticRole::Selection : SemanticRole::Text;
            // Find matches take precedence over the text selection so the query
            // hits stay visible; the active match reuses the selection role.
            auto const matchRole = matchRoleAt(documentOffset);
            if (matchRole) {
                cellRole = *matchRole;
                cellBg = *matchRole == SemanticRole::Selection ? selectionBg
                                                                 : searchMatchBg;
            }
            const auto overlaps = [&](const DiffWordRange& range) {
                const auto spanEnd = span.byteOffset + span.byteLen;
                const auto rangeEnd = range.byteStart + range.byteLength;
                return span.byteOffset < rangeEnd && range.byteStart < spanEnd;
            };
            auto wordTint = DiffTint::None;
            if (lineChange) {
                // Only three diff colors exist (added/removed/modified row);
                // a word-level mark inside a modified line reuses the Added
                // color directly for BOTH a purely inserted span and a
                // changed-in-place span -- both are "new content in the
                // target", so both read as the same green highlight over
                // the row's own modified wash. There is no separate fourth
                // "modified word" shade.
                if (std::any_of(lineChange->targetAddedWordRanges.begin(),
                                lineChange->targetAddedWordRanges.end(),
                                overlaps) ||
                    std::any_of(lineChange->targetModifiedWordRanges.begin(),
                                lineChange->targetModifiedWordRanges.end(),
                                overlaps)) {
                    wordTint = DiffTint::AddedWord;
                }
            }
            auto const cellTint =
                selected || matchRole
                    ? DiffTint::None
                    : wordTint != DiffTint::None ? wordTint : rowTint;
            auto const width = std::max<std::uint32_t>(span.cellWidth, 1);
            put(grid, column, content.y + static_cast<int>(rowIndex),
                std::move(text), foreground, cellBg, cellRole, false, cellTint);
            for (std::uint32_t offset = 1;
                 offset < width &&
                 column + static_cast<int>(offset) < content.right();
                 ++offset) {
                put(grid, column + static_cast<int>(offset),
                    content.y + static_cast<int>(rowIndex), "", foreground,
                   cellBg, cellRole, true, cellTint);
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
        auto const eolMatchRole =
            isFinalVisualRow ? matchRoleAt(lineEnd) : std::nullopt;
        bool const eolSelected =
            isFinalVisualRow && offsetInSelection(selection, lineEnd);
        if (rowTint != DiffTint::None || eolMatchRole || eolSelected) {
            auto const role = eolMatchRole   ? *eolMatchRole
                              : eolSelected ? SemanticRole::Selection
                                            : SemanticRole::Text;
            auto const fillBg =
                role == SemanticRole::SearchMatch
                    ? searchMatchBg
                    : role == SemanticRole::Selection ? selectionBg : background;
            auto const fillTint =
                eolMatchRole || eolSelected ? DiffTint::None : rowTint;
            auto const foreground =
                semanticIndex(theme, SemanticRole::Text);
            for (int fill = column; fill < content.right(); ++fill) {
                put(grid, fill, content.y + static_cast<int>(rowIndex), " ",
                    foreground, fillBg, role, false, fillTint);
            }
        }
    }
}

void paintScrollbar(CellGrid& grid, PaneGeometry const& pane,
                     ViewportViewState const& viewport,
                     ThemeSnapshot const& theme, std::uint8_t background,
                     Style const& style) {
    paintScrollGutter(grid, pane.scrollbar.x, pane.scrollbar.y,
                        pane.scrollbar.height, viewport.scrollbar, theme,
                        background, style);
}

// The left line-number gutter. Each visible row shows
// its 1-indexed logical line number right-aligned with a trailing space; a
// wrapped continuation row (firstSpan != 0) shows a blank gutter; the caret's
// logical line uses the current-line roles.
void paintLineNumbers(CellGrid& grid, SessionSnapshot const& snapshot,
                       PaneGeometry const& pane, ThemeSnapshot const& theme) {
    if (pane.lineNumbers.width <= 0) return;
    auto const& viewport = snapshot.presentation()->viewport;
    auto const numberFg = semanticIndex(theme, SemanticRole::LineNumber);
    auto const numberBg = semanticIndex(theme, SemanticRole::LineNumberBackground);
    auto const currentFg = semanticIndex(theme, SemanticRole::CurrentLineNumber);
    auto const currentBg =
        semanticIndex(theme, SemanticRole::CurrentLineNumberBackground);
    // Every caret's logical line highlights its gutter number, not just the
    // primary's, so multi-cursor edits show one lit number per cursor.
    auto const& selections = snapshot.sections().selection;
    std::vector<std::uint32_t> caretLines;
    caretLines.reserve(selections.items().size());
    for (auto const& selection : selections.items()) {
        caretLines.push_back(selection.active.line.value());
    }
    auto const isCaretLine = [&](std::uint32_t logicalLine) {
        return std::find(caretLines.begin(), caretLines.end(), logicalLine) !=
               caretLines.end();
    };
    int const width = pane.lineNumbers.width;
    for (std::size_t rowIndex = 0; rowIndex < viewport.visibleRows.size();
         ++rowIndex) {
        if (rowIndex >= static_cast<std::size_t>(pane.lineNumbers.height)) break;
        auto const& row = viewport.visibleRows[rowIndex];
        int const y = pane.lineNumbers.y + static_cast<int>(rowIndex);
        bool const isCurrent = isCaretLine(row.logicalLine);
        auto const fg = isCurrent ? currentFg : numberFg;
        auto const bg = isCurrent ? currentBg : numberBg;
        auto const role =
            isCurrent ? SemanticRole::CurrentLineNumber : SemanticRole::LineNumber;
        // Only the first visual row of a logical line shows the number; wrapped
        // continuation rows show a blank gutter.
        std::string label(static_cast<std::size_t>(width), ' ');
        if (row.firstSpan == 0) {
            auto const number = std::to_string(row.logicalLine + 1);
            // Right-align in width-1 columns, then a trailing space.
            if (number.size() <= static_cast<std::size_t>(width - 1)) {
                auto const pad = static_cast<std::size_t>(width - 1) -
                                 number.size();
                for (std::size_t i = 0; i < number.size(); ++i) {
                    label[pad + i] = number[i];
                }
            }
        }
        for (int i = 0; i < width; ++i) {
            put(grid, pane.lineNumbers.x + i, y, std::string{label[i]}, fg, bg,
                role);
        }
    }
    // Rows below the document content (past the last visible row) get a blank
    // gutter in the inactive gutter background so the column reads as a solid
    // band distinct from the document content.
    for (int y = pane.lineNumbers.y +
                 static_cast<int>(viewport.visibleRows.size());
         y < pane.lineNumbers.bottom(); ++y) {
        for (int i = 0; i < width; ++i) {
            put(grid, pane.lineNumbers.x + i, y, " ", numberFg, numberBg,
                SemanticRole::LineNumber);
        }
    }
}

// Paint the reserved prompt rows (find/replace/settings/command_argument).  The
// palette is excluded: it renders its query in the header and reserves no rows.
// Returns the screen cell for the text cursor at the end of the first input, so
// the caller can place the hardware cursor when the prompt is focused.
std::optional<GridPosition> paintPrompt(CellGrid& grid,
                                         PromptViewState const& prompt,
                                         ThemeSnapshot const& theme,
                                         SemanticRole foregroundRole,
                                         SemanticRole backgroundRole,
                                         Style const& style) {
    auto const promptFg = semanticIndex(theme, foregroundRole);
    auto const promptBg = semanticIndex(theme, backgroundRole);
    std::optional<GridPosition> caret;
    for (auto const& control : prompt.controls) {
        std::string text;
        switch (control.kind) {
            case PromptControlKind::Input:
                text = textInputText(control.accessibleLabel,
                                     style.promptLabelSeparator, control.value);
                break;
            case PromptControlKind::Count:
                text = control.value;
                break;
            case PromptControlKind::Toggle:
                text = checkboxText(control.checked, control.accessibleLabel,
                                    style.toggle);
                break;
        }
        // Clear the row region first so a shrinking value does not leave stale
        // glyphs behind, then paint the control text.
        for (int column = control.rect.x; column < control.rect.right(); ++column) {
            put(grid, column, control.rect.y, " ", promptFg, promptBg,
                SemanticRole::Prompt);
        }
        paintText(grid, control.rect.x, control.rect.y, control.rect.right(),
                   text, promptFg, promptBg, SemanticRole::Prompt, style);
        // Place the hardware cursor on the editable input: the replacement row
        // for a replace prompt (its query row is display-only), otherwise the
        // first input.
        bool const activeInput =
            prompt.kind == PromptKind::Replace
                ? control.id == "replace.replacement"
                : !caret;
        if (control.kind == PromptControlKind::Input && activeInput && !caret) {
            auto const labelWidth =
                static_cast<int>(GraphemeLayout{}
                                     .computeRun(textInputText(
                                         control.accessibleLabel,
                                         style.promptLabelSeparator, {}))
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
        static_cast<int>(GraphemeLayout{}.computeRun(message).totalCells);
    int const row = size.rows / 2;
    int const start = std::max(0, (size.columns - messageCells) / 2);
    paintText(grid, start, row, size.columns, message, foreground, background,
               SemanticRole::Text, style);
    return grid;
}

}  // namespace

CellGridCell const& CellGrid::at(int column, int row) const {
    if (column < 0 || row < 0 || column >= size.columns || row >= size.rows) {
        throw std::out_of_range{"cell is outside the grid"};
    }
    return cells[static_cast<std::size_t>(row * size.columns + column)];
}

// Deliberately omits literal palette/diffTints/selectionFill RGB values: those
// are theme tuning, which changes often and independently of layout/content
// correctness, and per-cell lines already reference palette INDEX + symbolic
// tint kind (not resolved colors) -- coupling this golden to exact color
// bytes would break every legitimate color tweak for no structural reason.
// Theme color values are tested directly in test_theme.cpp instead.
std::string CellGrid::canonical() const {
    std::ostringstream output;
    output << "size " << size.columns << ' ' << size.rows << '\n';
    for (int row = 0; row < size.rows; ++row) {
        for (int column = 0; column < size.columns; ++column) {
            auto const& cell = at(column, row);
            if (cell.text == " " && cell.role == SemanticRole::Canvas &&
                !cell.continuation && cell.tint == DiffTint::None) {
                continue;
            }
            output << "cell " << column << ' ' << row << ' '
                   << static_cast<unsigned>(cell.foreground) << ' '
                   << static_cast<unsigned>(cell.background) << ' '
                   << static_cast<unsigned>(cell.role) << ' '
                   << (cell.continuation ? "~" : '"' + escaped(cell.text) + '"')
                   << (cell.tint == DiffTint::None
                           ? ""
                           : " tint " +
                                 std::to_string(static_cast<unsigned>(cell.tint)))
                   << '\n';
        }
    }
    return output.str();
}

CellGrid Renderer::render(SessionSnapshot const& snapshot,
                          LineLayoutCache* lineCache) const {
    auto const& shell = snapshot.presentation()->shell;
    auto const& theme = snapshot.sections().theme;
    auto const& style = snapshot.presentation()->style;
    if (shell.viewport.columns <= 0 || shell.viewport.rows <= 0) {
        // The shell layout was declined (viewport below the 20x4 minimum): the
        // library renders the too-small placeholder, sized from the terminal
        // dimensions the client viewport carries (M11-L).
        auto const& dimensions = snapshot.presentation()->viewport.dimensions;
        return renderTooSmall(
            GridSize{static_cast<int>(dimensions.columns),
                     static_cast<int>(dimensions.rows)},
            theme, style);
    }

    const auto& ui = snapshot.sections().ui;
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
    CellGrid grid{
        shell.viewport, themeColorTable(theme),
        std::vector<CellGridCell>(
            static_cast<std::size_t>(shell.viewport.columns *
                                     shell.viewport.rows),
            CellGridCell{" ", foreground, background, SemanticRole::Canvas,
                         false})};
    grid.diffTints = themeDiffTints(theme);
    grid.selectionFill = theme.color(SemanticRole::Selection);

    auto const panelBackground =
        shell.panel ? semanticIndex(
                          theme, nodeBackground(ui, kPanelNodeId,
                                                SemanticRole::TreeBackground))
                    : background;
    if (shell.panel) {
        const auto panelBackgroundRole =
            nodeBackground(ui, kPanelNodeId, SemanticRole::TreeBackground);
        fillRect(grid, *shell.panel, foreground, panelBackground,
                  panelBackgroundRole);
    }

    // The header and footer are solid chrome bands distinct from the document,
    // so fill their whole rows first; the field text then paints on the band and
    // the gaps between fields carry the band colour rather than the document
    // background.
    if (shell.header) {
        const auto role =
            nodeBackground(ui, kHeaderNodeId, SemanticRole::HeaderBackground);
        fillRect(grid, *shell.header, foreground,
                 semanticIndex(theme, role), role);
    }
    if (shell.footer) {
        const auto role =
            nodeBackground(ui, kFooterNodeId, SemanticRole::FooterBackground);
        fillRect(grid, *shell.footer, foreground,
                 semanticIndex(theme, role), role);
    }
    // The tab bar shares the inactive-tab background across its whole width, so
    // its empty region (past the last tab) reads as inactive chrome rather than
    // as the active tab. Each tab then overpaints its own chip: an inactive tab
    // blends into this band; the active tab cuts a Background-coloured notch that
    // merges with the document.
    if (shell.tabBar) {
        const auto role =
            nodeBackground(ui, kTabBarNodeId,
                           SemanticRole::TabInactiveBackground);
        fillRect(grid, *shell.tabBar, foreground,
                 semanticIndex(theme, role), role);
    }

    paintShellLeaves(grid, shell, theme, ui, background, panelBackground,
                     documentBackground, style);

    if (shell.panel) {
        // The tree window (grid projection) lives in presentation; it holds the
        // active provider's scroll offset and thumb. Empty when no provider or no
        // presentation (a native-layout client never calls this renderer).
        static TreeWindow const emptyWindow{};
        auto const& windows = snapshot.presentation()->treeWindows;
        auto const& window = windows.empty() ? emptyWindow : windows.front();
        paintPanelTree(grid, *shell.panel, shell.panelScrollbar,
                         snapshot.sections().tree, window, theme,
                         panelBackground, snapshot.sections().focus == FocusTarget::Panel,
                         style);
    }
    if (!shell.panes.empty()) {
        fillRect(grid, shell.panes.front().content, foreground,
                 documentBackground, documentBackgroundRole);
        if (shell.palette) {
            const auto paletteBackground = semanticIndex(
                theme, nodeBackground(ui, kFindResultsNodeId,
                                      SemanticRole::Canvas));
            paintPalette(grid, *shell.palette, theme, paletteBackground, style);
        } else {
            paintDocument(grid, snapshot, shell.panes.front().content, theme,
                           documentBackground, style, lineCache);
            // After the document: a diagnostic underlines whatever the cell
            // already shows rather than replacing it.
            paintDiagnostics(grid, snapshot, shell.panes.front().content);
            paintHyperlinks(grid, snapshot, shell.panes.front().content);
            paintLineNumbers(grid, snapshot, shell.panes.front(), theme);
            paintScrollbar(grid, shell.panes.front(), snapshot.presentation()->viewport,
                            theme, documentBackground, style);

            // Paint the reserved prompt rows (find/replace/settings) and place
            // the hardware cursor at the query when the prompt is focused.
            auto const& prompt = snapshot.presentation()->prompt;
            if (prompt) {
                const auto promptForegroundRole =
                    nodeForeground(ui, kFooterPromptNodeId,
                                   SemanticRole::Prompt);
                const auto promptBackgroundRole =
                    nodeBackground(ui, kFooterPromptNodeId,
                                   SemanticRole::Canvas);
                auto promptCaret =
                    paintPrompt(grid, *prompt, theme, promptForegroundRole,
                                promptBackgroundRole, style);
                if (snapshot.sections().focus == FocusTarget::Prompt && promptCaret) {
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
            if (snapshot.sections().focus == FocusTarget::Editor) {
                auto const& content = shell.panes.front().content;
                auto const& viewport = snapshot.presentation()->viewport;
                auto const& selections =
                    snapshot.sections().selection;
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
    // picker paints its RESULTS through paintPalette (the `shell.palette`
    // branch), so a caret placed beside the prompt rows in the `else` branch
    // would never be reached while a picker is open.  The cursor is the primary
    // way a user can tell a text input has focus, so it must
    // not depend on which pane branch ran.
    if (snapshot.sections().focus == FocusTarget::Prompt) {
        if (auto caret = inputLineCaret(shell)) grid.caret = *caret;
    }
    return grid;
}

std::uint64_t Renderer::renderSegmentationCalls() {
    return gRenderSegmentationCalls;
}

void Renderer::resetRenderSegmentationCalls() { gRenderSegmentationCalls = 0; }

}  // namespace ssg
