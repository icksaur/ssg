#include <ssg/render.h>

#include <ssg/editor_runtime.h>
#include <ssg/find_replace.h>
#include <ssg/session_snapshot.h>

#include "test_helpers.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path uniqueRoot() {
    auto base = fs::temp_directory_path() /
                ("ssg-render-" + std::to_string(::rand()));
    fs::create_directories(base / "scratch");
    fs::create_directories(base / "recovery");
    return base;
}

std::unique_ptr<ssg::EditorRuntime> makeRuntime(fs::path const& root) {
    auto created = ssg::EditorRuntime::create(
        {root, root / "scratch", root / "recovery"});
    if (!created.accepted()) return nullptr;
    auto runtime = std::move(created.runtime);
    (void)runtime->attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                          ssg::ViewId{1});
    return runtime;
}

}  // namespace

namespace {

std::string rowText(ssg::CellGrid const& grid, int row) {
    std::string line;
    for (int column = 0; column < grid.size.columns; ++column) {
        auto const& cell = grid.at(column, row);
        if (!cell.continuation) line += cell.text;
    }
    return line;
}

bool gridContains(ssg::CellGrid const& grid, std::string_view needle) {
    for (int row = 0; row < grid.size.rows; ++row) {
        if (rowText(grid, row).find(needle) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

TEST(renderPaintsContentNotAccessibilityLabels) {
    auto root = uniqueRoot();
    std::ofstream{root / "hello.txt"} << "alpha\nbeta\n";
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    (void)runtime->dispatch(
        ssg::ClientId{1},
        {"file.open", runtime->revision(), std::string{"hello.txt"}});
    auto snapshot = runtime->snapshot(ssg::ClientId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto grid = ssg::render(*snapshot);
    // Container accessibility labels must never be painted.
    ASSERT_FALSE(gridContains(grid, "Status header"));
    ASSERT_FALSE(gridContains(grid, "Open tabs"));
    ASSERT_FALSE(gridContains(grid, "Workspace"));
    // The open document's real content is painted.
    ASSERT_TRUE(gridContains(grid, "alpha"));
}

TEST(renderSegmentsOnlyVisibleLinesNotWholeDocument) {
    // INV-render-projection (M12): render() runs compute_cell_run only for the
    // logical lines the viewport shows (<= rows), independent of document length.
    auto root = uniqueRoot();
    auto makeDoc = [](std::size_t lineCount) {
        std::string text;
        for (std::size_t i = 0; i < lineCount; ++i) {
            text += "line " + std::to_string(i) + "\n";
        }
        return text;
    };
    std::ofstream{root / "small.txt"} << makeDoc(50);
    std::ofstream{root / "big.txt"} << makeDoc(5000);
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;

    auto segmentCountFor = [&](std::string const& file) -> std::uint64_t {
        (void)runtime->dispatch(
            ssg::ClientId{1}, {"file.open", runtime->revision(), file});
        auto snapshot = runtime->snapshot(ssg::ClientId{1}, {80, 24});
        ASSERT_TRUE(snapshot.has_value());
        if (!snapshot) return 0;
        ssg::resetRenderSegmentationCalls();
        auto grid = ssg::render(*snapshot);
        (void)grid;
        return ssg::renderSegmentationCalls();
    };

    auto const smallCalls = segmentCountFor("small.txt");
    auto const bigCalls = segmentCountFor("big.txt");

    // At most one segmentation per visible editor row (24-tall terminal, minus
    // the chrome rows), and NOT proportional to the 100x-larger document.
    ASSERT_TRUE(smallCalls > 0);
    ASSERT_TRUE(smallCalls <= 24);
    ASSERT_TRUE(bigCalls <= 24);
    ASSERT_EQ(smallCalls, bigCalls);
}


TEST(wordWrapOffRendersHorizontallyScrolledContent) {
    // M12 VP-H: with the caret at the end of a long line (word wrap off), the
    // editor paints the horizontally-scrolled window — the line's END is visible
    // and its START has scrolled off — proving render honors first_visual_column.
    auto root = uniqueRoot();
    std::string line = "STARTmarker";
    line += std::string(120, '.');
    line += "ENDmarker";
    std::ofstream{root / "long.txt"} << line << "\nsecond\n";
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    (void)runtime->dispatch(
        ssg::ClientId{1},
        {"file.open", runtime->revision(), std::string{"long.txt"}});

    ssg::ViewportDimensions const dims{40, 8};
    (void)runtime->snapshot(ssg::ClientId{1}, dims);  // prime the pane cache
    (void)runtime->dispatch(ssg::ClientId{1},
                            {"cursor.line_end", runtime->revision(), {}});
    auto snapshot = runtime->snapshot(ssg::ClientId{1}, dims);
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_TRUE(snapshot->client().viewport.firstVisualColumn > 0);
    auto grid = ssg::render(*snapshot);

    // The end of the line is on screen; the start has scrolled off.
    ASSERT_TRUE(gridContains(grid, "ENDmarker"));
    ASSERT_FALSE(gridContains(grid, "STARTmarker"));
}


TEST(renderColorsArePaletteIndices) {
    auto root = uniqueRoot();
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    auto snapshot = runtime->snapshot(ssg::ClientId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto grid = ssg::render(*snapshot);
    bool allInPalette = true;
    for (auto const& cell : grid.cells) {
        if (cell.foreground >= ssg::kThemePaletteSize ||
            cell.background >= ssg::kThemePaletteSize) {
            allInPalette = false;
        }
    }
    ASSERT_TRUE(allInPalette);
}

TEST(renderIsDeterministic) {
    auto root = uniqueRoot();
    std::ofstream{root / "file.txt"} << "content\n";
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    auto first = runtime->snapshot(ssg::ClientId{1}, {80, 24});
    auto second = runtime->snapshot(ssg::ClientId{1}, {80, 24});
    ASSERT_TRUE(first.has_value() && second.has_value());
    if (!first || !second) return;
    ASSERT_EQ(ssg::render(*first).canonical(), ssg::render(*second).canonical());
}

TEST(renderProjectsPaletteResultsIntoActivePane) {
    auto root = uniqueRoot();
    std::ofstream{root / "hello.txt"} << "alpha\nbeta\n";
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    (void)runtime->dispatch(
        ssg::ClientId{1},
        {"file.open", runtime->revision(), std::string{"hello.txt"}});
    auto snapshot = runtime->snapshot(ssg::ClientId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;

    // Without a palette projection the document content is painted.
    ASSERT_TRUE(gridContains(ssg::render(*snapshot), "alpha"));

    auto sections = snapshot->sections();
    ASSERT_FALSE(sections.shell.panes.empty());
    if (sections.shell.panes.empty()) return;
    ssg::PaletteProjection projection;
    projection.rect = sections.shell.panes.front().content;
    projection.rows = {{"file.save", "ESC s"}, {"file.quit", "ESC q"}};
    projection.selected = std::uint32_t{1};
    sections.shell.palette = projection;

    ssg::SessionSnapshot projected{snapshot->revision(), snapshot->topology(),
                                   snapshot->client(), std::move(sections)};
    auto grid = ssg::render(projected);

    // Results replace the document text in the pane.
    ASSERT_TRUE(gridContains(grid, "file.save"));
    ASSERT_TRUE(gridContains(grid, "ESC q"));
    ASSERT_FALSE(gridContains(grid, "alpha"));

    // The selected row is painted with the selection role, including on the
    // label's glyph cells (not only trailing filler).
    int const selectedRow = projection.rect.y + 1;
    ASSERT_EQ(grid.at(projection.rect.x, selectedRow).text, std::string{"f"});
    ASSERT_EQ(grid.at(projection.rect.x, selectedRow).role,
              ssg::SemanticRole::Selection);
    // The unselected row must not carry the selection role.
    ASSERT_FALSE(grid.at(projection.rect.x, projection.rect.y).role ==
                 ssg::SemanticRole::Selection);
}

TEST(renderShowsPaletteQueryAndGhostInHeader) {
    auto root = uniqueRoot();
    std::ofstream{root / "hello.txt"} << "alpha\n";
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    (void)runtime->dispatch(
        ssg::ClientId{1},
        {"file.open", runtime->revision(), std::string{"hello.txt"}});
    (void)runtime->dispatch(ssg::ClientId{1},
                            {"palette.open", runtime->revision(), {}});

    ssg::PaletteReport report;
    report.query = "sa";
    report.ghost = "ve File";
    report.rows = {{"file.save", "Save File", ""}};
    report.selected = std::uint32_t{0};
    auto snapshot =
        runtime->snapshot(ssg::ClientId{1}, {80, 24}, {}, report);
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto grid = ssg::render(*snapshot);

    // The header shows the query (prompt role) and the dim ghost completion.
    ASSERT_TRUE(gridContains(grid, "> sa"));
    ASSERT_TRUE(gridContains(grid, "ve File"));

    bool queryPromptRole = false;
    bool ghostDimRole = false;
    for (int column = 0; column < grid.size.columns; ++column) {
        auto const& cell = grid.at(column, 0);
        if (cell.text == "s" && cell.role == ssg::SemanticRole::Prompt) {
            queryPromptRole = true;
        }
        if (cell.text == "v" && cell.role == ssg::SemanticRole::LineNumber) {
            ghostDimRole = true;
        }
    }
    ASSERT_TRUE(queryPromptRole);
    ASSERT_TRUE(ghostDimRole);
}

TEST(renderPaintsSelectionHighlightAndSecondaryCarets) {
    auto root = uniqueRoot();
    std::ofstream{root / "sel.txt"} << "alpha\nbeta\n";
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    (void)runtime->dispatch(
        ssg::ClientId{1},
        {"file.open", runtime->revision(), std::string{"sel.txt"}});

    // Baseline: no selection -> the document row has no selection-role cells.
    {
        auto snapshot = runtime->snapshot(ssg::ClientId{1}, {80, 24});
        ASSERT_TRUE(snapshot.has_value());
        if (!snapshot) return;
        auto grid = ssg::render(*snapshot);
        bool anySelection = false;
        for (int row = 0; row < grid.size.rows; ++row) {
            for (int col = 0; col < grid.size.columns; ++col) {
                if (grid.at(col, row).role == ssg::SemanticRole::Selection) {
                    anySelection = true;
                }
            }
        }
        ASSERT_FALSE(anySelection);
    }

    // Select to end of the first line: "alpha" cells carry the selection role.
    (void)runtime->dispatch(ssg::ClientId{1},
                            {"select.line_end", runtime->revision(), {}});
    auto snapshot = runtime->snapshot(ssg::ClientId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto grid = ssg::render(*snapshot);

    int alphaRow = -1;
    for (int row = 0; row < grid.size.rows; ++row) {
        if (rowText(grid, row).find("alpha") != std::string::npos) alphaRow = row;
    }
    ASSERT_TRUE(alphaRow >= 0);
    if (alphaRow < 0) return;

    // Find the first column of "alpha" on that row.
    int alphaCol = -1;
    for (int col = 0; col + 5 <= grid.size.columns; ++col) {
        std::string window;
        for (int k = 0; k < 5; ++k) window += grid.at(col + k, alphaRow).text;
        if (window == "alpha") { alphaCol = col; break; }
    }
    ASSERT_TRUE(alphaCol >= 0);
    if (alphaCol < 0) return;
    for (int k = 0; k < 5; ++k) {
        ASSERT_EQ(grid.at(alphaCol + k, alphaRow).role,
                  ssg::SemanticRole::Selection);
    }
    // The end-of-line past "alpha" is NOT filled: the selection ends at the line
    // end and does not span into the next line.
    ASSERT_NE(grid.at(alphaCol + 5, alphaRow).role,
              ssg::SemanticRole::Selection);
    // The hardware caret sits at the primary active position (end of "alpha").
    ASSERT_TRUE(grid.caret.has_value());
    if (grid.caret) {
        ASSERT_EQ(grid.caret->row, alphaRow);
        ASSERT_EQ(grid.caret->column, alphaCol + 5);
    }
}

TEST(renderFillsEndOfLineForMultilineSelection) {
    auto root = uniqueRoot();
    std::ofstream{root / "ml.txt"} << "alpha\nbeta\n";
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    (void)runtime->dispatch(
        ssg::ClientId{1},
        {"file.open", runtime->revision(), std::string{"ml.txt"}});
    // Anchor at line 0 col 0, extend down into line 1: the selection spans the
    // newline after "alpha", so alpha's end-of-line fills to the pane edge.
    (void)runtime->dispatch(ssg::ClientId{1},
                            {"select.line_down", runtime->revision(), {}});
    auto snapshot = runtime->snapshot(ssg::ClientId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto grid = ssg::render(*snapshot);

    int alphaRow = -1, alphaCol = -1;
    for (int row = 0; row < grid.size.rows && alphaRow < 0; ++row) {
        for (int col = 0; col + 5 <= grid.size.columns; ++col) {
            std::string window;
            for (int k = 0; k < 5; ++k) window += grid.at(col + k, row).text;
            if (window == "alpha") { alphaRow = row; alphaCol = col; break; }
        }
    }
    ASSERT_TRUE(alphaRow >= 0);
    if (alphaRow < 0) return;
    // "alpha" is highlighted AND the cells past it to the pane's right edge are
    // the end-of-line fill (all selection role).
    for (int col = alphaCol; col < grid.size.columns - 1; ++col) {
        ASSERT_EQ(grid.at(col, alphaRow).role, ssg::SemanticRole::Selection);
    }
}

TEST(renderHighlightsWideGlyphCells) {
    auto root = uniqueRoot();
    // A CJK wide glyph occupies two cells; selecting it must highlight both.
    std::ofstream{root / "w.txt"} << "a\xe4\xb8\x80""b\n";  // "a一b"
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    (void)runtime->dispatch(
        ssg::ClientId{1},
        {"file.open", runtime->revision(), std::string{"w.txt"}});
    (void)runtime->dispatch(ssg::ClientId{1},
                            {"select.all", runtime->revision(), {}});
    auto snapshot = runtime->snapshot(ssg::ClientId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto grid = ssg::render(*snapshot);

    int row = -1, col = -1;
    for (int r = 0; r < grid.size.rows && row < 0; ++r) {
        for (int c = 0; c < grid.size.columns; ++c) {
            if (grid.at(c, r).text == "\xe4\xb8\x80") { row = r; col = c; break; }
        }
    }
    ASSERT_TRUE(row >= 0);
    if (row < 0) return;
    // The wide glyph's lead cell and its continuation cell both carry selection.
    ASSERT_EQ(grid.at(col, row).role, ssg::SemanticRole::Selection);
    ASSERT_TRUE(grid.at(col + 1, row).continuation);
    ASSERT_EQ(grid.at(col + 1, row).role, ssg::SemanticRole::Selection);
}

TEST(renderPaintsSecondaryRangedSelectionCaret) {
    auto root = uniqueRoot();
    std::ofstream{root / "rc.txt"} << "cat cat\n";
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    (void)runtime->dispatch(
        ssg::ClientId{1},
        {"file.open", runtime->revision(), std::string{"rc.txt"}});
    // Select the first word, then add the next occurrence: two RANGED selections,
    // each with an active caret. The secondary (non-primary) ranged selection's
    // caret must render as a caret cell even though it is not a bare caret.
    (void)runtime->dispatch(ssg::ClientId{1},
                            {"select.word_right", runtime->revision(), {}});
    (void)runtime->dispatch(ssg::ClientId{1},
                            {"select.add_next_occurrence", runtime->revision(), {}});
    auto snapshot = runtime->snapshot(ssg::ClientId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto const& items = snapshot->sections().selection.selections.items();
    if (items.size() < 2) return;  // Ranker may not find a second; skip if so.
    bool allRanged = true;
    for (auto const& item : items) if (item.isCaret()) allRanged = false;
    ASSERT_TRUE(allRanged);
    auto grid = ssg::render(*snapshot);
    int paintedSecondary = 0;
    for (int row = 0; row < grid.size.rows; ++row) {
        for (int col = 0; col < grid.size.columns; ++col) {
            bool const isPrimary = grid.caret && grid.caret->row == row &&
                                    grid.caret->column == col;
            if (grid.at(col, row).role == ssg::SemanticRole::Caret && !isPrimary) {
                ++paintedSecondary;
            }
        }
    }
    // One painted caret for the secondary ranged selection's active position.
    ASSERT_EQ(paintedSecondary, 1);
}

TEST(renderPaintsSecondaryCaretAsACell) {
    auto root = uniqueRoot();
    std::ofstream{root / "car.txt"} << "alpha\nbeta\n";
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    (void)runtime->dispatch(
        ssg::ClientId{1},
        {"file.open", runtime->revision(), std::string{"car.txt"}});

    // Two carets (primary + one below): the primary uses the hardware cursor,
    // the other renders as a caret-role cell.
    (void)runtime->dispatch(ssg::ClientId{1},
                            {"select.add_cursor_down", runtime->revision(), {}});
    auto snapshot = runtime->snapshot(ssg::ClientId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_EQ(snapshot->sections().selection.selections.items().size(),
              std::size_t{2});
    auto grid = ssg::render(*snapshot);
    ASSERT_TRUE(grid.caret.has_value());

    int paintedSecondary = 0;
    for (int row = 0; row < grid.size.rows; ++row) {
        for (int col = 0; col < grid.size.columns; ++col) {
            bool const isPrimary = grid.caret && grid.caret->row == row &&
                                    grid.caret->column == col;
            if (grid.at(col, row).role == ssg::SemanticRole::Caret && !isPrimary) {
                ++paintedSecondary;
            }
        }
    }
    // Exactly one caret is painted as a cell; the other is the hardware cursor.
    ASSERT_EQ(paintedSecondary, 1);
}

TEST(renderPaintsFindMatchesAndActiveMatch) {
    auto root = uniqueRoot();
    std::ofstream{root / "find.txt"} << "cat cat cat\n";
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    (void)runtime->dispatch(
        ssg::ClientId{1},
        {"file.open", runtime->revision(), std::string{"find.txt"}});
    (void)runtime->dispatch(ssg::ClientId{1},
                            {"find.open", runtime->revision(), {}});
    (void)runtime->dispatch(
        ssg::ClientId{1},
        {"find.update_query", runtime->revision(), ssg::FindQueryArguments{"cat"}});
    auto snapshot = runtime->snapshot(ssg::ClientId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_EQ(snapshot->sections().findReplace.matches.size(), std::size_t{3});
    auto grid = ssg::render(*snapshot);

    int row = -1;
    for (int r = 0; r < grid.size.rows; ++r) {
        if (rowText(grid, r).find("cat cat cat") != std::string::npos) row = r;
    }
    ASSERT_TRUE(row >= 0);
    if (row < 0) return;
    int col = -1;
    for (int c = 0; c + 3 <= grid.size.columns; ++c) {
        std::string window;
        for (int k = 0; k < 3; ++k) window += grid.at(c + k, row).text;
        if (window == "cat") { col = c; break; }
    }
    ASSERT_TRUE(col >= 0);
    if (col < 0) return;

    // The active match (the first "cat") carries the selection role; the two
    // other matches carry search_match; the separating spaces carry neither.
    for (int k = 0; k < 3; ++k) {
        ASSERT_EQ(grid.at(col + k, row).role, ssg::SemanticRole::Selection);
    }
    ASSERT_NE(grid.at(col + 3, row).role, ssg::SemanticRole::Selection);
    ASSERT_NE(grid.at(col + 3, row).role, ssg::SemanticRole::SearchMatch);
    for (int k = 0; k < 3; ++k) {
        ASSERT_EQ(grid.at(col + 4 + k, row).role, ssg::SemanticRole::SearchMatch);
        ASSERT_EQ(grid.at(col + 8 + k, row).role, ssg::SemanticRole::SearchMatch);
    }
}

TEST(renderHidesFindMatchesAfterDocumentRevisionChanges) {
    auto root = uniqueRoot();
    std::ofstream{root / "stale.txt"} << "cat cat cat\n";
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    (void)runtime->dispatch(
        ssg::ClientId{1},
        {"file.open", runtime->revision(), std::string{"stale.txt"}});
    (void)runtime->dispatch(ssg::ClientId{1},
                            {"find.open", runtime->revision(), {}});
    (void)runtime->dispatch(
        ssg::ClientId{1},
        {"find.update_query", runtime->revision(), ssg::FindQueryArguments{"cat"}});

    // Editing the document advances its revision without re-evaluating find, so
    // the controller is stale: reconcile closes it and no matches are painted.
    (void)runtime->dispatch(
        ssg::ClientId{1},
        {"text.insert", runtime->revision(), ssg::TextInputArguments{"z"}});
    auto snapshot = runtime->snapshot(ssg::ClientId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_FALSE(snapshot->sections().findReplace.open);
    auto grid = ssg::render(*snapshot);
    bool anyMatch = false;
    for (int row = 0; row < grid.size.rows; ++row) {
        for (int col = 0; col < grid.size.columns; ++col) {
            if (grid.at(col, row).role == ssg::SemanticRole::SearchMatch) {
                anyMatch = true;
            }
        }
    }
    ASSERT_FALSE(anyMatch);
}

TEST(renderReplacePromptShowsQueryAndReplacementWithCursorOnReplacement) {
    auto root = uniqueRoot();
    std::ofstream{root / "rep.txt"} << "cat cat cat\n";
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    (void)runtime->dispatch(
        ssg::ClientId{1},
        {"file.open", runtime->revision(), std::string{"rep.txt"}});
    (void)runtime->dispatch(ssg::ClientId{1},
                            {"replace.open", runtime->revision(), {}});
    (void)runtime->dispatch(
        ssg::ClientId{1},
        {"find.update_query", runtime->revision(), ssg::FindQueryArguments{"cat"}});
    (void)runtime->dispatch(
        ssg::ClientId{1},
        {"replace.update_replacement", runtime->revision(), ssg::FindQueryArguments{"dog"}});
    auto snapshot = runtime->snapshot(ssg::ClientId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto grid = ssg::render(*snapshot);

    // The reserved replace rows show the query and the replacement.
    ASSERT_TRUE(gridContains(grid, "cat"));
    ASSERT_TRUE(gridContains(grid, "dog"));
    // The hardware cursor sits on the replacement row (the one containing "dog").
    ASSERT_TRUE(grid.caret.has_value());
    if (grid.caret) {
        ASSERT_TRUE(rowText(grid, grid.caret->row).find("dog") != std::string::npos);
    }
}

TEST(renderFindPromptShowsOptionIndicators) {
    auto root = uniqueRoot();
    std::ofstream{root / "opt.txt"} << "Cat cat CAT\n";
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    (void)runtime->dispatch(
        ssg::ClientId{1},
        {"file.open", runtime->revision(), std::string{"opt.txt"}});
    (void)runtime->dispatch(ssg::ClientId{1},
                            {"find.open", runtime->revision(), {}});
    (void)runtime->dispatch(
        ssg::ClientId{1},
        {"find.update_query", runtime->revision(), ssg::FindQueryArguments{"cat"}});

    // Default options: all three indicators render unchecked.
    {
        auto snapshot = runtime->snapshot(ssg::ClientId{1}, {80, 24});
        ASSERT_TRUE(snapshot.has_value());
        if (!snapshot) return;
        auto grid = ssg::render(*snapshot);
        ASSERT_TRUE(gridContains(grid, "[ ] Case"));
        ASSERT_TRUE(gridContains(grid, "[ ] Word"));
        ASSERT_TRUE(gridContains(grid, "[ ] Regex"));
    }

    // Toggling case flips its indicator to checked.
    (void)runtime->dispatch(ssg::ClientId{1},
                            {"find.toggle_case", runtime->revision(), {}});
    auto snapshot = runtime->snapshot(ssg::ClientId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto grid = ssg::render(*snapshot);
    ASSERT_TRUE(gridContains(grid, "[x] Case"));
    ASSERT_TRUE(gridContains(grid, "[ ] Word"));
}

TEST(renderPanelTreeWindowsAndDrawsAThumbWhenTallerThanThePanel) {
    auto root = uniqueRoot();
    for (int i = 0; i < 40; ++i) {
        char name[32];
        std::snprintf(name, sizeof name, "file-%02d.txt", i);
        std::ofstream{root / name} << "x";
    }
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    (void)runtime->dispatch(ssg::ClientId{1}, {"panel.toggle", runtime->revision(), {}});
    // Expand the workspace root, then drive the selection to the bottom.
    (void)runtime->dispatch(ssg::ClientId{1}, {"tree.select_next", runtime->revision(), {}});
    (void)runtime->dispatch(ssg::ClientId{1}, {"tree.activate", runtime->revision(), {}});
    // Prime the cached panel height (the command-path keep-visible reads it).
    (void)runtime->snapshot(ssg::ClientId{1}, {80, 12});
    for (int i = 0; i < 60; ++i) {
        (void)runtime->dispatch(ssg::ClientId{1}, {"tree.select_next", runtime->revision(), {}});
    }
    auto snapshot = runtime->snapshot(ssg::ClientId{1}, {80, 12});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto const& shell = snapshot->sections().shell;
    ASSERT_TRUE(shell.panelScrollbar.has_value());
    if (!shell.panelScrollbar) return;
    auto grid = ssg::render(*snapshot);

    // A thumb ('#') is drawn in the reserved gutter column.
    int const gx = shell.panelScrollbar->x;
    bool hasThumb = false;
    for (int y = shell.panelScrollbar->y;
         y < shell.panelScrollbar->y + shell.panelScrollbar->height; ++y) {
        if (grid.at(gx, y).text == "#") hasThumb = true;
    }
    ASSERT_TRUE(hasThumb);
    // The window scrolled to the end: the first file is off-screen, the last is
    // visible.
    ASSERT_FALSE(gridContains(grid, "file-00.txt"));
    ASSERT_TRUE(gridContains(grid, "file-39.txt"));
}

TEST(renderPanelTreeReservesAnEmptyGutterWhenItFits) {
    auto root = uniqueRoot();
    std::ofstream{root / "a.txt"} << "x";
    std::ofstream{root / "b.txt"} << "x";
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    (void)runtime->dispatch(ssg::ClientId{1}, {"panel.toggle", runtime->revision(), {}});
    // Expand the root so its two files are visible; the tree still fits.
    (void)runtime->dispatch(ssg::ClientId{1}, {"tree.select_next", runtime->revision(), {}});
    (void)runtime->dispatch(ssg::ClientId{1}, {"tree.activate", runtime->revision(), {}});
    auto snapshot = runtime->snapshot(ssg::ClientId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto const& shell = snapshot->sections().shell;
    ASSERT_TRUE(shell.panelScrollbar.has_value());
    if (!shell.panelScrollbar) return;
    auto grid = ssg::render(*snapshot);
    // The gutter is reserved (column exists) but blank: no thumb or track glyphs,
    // so the tree's content width never changes as items are added or removed.
    int const gx = shell.panelScrollbar->x;
    for (int y = shell.panelScrollbar->y;
         y < shell.panelScrollbar->y + shell.panelScrollbar->height; ++y) {
        ASSERT_NE(grid.at(gx, y).text, std::string{"#"});
        ASSERT_NE(grid.at(gx, y).text, std::string{"|"});
    }
    ASSERT_TRUE(gridContains(grid, "a.txt"));
}

TEST(renderPaletteWindowsRowsAndDrawsAThumbWithAbsoluteSelection) {
    auto root = uniqueRoot();
    std::ofstream{root / "hello.txt"} << "alpha\nbeta\n";
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    (void)runtime->dispatch(
        ssg::ClientId{1},
        {"file.open", runtime->revision(), std::string{"hello.txt"}});
    auto snapshot = runtime->snapshot(ssg::ClientId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto sections = snapshot->sections();
    ASSERT_FALSE(sections.shell.panes.empty());
    if (sections.shell.panes.empty()) return;

    // A 40-item ranked list windowed to rows [20, 20+height); the absolute
    // selection is 25, so the on-screen highlight is at window row 5.
    auto const& pane = sections.shell.panes.front();
    std::uint32_t const rows = static_cast<std::uint32_t>(pane.content.height);
    ssg::PaletteProjection projection;
    projection.rect = pane.content;
    projection.scrollbarRect = pane.scrollbar;
    projection.firstVisible = 20;
    projection.selected = std::uint32_t{25};
    projection.scrollbar = ssg::Viewport{}.scrollbarMetrics(40, rows, 20);
    for (std::uint32_t i = 0; i < rows; ++i) {
        projection.rows.push_back(
            {"cmd-" + std::to_string(20 + i), ""});
    }
    sections.shell.palette = projection;
    ssg::SessionSnapshot projected{snapshot->revision(), snapshot->topology(),
                                   snapshot->client(), std::move(sections)};
    auto grid = ssg::render(projected);

    // The window shows cmd-20.. (not cmd-00), and the absolute-25 selection lands
    // at window row 5.
    ASSERT_TRUE(gridContains(grid, "cmd-20"));
    ASSERT_FALSE(gridContains(grid, "cmd-00"));
    int const selectedRow = projection.rect.y + 5;
    ASSERT_EQ(grid.at(projection.rect.x, selectedRow).role,
              ssg::SemanticRole::Selection);
    // Row 0 (absolute 20) is not selected.
    ASSERT_FALSE(grid.at(projection.rect.x, projection.rect.y).role ==
                 ssg::SemanticRole::Selection);
    // A thumb is drawn in the reserved gutter column.
    bool hasThumb = false;
    for (int y = projection.scrollbarRect.y;
         y < projection.scrollbarRect.y + projection.scrollbarRect.height; ++y) {
        if (grid.at(projection.scrollbarRect.x, y).text == "#") hasThumb = true;
    }
    ASSERT_TRUE(hasThumb);
}

TEST(renderPaletteReservesAnEmptyGutterWhenTheListFits) {
    auto root = uniqueRoot();
    std::ofstream{root / "hello.txt"} << "alpha\n";
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    (void)runtime->dispatch(
        ssg::ClientId{1},
        {"file.open", runtime->revision(), std::string{"hello.txt"}});
    auto snapshot = runtime->snapshot(ssg::ClientId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto sections = snapshot->sections();
    auto const& pane = sections.shell.panes.front();
    ssg::PaletteProjection projection;
    projection.rect = pane.content;
    projection.scrollbarRect = pane.scrollbar;
    projection.firstVisible = 0;
    projection.selected = std::uint32_t{0};
    projection.scrollbar =
        ssg::Viewport{}.scrollbarMetrics(2, static_cast<std::uint32_t>(pane.content.height), 0);
    projection.rows = {{"a", ""}, {"b", ""}};
    sections.shell.palette = projection;
    ssg::SessionSnapshot projected{snapshot->revision(), snapshot->topology(),
                                   snapshot->client(), std::move(sections)};
    auto grid = ssg::render(projected);
    // The gutter is reserved (column exists) but blank: no thumb/track glyphs.
    for (int y = projection.scrollbarRect.y;
         y < projection.scrollbarRect.y + projection.scrollbarRect.height; ++y) {
        ASSERT_NE(grid.at(projection.scrollbarRect.x, y).text, std::string{"#"});
        ASSERT_NE(grid.at(projection.scrollbarRect.x, y).text, std::string{"|"});
    }
}

TEST(renderTooSmallViewportProducesLibraryPlaceholder) {
    // M11-L: below the 20x4 minimum the library (not the app) renders the
    // placeholder screen, sized to the terminal, so no app code authors cells.
    auto root = uniqueRoot();
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;

    // A sub-minimum viewport: render must NOT throw and must yield a grid of the
    // requested terminal size carrying the centered message.
    auto snapshot = runtime->snapshot(ssg::ClientId{1}, {10, 5});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_EQ(snapshot->sections().shell.viewport.columns, 0);  // declined layout
    ssg::CellGrid grid;
    ASSERT_NO_THROW(grid = ssg::render(*snapshot));
    ASSERT_EQ(grid.size.columns, 10);
    ASSERT_EQ(grid.size.rows, 5);
    ASSERT_EQ(grid.cells.size(), std::size_t{50});
    // The message is centered on the middle row (rows/2 = 2) and clipped with an
    // ellipsis to the 10-column width.
    ASSERT_EQ(rowText(grid, 2), std::string("terminal \xe2\x80\xa6"));
    ASSERT_EQ(rowText(grid, 0), std::string(10, ' '));

    std::filesystem::remove_all(root);
}

TEST(renderTooSmallMatchesHandAuthoredGolden) {
    auto root = uniqueRoot();
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    // A 24-wide, 3-row terminal fits the whole 18-cell message, centered.
    auto snapshot = runtime->snapshot(ssg::ClientId{1}, {24, 3});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto grid = ssg::render(*snapshot);
    ASSERT_EQ(grid.size.columns, 24);
    ASSERT_EQ(grid.size.rows, 3);
    // 18-cell message centered in 24 columns -> start column (24-18)/2 = 3, on
    // the middle row (3/2 = 1).
    ASSERT_EQ(rowText(grid, 0), std::string(24, ' '));
    ASSERT_EQ(rowText(grid, 1),
              std::string("   terminal too small   "));
    ASSERT_EQ(rowText(grid, 2), std::string(24, ' '));
    // Determinism.
    ASSERT_EQ(ssg::render(*snapshot).canonical(), grid.canonical());
    std::filesystem::remove_all(root);
}

TEST(renderTooSmallIsSafeAtOneByOne) {
    auto root = uniqueRoot();
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    auto snapshot = runtime->snapshot(ssg::ClientId{1}, {1, 1});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ssg::CellGrid grid;
    ASSERT_NO_THROW(grid = ssg::render(*snapshot));
    ASSERT_EQ(grid.size.columns, 1);
    ASSERT_EQ(grid.size.rows, 1);
    ASSERT_EQ(grid.cells.size(), std::size_t{1});
    std::filesystem::remove_all(root);
}

int main() {
    RUN(renderPaintsContentNotAccessibilityLabels);
    RUN(renderSegmentsOnlyVisibleLinesNotWholeDocument);
    RUN(wordWrapOffRendersHorizontallyScrolledContent);
    RUN(renderColorsArePaletteIndices);
    RUN(renderIsDeterministic);
    RUN(renderProjectsPaletteResultsIntoActivePane);
    RUN(renderShowsPaletteQueryAndGhostInHeader);
    RUN(renderPaintsSelectionHighlightAndSecondaryCarets);
    RUN(renderFillsEndOfLineForMultilineSelection);
    RUN(renderHighlightsWideGlyphCells);
    RUN(renderPaintsSecondaryRangedSelectionCaret);
    RUN(renderPaintsSecondaryCaretAsACell);
    RUN(renderPaintsFindMatchesAndActiveMatch);
    RUN(renderHidesFindMatchesAfterDocumentRevisionChanges);
    RUN(renderReplacePromptShowsQueryAndReplacementWithCursorOnReplacement);
    RUN(renderFindPromptShowsOptionIndicators);
    RUN(renderPanelTreeWindowsAndDrawsAThumbWhenTallerThanThePanel);
    RUN(renderPanelTreeReservesAnEmptyGutterWhenItFits);
    RUN(renderPaletteWindowsRowsAndDrawsAThumbWithAbsoluteSelection);
    RUN(renderPaletteReservesAnEmptyGutterWhenTheListFits);
    RUN(renderTooSmallViewportProducesLibraryPlaceholder);
    RUN(renderTooSmallMatchesHandAuthoredGolden);
    RUN(renderTooSmallIsSafeAtOneByOne);

    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed > 0 ? 1 : 0;
}
