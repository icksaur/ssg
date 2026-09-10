#include "RendererPaint.h"

#include <ssg/DiffRows.h>
#include <ssg/GraphemeLayout.h>
#include <ssg/Scrollbar.h>
#include <ssg/SyntaxModel.h>

#include <algorithm>
#include <span>
#include <unordered_map>
#include <unordered_set>

namespace ssg {

namespace {

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
    LineLayoutCache& cache) {
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
            lines.emplace(index,
                          LogicalLine{line, begin, cache.run(line, 4)});
        }
        if (end == std::string::npos || index >= maxLine) break;
        begin = end + 1;
        ++index;
    }
    return lines;
}

} // namespace

// A separate pass over already-painted cells rather than a parameter threaded
// through painting: a diagnostic is a DECORATION over whatever the cell already
// shows, so it must not disturb syntax colour, selection, find highlighting or
// a diff tint -- all of which can apply to the same cell.
//
// Only the ACTIVE document's diagnostics are painted, and only when the server
// produced them for the revision on screen: diagnostic ranges are byte offsets
// into a specific revision, and painting stale ones underlines whatever text has
// since moved into those positions.
void paintDiagnostics(CellGrid& grid, GridPresentation const& snapshot,
                       Rect const& content) {
    auto const& lsp = snapshot.lspSync;
    if (lsp.documents.empty()) return;
    auto const& viewport = snapshot.viewport;

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
        if (file.revision != snapshot.documentRevision) continue;
        for (auto const& diagnostic : file.diagnostics) {
            auto const begin =
                lspPositionToByteOffset(snapshot.documentText,
                                        diagnostic.range.start);
            auto const end =
                lspPositionToByteOffset(snapshot.documentText,
                                        diagnostic.range.end);
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
void paintHyperlinks(CellGrid& grid, GridPresentation const& snapshot,
                      Rect const& content) {
    auto const& text = snapshot.documentText;
    auto const& viewport = snapshot.viewport;
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

namespace {

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

} // namespace

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

void paintDocument(CellGrid& grid, GridPresentation const& snapshot,
                    Rect const& content, ThemeSnapshot const& theme,
                    std::uint8_t background, Style const& style,
                    LineLayoutCache& lineCache) {
    auto const& viewport = snapshot.viewport;
    auto const activeDiff =
        snapshot.diff.fileForIdentity(snapshot.diffFileIdentity);
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
    auto lines = visibleLogicalLines(snapshot.documentText,
                                       viewport.visibleRows, lineCache);
    auto const& selection = snapshot.selections;
    auto const& findState = snapshot.findReplace;
    // Find matches are byte offsets into a specific document revision; only paint
    // them when that revision still matches the document being rendered.  A
    // global undo/redo or tab switch during prompt focus moves the document out
    // from under stale offsets, which must not highlight unrelated cells.
    bool const findMatchesCurrent =
        findState.open &&
        findState.sourceRevision == snapshot.documentRevision;
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
           auto const cells = computeCellRun(phantom->text);
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
        // computeCellRun over the SAME merged text purely to know where
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
            auto const cells = computeCellRun(mergedText);
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
                snapshot.syntax.scopeAt(ByteOffset{documentOffset});
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

void paintScrollbar(CellGrid& grid, SolvedDocumentSurface const& document,
                     ViewportViewState const& viewport,
                     ThemeSnapshot const& theme, std::uint8_t background,
                     Style const& style) {
    paintScrollGutter(grid, document.scrollbarGutter.x,
                     document.scrollbarGutter.y,
                     document.scrollbarGutter.height, viewport.scrollbar,
                     theme, background, style);
}

// The left line-number gutter. Each visible row shows
// its 1-indexed logical line number right-aligned with a trailing space; a
// wrapped continuation row (firstSpan != 0) shows a blank gutter; the caret's
// logical line uses the current-line roles.
void paintLineNumbers(CellGrid& grid, GridPresentation const& snapshot,
                      SolvedDocumentSurface const& document,
                      ThemeSnapshot const& theme) {
    if (document.lineNumbers.width <= 0) return;
    auto const& viewport = snapshot.viewport;
    auto const numberFg = semanticIndex(theme, SemanticRole::LineNumber);
    auto const numberBg = semanticIndex(theme, SemanticRole::LineNumberBackground);
    auto const currentFg = semanticIndex(theme, SemanticRole::CurrentLineNumber);
    auto const currentBg =
        semanticIndex(theme, SemanticRole::CurrentLineNumberBackground);
    // Every caret's logical line highlights its gutter number, not just the
    // primary's, so multi-cursor edits show one lit number per cursor.
    auto const& selections = snapshot.selections;
    std::vector<std::uint32_t> caretLines;
    caretLines.reserve(selections.items().size());
    for (auto const& selection : selections.items()) {
        caretLines.push_back(selection.active.line.value());
    }
    auto const isCaretLine = [&](std::uint32_t logicalLine) {
        return std::find(caretLines.begin(), caretLines.end(), logicalLine) !=
               caretLines.end();
    };
    int const width = document.lineNumbers.width;
    for (std::size_t rowIndex = 0; rowIndex < viewport.visibleRows.size();
         ++rowIndex) {
        if (rowIndex >=
            static_cast<std::size_t>(document.lineNumbers.height)) {
            break;
        }
        auto const& row = viewport.visibleRows[rowIndex];
        int const y = document.lineNumbers.y + static_cast<int>(rowIndex);
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
            put(grid, document.lineNumbers.x + i, y,
                std::string{label[i]}, fg, bg, role);
        }
    }
    // Rows below the document content (past the last visible row) get a blank
    // gutter in the inactive gutter background so the column reads as a solid
    // band distinct from the document content.
    for (int y = document.lineNumbers.y +
                 static_cast<int>(viewport.visibleRows.size());
         y < document.lineNumbers.bottom(); ++y) {
        for (int i = 0; i < width; ++i) {
            put(grid, document.lineNumbers.x + i, y, " ", numberFg, numberBg,
                SemanticRole::LineNumber);
        }
    }
}

} // namespace ssg
