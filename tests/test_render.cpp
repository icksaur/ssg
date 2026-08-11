#include <ssg/Renderer.h>

#include <ssg/EditorRuntime.h>
#include <ssg/FindReplace.h>
#include <ssg/PaletteSearcher.h>
#include <ssg/Selection.h>
#include <ssg/TreeModel.h>
#include <ssg/session_snapshot.h>

#include "session_snapshot_builder.h"
#include "test_helpers.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
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

// Rebuilds a snapshot with a replaced Style section.  After Y4 the renderer
// reads style from the snapshot, not from a member, so this is how a test
// restyles -- and doing it through the published section is exactly the proof
// that the renderer and runtime share the one instance.
ssg::SessionSnapshot withStyle(ssg::SessionSnapshot const& base,
                               ssg::Style style) {
    auto presentation = *base.presentation();
    presentation.style = std::move(style);
    return ssg::SessionSnapshot{base.revision(), base.topology(), base.client(),
                                base.sections(),
                                std::move(presentation)};
}

}  // namespace

TEST(chromeBackgroundsAreDistinctShadesAndTheActiveTabMergesWithTheDocument) {
    // The chrome color model: the header, footer, and tab bar each carry their
    // OWN background shade -- distinct greyscale bands, so the regions read apart
    // -- and every band is distinct from the document. The active tab uses the
    // document Background so it reads as selected by merging into the content
    // below.
    auto root = uniqueRoot();
    std::ofstream{root / "alpha.txt"} << "one\n";
    std::ofstream{root / "beta.txt"} << "two\n";
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    (void)runtime->dispatch(ssg::ClientId{1},
                            {"file.open", runtime->revision(), std::string{"alpha.txt"}});
    (void)runtime->dispatch(ssg::ClientId{1},
                            {"file.open", runtime->revision(), std::string{"beta.txt"}});
    auto snapshot = runtime->snapshot(ssg::ClientId{1}, {60, 12});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    const auto& shell = snapshot->sections().shell;
    ASSERT_TRUE(shell.header.has_value());
    ASSERT_TRUE(shell.footer.has_value());
    ASSERT_TRUE(shell.tabBar.has_value());
    auto grid = ssg::Renderer{}.render(*snapshot);

    const auto& theme = snapshot->sections().theme;
    const auto headerBand =
        theme.color(ssg::SemanticRole::HeaderBackground);
    const auto footerBand =
        theme.color(ssg::SemanticRole::FooterBackground);
    const auto tabBand =
        theme.color(ssg::SemanticRole::TabInactiveBackground);
    const auto docColor = theme.color(ssg::SemanticRole::Canvas);
    // Every chrome band is a distinct color from the document.
    ASSERT_NE(headerBand, docColor);
    ASSERT_NE(footerBand, docColor);
    ASSERT_NE(tabBand, docColor);

    // A cell's background is a slot index into grid.colors; resolve it back to
    // the color to check the chrome painting.
    const auto colorOf = [&](int x, int y) {
        return grid.colors[grid.at(x, y).background];
    };
    // Each region is painted with ITS OWN band role.
    ASSERT_EQ(colorOf(shell.tabBar->right() - 1, shell.tabBar->y), tabBand);
    ASSERT_EQ(colorOf(shell.header->x, shell.header->y), headerBand);
    ASSERT_EQ(colorOf(shell.footer->x, shell.footer->y), footerBand);

    // The active tab's first cell carries the document Background, not the band,
    // so it merges with the content below.
    int activeTabX = -1;
    for (const auto& node : shell.accessibilityNodes) {
        if (node.kind == ssg::ShellNodeKind::Tab &&
            node.role == ssg::SemanticRole::TabActive) {
            activeTabX = node.rect.x;
            break;
        }
    }
    ASSERT_NE(activeTabX, -1);
    if (activeTabX >= 0) {
        ASSERT_EQ(colorOf(activeTabX, shell.tabBar->y), docColor);
    }
}

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
    auto grid = ssg::Renderer{}.render(*snapshot);
    // Container accessibility labels must never be painted.
    ASSERT_FALSE(gridContains(grid, "Status header"));
    ASSERT_FALSE(gridContains(grid, "Open tabs"));
    ASSERT_FALSE(gridContains(grid, "Workspace"));
    // The open document's real content is painted.
    ASSERT_TRUE(gridContains(grid, "alpha"));
}

TEST(lineNumberGutterPaintsNumbersAndHighlightsTheCaretLine) {
    auto root = uniqueRoot();
    std::ofstream{root / "n.txt"} << "alpha\nbeta\ngamma\n";
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    (void)runtime->dispatch(
        ssg::ClientId{1},
        {"file.open", runtime->revision(), std::string{"n.txt"}});
    (void)runtime->dispatch(
        ssg::ClientId{1}, {"view.toggle_line_numbers", runtime->revision(), {}});
    // Put the caret on line 2 (0-indexed 1) so its number highlights.
    auto atBeta = ssg::SelectionNavigator::resolvePosition("alpha\nbeta\ngamma\n",
                                                           ssg::ByteOffset{6});
    ASSERT_TRUE(atBeta.has_value());
    (void)runtime->dispatch(
        ssg::ClientId{1},
        {"cursor.set_position", runtime->revision(),
         ssg::SelectionCommandArguments{atBeta, std::nullopt}});

    auto snapshot = runtime->snapshot(ssg::ClientId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto const& pane = snapshot->sections().shell.panes.front();
    ASSERT_TRUE(pane.lineNumbers.width > 0);
    auto grid = ssg::Renderer{}.render(*snapshot);
    int const gx = pane.lineNumbers.x;
    int const gw = pane.lineNumbers.width;  // 4 lines -> 1 digit -> width 2
    ASSERT_EQ(gw, 2);
    auto gutterText = [&](int row) {
        std::string s;
        for (int c = 0; c < gw; ++c) s += grid.at(gx + c, pane.lineNumbers.y + row).text;
        return s;
    };
    // Right-aligned number + trailing space: "1 ", "2 ", "3 ".
    ASSERT_EQ(gutterText(0), std::string{"1 "});
    ASSERT_EQ(gutterText(1), std::string{"2 "});
    ASSERT_EQ(gutterText(2), std::string{"3 "});
    // The caret's line (row 1) uses the current-line roles; others use LineNumber.
    ASSERT_EQ(grid.at(gx, pane.lineNumbers.y + 1).role,
              ssg::SemanticRole::CurrentLineNumber);
    ASSERT_EQ(grid.at(gx, pane.lineNumbers.y + 0).role,
              ssg::SemanticRole::LineNumber);
    // The inactive gutter has its own background band: distinct from the
    // document content background beside it AND from the current line's band.
    auto gutterBg = [&](int row) {
        return grid.colors[grid.at(gx, pane.lineNumbers.y + row).background];
    };
    auto const contentBg =
        grid.colors[grid.at(pane.content.x, pane.content.y).background];
    ASSERT_TRUE(gutterBg(0) != contentBg);
    ASSERT_TRUE(gutterBg(0) != gutterBg(1));
    std::filesystem::remove_all(root);
}

TEST(lineNumberGutterHighlightsEveryCursorLineNotJustThePrimary) {
    auto root = uniqueRoot();
    std::ofstream{root / "m.txt"} << "one\ntwo\nthree\nfour\n";
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    (void)runtime->dispatch(
        ssg::ClientId{1},
        {"file.open", runtime->revision(), std::string{"m.txt"}});
    (void)runtime->dispatch(
        ssg::ClientId{1}, {"view.toggle_line_numbers", runtime->revision(), {}});
    // Add a second cursor on the line below: carets now on lines 1 and 2.
    (void)runtime->dispatch(ssg::ClientId{1},
                            {"select.add_cursor_down", runtime->revision(), {}});

    auto snapshot = runtime->snapshot(ssg::ClientId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_EQ(snapshot->sections().selection.selections.items().size(),
              std::size_t{2});
    auto const& pane = snapshot->sections().shell.panes.front();
    auto grid = ssg::Renderer{}.render(*snapshot);
    int const gx = pane.lineNumbers.x;
    auto roleAt = [&](int row) {
        return grid.at(gx, pane.lineNumbers.y + row).role;
    };
    // Both cursor lines (rows 0 and 1) highlight; the cursor-free line 3 does not.
    ASSERT_EQ(roleAt(0), ssg::SemanticRole::CurrentLineNumber);
    ASSERT_EQ(roleAt(1), ssg::SemanticRole::CurrentLineNumber);
    ASSERT_EQ(roleAt(2), ssg::SemanticRole::LineNumber);
    std::filesystem::remove_all(root);
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
        ssg::Renderer::resetRenderSegmentationCalls();
        auto grid = ssg::Renderer{}.render(*snapshot);
        (void)grid;
        return ssg::Renderer::renderSegmentationCalls();
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
    ASSERT_TRUE(snapshot->presentation()->viewport.firstVisualColumn > 0);
    auto grid = ssg::Renderer{}.render(*snapshot);

    // The end of the line is on screen; the start has scrolled off.
    ASSERT_TRUE(gridContains(grid, "ENDmarker"));
    ASSERT_FALSE(gridContains(grid, "STARTmarker"));
}


TEST(renderColorsAreInBoundsColorSlots) {
    auto snapshot = ssg::test::SessionSnapshotBuilder{}.viewport(80, 24).build();
    auto grid = ssg::Renderer{}.render(snapshot);
    bool allInBounds = true;
    for (auto const& cell : grid.cells) {
        if (cell.foreground >= ssg::kThemeColorSlotCount ||
            cell.background >= ssg::kThemeColorSlotCount) {
            allInBounds = false;
        }
    }
    ASSERT_TRUE(allInBounds);
}

TEST(renderIsDeterministic) {
    auto snapshot =
        ssg::test::SessionSnapshotBuilder{}.document("content\n").viewport(80, 24).build();
    ASSERT_EQ(ssg::Renderer{}.render(snapshot).canonical(),
              ssg::Renderer{}.render(snapshot).canonical());
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
    ASSERT_TRUE(gridContains(ssg::Renderer{}.render(*snapshot), "alpha"));

    auto sections = snapshot->sections();
    ASSERT_FALSE(sections.shell.panes.empty());
    if (sections.shell.panes.empty()) return;
    ssg::PaletteProjection projection;
    projection.rect = sections.shell.panes.front().content;
    projection.rows = {{"file.save", "ESC s"}, {"file.quit", "ESC q"}};
    projection.selected = std::uint32_t{1};
    sections.shell.palette = projection;

    ssg::SessionSnapshot projected{snapshot->revision(), snapshot->topology(),
                                   snapshot->client(), std::move(sections), snapshot->presentation()};
    auto grid = ssg::Renderer{}.render(projected);

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
        runtime->snapshot(ssg::ClientId{1}, {80, 24}, report);
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto grid = ssg::Renderer{}.render(*snapshot);

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
        auto grid = ssg::Renderer{}.render(*snapshot);
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
    auto grid = ssg::Renderer{}.render(*snapshot);

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
    auto grid = ssg::Renderer{}.render(*snapshot);

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
    auto grid = ssg::Renderer{}.render(*snapshot);

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
    auto grid = ssg::Renderer{}.render(*snapshot);
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
    auto grid = ssg::Renderer{}.render(*snapshot);
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
    auto grid = ssg::Renderer{}.render(*snapshot);

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
    auto grid = ssg::Renderer{}.render(*snapshot);
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
    auto grid = ssg::Renderer{}.render(*snapshot);

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
        auto grid = ssg::Renderer{}.render(*snapshot);
        ASSERT_TRUE(gridContains(grid, "[ ] case"));
        ASSERT_TRUE(gridContains(grid, "[ ] word"));
        ASSERT_TRUE(gridContains(grid, "[ ] regex"));
        // The query input label is chrome and renders lowercase.
        ASSERT_TRUE(gridContains(grid, "find query"));
        ASSERT_FALSE(gridContains(grid, "Find query"));
    }

    // Toggling case flips its indicator to checked.
    (void)runtime->dispatch(ssg::ClientId{1},
                            {"find.toggle_case", runtime->revision(), {}});
    auto snapshot = runtime->snapshot(ssg::ClientId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto grid = ssg::Renderer{}.render(*snapshot);
    ASSERT_TRUE(gridContains(grid, "[x] case"));
    ASSERT_TRUE(gridContains(grid, "[ ] word"));
}

TEST(renderPromptControlLabelsAreLowercaseChrome) {
    // Every prompt's rendered control captions are lowercase chrome, across the
    // settings, goto-line, and open-file path prompts.
    auto root = uniqueRoot();
    std::ofstream{root / "p.txt"} << "hello\n";
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    (void)runtime->dispatch(
        ssg::ClientId{1},
        {"file.open", runtime->revision(), std::string{"p.txt"}});

    struct Case {
        const char* command;
        const char* lower;
        const char* title;
    };
    for (auto const& c : {Case{"settings.open", "settings query", "Settings query"},
                          Case{"goto.line", "line number", "Line number"},
                          Case{"file.open", "open file", "Open file"}}) {
        (void)runtime->dispatch(ssg::ClientId{1},
                                {c.command, runtime->revision(), {}});
        auto snapshot = runtime->snapshot(ssg::ClientId{1}, {80, 24});
        ASSERT_TRUE(snapshot.has_value());
        if (!snapshot) continue;
        auto grid = ssg::Renderer{}.render(*snapshot);
        ASSERT_TRUE(gridContains(grid, c.lower));
        ASSERT_FALSE(gridContains(grid, c.title));
        (void)runtime->dispatch(ssg::ClientId{1},
                                {"prompt.cancel", runtime->revision(), {}});
    }
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
    auto grid = ssg::Renderer{}.render(*snapshot);

    // A thumb (the default thumb glyph) is drawn in the reserved gutter column.
    int const gx = shell.panelScrollbar->x;
    bool hasThumb = false;
    for (int y = shell.panelScrollbar->y;
         y < shell.panelScrollbar->y + shell.panelScrollbar->height; ++y) {
        if (grid.at(gx, y).text == ssg::Style{}.scrollbar.body) hasThumb = true;
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
    auto grid = ssg::Renderer{}.render(*snapshot);
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
                                   snapshot->client(), std::move(sections), snapshot->presentation()};
    auto grid = ssg::Renderer{}.render(projected);

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
        if (grid.at(projection.scrollbarRect.x, y).text ==
            ssg::Style{}.scrollbar.body)
            hasThumb = true;
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
                                   snapshot->client(), std::move(sections), snapshot->presentation()};
    auto grid = ssg::Renderer{}.render(projected);
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
    auto snapshot = ssg::test::SessionSnapshotBuilder{}.viewport(10, 5).build();
    ASSERT_EQ(snapshot.sections().shell.viewport.columns, 0);  // declined layout
    ssg::CellGrid grid;
    ASSERT_NO_THROW(grid = ssg::Renderer{}.render(snapshot));
    ASSERT_EQ(grid.size.columns, 10);
    ASSERT_EQ(grid.size.rows, 5);
    ASSERT_EQ(grid.cells.size(), std::size_t{50});
    // The message is centered on the middle row (rows/2 = 2) and clipped with an
    // ellipsis to the 10-column width.
    ASSERT_EQ(rowText(grid, 2), std::string("terminal \xe2\x80\xa6"));
    ASSERT_EQ(rowText(grid, 0), std::string(10, ' '));
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
    auto grid = ssg::Renderer{}.render(*snapshot);
    ASSERT_EQ(grid.size.columns, 24);
    ASSERT_EQ(grid.size.rows, 3);
    // 18-cell message centered in 24 columns -> start column (24-18)/2 = 3, on
    // the middle row (3/2 = 1).
    ASSERT_EQ(rowText(grid, 0), std::string(24, ' '));
    ASSERT_EQ(rowText(grid, 1),
              std::string("   terminal too small   "));
    ASSERT_EQ(rowText(grid, 2), std::string(24, ' '));
    // Determinism.
    ASSERT_EQ(ssg::Renderer{}.render(*snapshot).canonical(), grid.canonical());
    std::filesystem::remove_all(root);
}

TEST(renderTooSmallIsSafeAtOneByOne) {
    auto snapshot = ssg::test::SessionSnapshotBuilder{}.viewport(1, 1).build();
    ssg::CellGrid grid;
    ASSERT_NO_THROW(grid = ssg::Renderer{}.render(snapshot));
    ASSERT_EQ(grid.size.columns, 1);
    ASSERT_EQ(grid.size.rows, 1);
    ASSERT_EQ(grid.cells.size(), std::size_t{1});
}


// The cursor is how a user can tell a text input has focus, and a picker holds
// Prompt focus while reserving ZERO prompt rows -- so paintPrompt yields no
// caret and the cursor was previously left wherever painting finished
TEST(anOpenPickerPutsTheCaretAtTheEndOfTheTypedQuery) {
    auto root = uniqueRoot();
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    ASSERT_TRUE(runtime->dispatch(ssg::ClientId{1},
                                  {"palette.open", runtime->revision(), {}})
                    .accepted());

    // The client owns the query text and reports it through the palette report,
    // exactly as the app does.
    ssg::PaletteReport report;
    report.query = "save";
    auto snapshot = runtime->snapshot(ssg::ClientId{1}, {80, 24}, report);
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto grid = ssg::Renderer{}.render(*snapshot);

    ASSERT_TRUE(grid.caret.has_value());
    if (!grid.caret) return;
    // In the header row, not wherever painting stopped.
    ASSERT_EQ(grid.caret->row, std::uint32_t{0});

    // And exactly one cell past the last drawn character of "> save".
    auto const& shell = snapshot->sections().shell;
    const ssg::AccessibilityNode* query = nullptr;
    for (auto const& node : shell.accessibilityNodes) {
        if (node.id == "input_line.query") query = &node;
    }
    ASSERT_TRUE(query != nullptr);
    if (query) {
        ASSERT_EQ(grid.caret->column,
                  static_cast<std::uint32_t>(query->rect.x +
                                             static_cast<int>(query->content.size())));
    }
}

// Display width, not byte count: a multi-byte query would otherwise place the
// caret several columns short of the text.
TEST(theInputLineCaretIsPlacedByDisplayWidthNotByteCount) {
    auto root = uniqueRoot();
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    ASSERT_TRUE(runtime->dispatch(ssg::ClientId{1},
                                  {"palette.open", runtime->revision(), {}})
                    .accepted());

    ssg::PaletteReport report;
    report.query = "\u00e9\u00e9\u00e9";  // 3 characters, 6 bytes, 3 columns.
    auto snapshot = runtime->snapshot(ssg::ClientId{1}, {80, 24}, report);
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto grid = ssg::Renderer{}.render(*snapshot);
    ASSERT_TRUE(grid.caret.has_value());
    if (!grid.caret) return;

    auto const& shell = snapshot->sections().shell;
    const ssg::AccessibilityNode* query = nullptr;
    for (auto const& node : shell.accessibilityNodes) {
        if (node.id == "input_line.query") query = &node;
    }
    ASSERT_TRUE(query != nullptr);
    if (query) {
        // "> " plus three single-width characters = 5 columns, NOT 8 bytes.
        ASSERT_EQ(grid.caret->column,
                  static_cast<std::uint32_t>(query->rect.x + 5));
    }
}

// With a scrolled query the caret must sit at the end of the VISIBLE text --
// still inside the header, still marking where the next keystroke lands.
TEST(theCaretFollowsAScrolledQueryToTheEndOfTheVisibleText) {
    auto root = uniqueRoot();
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    ASSERT_TRUE(runtime->dispatch(ssg::ClientId{1},
                                  {"palette.open", runtime->revision(), {}})
                    .accepted());

    ssg::PaletteReport report;
    report.query = std::string(300, 'x');
    auto snapshot = runtime->snapshot(ssg::ClientId{1}, {80, 24}, report);
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto grid = ssg::Renderer{}.render(*snapshot);

    ASSERT_TRUE(grid.caret.has_value());
    if (!grid.caret) return;
    ASSERT_EQ(grid.caret->row, std::uint32_t{0});
    // Inside the header, not run off the right edge by the untruncated query.
    ASSERT_TRUE(grid.caret->column < std::uint32_t{80});

    auto const& shell = snapshot->sections().shell;
    const ssg::AccessibilityNode* query = nullptr;
    for (auto const& node : shell.accessibilityNodes) {
        if (node.id == "input_line.query") query = &node;
    }
    ASSERT_TRUE(query != nullptr);
    if (query) {
        // At the end of what is actually drawn.
        ASSERT_EQ(grid.caret->column,
                  static_cast<std::uint32_t>(query->rect.x + query->rect.width));
    }
}

// Proves the renderer READS the snapshot's published Style section rather than
// holding its own.  The existing suite passing only shows the refactor changed
// nothing; it cannot show the routing is live, because the defaults reproduce
// the old glyphs exactly.  So: restyle the published section, and require the
// screen to follow -- which also proves the runtime and renderer share it.
TEST(theRendererDrawsChromeFromTheSnapshotStyleNotFromLiterals) {
    auto root = uniqueRoot();
    for (int i = 0; i < 40; ++i) {
        std::ofstream{root / ("file-" + std::to_string(i) + ".txt")} << "x";
    }
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    (void)runtime->dispatch(ssg::ClientId{1}, {"panel.toggle", runtime->revision(), {}});
    (void)runtime->dispatch(ssg::ClientId{1}, {"tree.select_next", runtime->revision(), {}});
    (void)runtime->dispatch(ssg::ClientId{1}, {"tree.activate", runtime->revision(), {}});
    auto snapshot = runtime->snapshot(ssg::ClientId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto const& shell = snapshot->sections().shell;
    ASSERT_TRUE(shell.panelScrollbar.has_value());
    if (!shell.panelScrollbar) return;

    ssg::Style style;
    style.scrollbar.track = ":";
    style.scrollbar.single = "@";
    style.scrollbar.top = "@";
    style.scrollbar.body = "@";
    style.scrollbar.bottom = "@";
    style.tree.collapsed = "+ ";
    style.tree.expanded = "- ";
    auto const grid = ssg::Renderer{}.render(withStyle(*snapshot, style));

    int const gx = shell.panelScrollbar->x;
    bool restyledThumb = false;
    bool restyledTrack = false;
    for (int y = shell.panelScrollbar->y;
         y < shell.panelScrollbar->y + shell.panelScrollbar->height; ++y) {
        auto const& text = grid.at(gx, y).text;
        if (text == "@") restyledThumb = true;
        if (text == ":") restyledTrack = true;
        // The shipped glyphs must be gone entirely, not merely joined.
        ASSERT_NE(text, std::string{"#"});
        ASSERT_NE(text, std::string{"|"});
    }
    ASSERT_TRUE(restyledThumb);
    ASSERT_TRUE(restyledTrack);

    // The tree indicator follows too, so panel painting is routed as well.
    ASSERT_TRUE(gridContains(grid, "- "));
}

// The replacement glyph is the one chrome glyph paintDocument draws, and it was
// duplicated at four sites before Y2's sweep.  A document with a control byte,
// rendered with a restyled `unrenderable`, must show the restyled glyph -- proof
// paintDocument reads the snapshot's Style rather than holding the literal.
TEST(theDocumentReplacementGlyphComesFromStyle) {
    ssg::Style restyled;
    restyled.unrenderable = "?";

    auto const shipped = ssg::Renderer{}.render(
        ssg::test::SessionSnapshotBuilder{}
            .document("a\x01" "b\n")  // \x01 has no glyph
            .viewport(80, 24)
            .build());
    ASSERT_TRUE(gridContains(shipped, "\xef\xbf\xbd"));  // U+FFFD by default

    auto const grid = ssg::Renderer{}.render(
        ssg::test::SessionSnapshotBuilder{}
            .document("a\x01" "b\n")
            .viewport(80, 24)
            .style(restyled)
            .build());
    ASSERT_TRUE(gridContains(grid, "a?b"));
    ASSERT_FALSE(gridContains(grid, "\xef\xbf\xbd"));
}

// The whole point of Y4+style.define: an init-script command restyles the live
// session.  Dispatch it and require both the published Style section and the
// rendered chrome to follow -- proof the command mutates the one shared instance
// the renderer reads.
TEST(styleDefineRestylesTheLiveSessionChrome) {
    auto root = uniqueRoot();
    for (int i = 0; i < 40; ++i) {
        std::ofstream{root / ("file-" + std::to_string(i) + ".txt")} << "x";
    }
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    (void)runtime->dispatch(ssg::ClientId{1}, {"panel.toggle", runtime->revision(), {}});
    (void)runtime->dispatch(ssg::ClientId{1}, {"tree.select_next", runtime->revision(), {}});
    (void)runtime->dispatch(ssg::ClientId{1}, {"tree.activate", runtime->revision(), {}});

    ssg::StyleDefineArguments args;
    args.values = {{"scrollbar_track", ":"},
                   {"scrollbar_single", "@"},
                   {"scrollbar_top", "@"},
                   {"scrollbar_body", "@"},
                   {"scrollbar_bottom", "@"},
                   {"tree_collapsed", "+ "},
                   {"tree_expanded", "- "}};
    auto const applied =
        runtime->dispatch(ssg::ClientId{1}, {"style.define", runtime->revision(), args});
    ASSERT_TRUE(applied.accepted());

    auto snapshot = runtime->snapshot(ssg::ClientId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    // The published section carries the new glyphs.
    ASSERT_EQ(snapshot->presentation()->style.scrollbar.track, std::string{":"});
    ASSERT_EQ(snapshot->presentation()->style.tree.expanded, std::string{"- "});

    // And the rendered screen shows them, with the shipped glyphs gone.
    auto const grid = ssg::Renderer{}.render(*snapshot);
    auto const& shell = snapshot->sections().shell;
    ASSERT_TRUE(shell.panelScrollbar.has_value());
    if (!shell.panelScrollbar) return;
    int const gx = shell.panelScrollbar->x;
    bool restyled = false;
    for (int y = shell.panelScrollbar->y;
         y < shell.panelScrollbar->y + shell.panelScrollbar->height; ++y) {
        auto const& text = grid.at(gx, y).text;
        if (text == "@" || text == ":") restyled = true;
        ASSERT_NE(text, std::string{"#"});
        ASSERT_NE(text, std::string{"|"});
    }
    ASSERT_TRUE(restyled);
    ASSERT_TRUE(gridContains(grid, "- "));
}

// style.define is transactional at the field level too: a table with an unknown
// key is rejected whole, and the live session keeps its previous style.
TEST(styleDefineRejectionLeavesTheLiveStyleUnchanged) {
    auto root = uniqueRoot();
    std::ofstream{root / "a.txt"} << "x";
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;

    ssg::StyleDefineArguments args;
    args.values = {{"tree_expanded", "- "}, {"bogus_key", "z"}};
    auto const rejected =
        runtime->dispatch(ssg::ClientId{1}, {"style.define", runtime->revision(), args});
    ASSERT_FALSE(rejected.accepted());

    auto snapshot = runtime->snapshot(ssg::ClientId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    // The good key in the rejected table did NOT leak into the live style.
    ASSERT_EQ(snapshot->presentation()->style.tree.expanded, ssg::Style{}.tree.expanded);
}

// LSP diagnostics reach the cells they cover, and only those cells.  The ranges
// are LSP positions (UTF-16 characters), so the conversion is what makes a
// squiggle land under the right text rather than shifted by every non-ASCII
// character earlier on the line.
TEST(lspDiagnosticsUnderlineExactlyTheirRange) {
    ssg::test::SessionSnapshotBuilder builder;
    auto snapshot =
        builder.document("alpha bravo\ncharlie\n")
            .viewport(40, 10)
            .sections([](ssg::SessionSnapshotSections& sections) {
                ssg::LspDocumentDiagnostics file;
                file.uri = "file:///doc";
                file.revision = sections.document.revision;
                // "bravo" on line 0: characters 6..11.
                file.diagnostics.push_back(
                    {{{0, 6}, {0, 11}}, ssg::LspDiagnosticSeverity::Error, "E1",
                     "bad"});
                // "charlie" on line 1, a warning.
                file.diagnostics.push_back(
                    {{{1, 0}, {1, 7}}, ssg::LspDiagnosticSeverity::Warning, "W1",
                     "meh"});
                sections.lspSync.documents.push_back(std::move(file));
            })
            .build();
    auto const grid = ssg::Renderer{}.render(snapshot);

    // Find the row carrying "alpha bravo" and check exactly its last five cells
    // are underlined as an error.
    auto const cellAt = [&](int x, int y) {
        return grid.cells[static_cast<std::size_t>(y * grid.size.columns + x)];
    };
    int alphaRow = -1;
    int charlieRow = -1;
    for (int y = 0; y < grid.size.rows; ++y) {
        if (cellAt(0, y).text == "a" && cellAt(1, y).text == "l") alphaRow = y;
        if (cellAt(0, y).text == "c" && cellAt(1, y).text == "h") charlieRow = y;
    }
    ASSERT_TRUE(alphaRow >= 0);
    ASSERT_TRUE(charlieRow >= 0);
    if (alphaRow < 0 || charlieRow < 0) return;

    for (int x = 0; x < 6; ++x) {
        // "alpha " is not part of the diagnostic.
        ASSERT_TRUE(cellAt(x, alphaRow).underline == ssg::CellUnderline::None);
    }
    for (int x = 6; x < 11; ++x) {
        ASSERT_TRUE(cellAt(x, alphaRow).underline == ssg::CellUnderline::Error);
    }
    ASSERT_TRUE(cellAt(11, alphaRow).underline == ssg::CellUnderline::None);
    for (int x = 0; x < 7; ++x) {
        ASSERT_TRUE(cellAt(x, charlieRow).underline == ssg::CellUnderline::Warning);
    }

    // The text itself is untouched: a diagnostic decorates, it does not replace.
    ASSERT_EQ(cellAt(6, alphaRow).text, std::string{"b"});
}

// Diagnostic ranges are offsets into a SPECIFIC revision.  Painting ones the
// server produced for an older revision underlines whatever text has since moved
// into those positions, which is worse than showing nothing.
TEST(staleDiagnosticsAreNotPainted) {
    ssg::test::SessionSnapshotBuilder builder;
    auto snapshot =
        builder.document("alpha bravo\n")
            .viewport(40, 10)
            .sections([](ssg::SessionSnapshotSections& sections) {
                ssg::LspDocumentDiagnostics file;
                file.uri = "file:///doc";
                // Deliberately not the document's revision.
                file.revision = ssg::Revision{sections.document.revision.value() + 1};
                file.diagnostics.push_back(
                    {{{0, 0}, {0, 5}}, ssg::LspDiagnosticSeverity::Error, "E1",
                     "bad"});
                sections.lspSync.documents.push_back(std::move(file));
            })
            .build();
    auto const grid = ssg::Renderer{}.render(snapshot);
    for (auto const& cell : grid.cells) {
        ASSERT_TRUE(cell.underline == ssg::CellUnderline::None);
    }
}

// A URL is clickable wherever it appears -- a comment, a string, a markdown
// link, a plain note -- because it is detected from the TEXT, not from one
// grammar's captures.  Coupling it to syntax would make it work in markdown and
// nowhere else.
TEST(urlsInTheDocumentBecomeClickableRuns) {
    ssg::test::SessionSnapshotBuilder builder;
    auto snapshot = builder.document("see https://example.com/a for more\n")
                        .viewport(60, 8)
                        .build();
    auto const grid = ssg::Renderer{}.render(snapshot);
    ASSERT_EQ(grid.hyperlinks.size(), std::size_t{1});
    if (grid.hyperlinks.empty()) return;
    auto const& link = grid.hyperlinks.front();
    ASSERT_EQ(link.uri, std::string{"https://example.com/a"});
    // Exactly the URL's cells: the words around it are not part of the link.
    ASSERT_EQ(link.width, static_cast<int>(link.uri.size()));
    auto const cellAt = [&](int x, int y) {
        return grid.cells[static_cast<std::size_t>(y * grid.size.columns + x)].text;
    };
    ASSERT_EQ(cellAt(link.column, link.row), std::string{"h"});
    ASSERT_EQ(cellAt(link.column - 1, link.row), std::string{" "});
}

// Trailing punctuation belongs to the sentence, not the URL, and a scheme in the
// middle of a word is not a link at all.
TEST(urlDetectionStopsAtSentenceAndBracketBoundaries) {
    auto linksFor = [](std::string text) {
        ssg::test::SessionSnapshotBuilder builder;
        auto snapshot = builder.document(std::move(text)).viewport(80, 8).build();
        return ssg::Renderer{}.render(snapshot).hyperlinks;
    };

    auto const sentence = linksFor("go to https://example.com.\n");
    ASSERT_EQ(sentence.size(), std::size_t{1});
    if (!sentence.empty()) {
        ASSERT_EQ(sentence.front().uri, std::string{"https://example.com"});
    }

    // A markdown link's closing paren is not part of the destination.
    auto const markdown = linksFor("[a](https://example.com/x) tail\n");
    ASSERT_EQ(markdown.size(), std::size_t{1});
    if (!markdown.empty()) {
        ASSERT_EQ(markdown.front().uri, std::string{"https://example.com/x"});
    }

    // A scheme inside a word is not a URL.
    ASSERT_TRUE(linksFor("nothttps://example.com\n").empty());
    // A bare scheme with no host is not a URL either.
    ASSERT_TRUE(linksFor("https:// nothing\n").empty());
    // Plain prose has none.
    ASSERT_TRUE(linksFor("no links here at all\n").empty());
}

// The dead-color-role guard. Proves the
// theme.set name surface equals the actually-color-consumed role surface, with
// `Caret` the sole exception (a live cell-role TAG whose color is intentionally
// unread). Not a tautology: it paints a snapshot exercising every surface
// through the real render path and asserts each role's color is observed in the
// output. A future role that accepts a color but paints nothing, or a surface
// that stops consuming its role, fails here.
TEST(everyNonCaretSemanticRoleIsColorConsumedByTheRenderer) {
    using ssg::SemanticRole;
    // A sentinel theme: each role's color is unique, so an observed color names
    // exactly one role. Roles are reds (index in the red channel), scopes are
    // greens, both offset past 0 so a default-constructed slot cannot collide.
    ssg::ThemeSnapshot theme{};
    for (std::size_t i = 0; i < theme.roleColors.size(); ++i) {
        theme.roleColors[i] = ssg::SrgbColor::fromSerializedChannels(
            static_cast<std::uint8_t>(30 + i), 1, 1);
    }
    for (std::size_t i = 0; i < theme.syntaxColors.size(); ++i) {
        theme.syntaxColors[i] = ssg::SrgbColor::fromSerializedChannels(
            2, static_cast<std::uint8_t>(30 + i), 2);
    }

    // A tall document so the gutter shows line numbers and the pane scrollbar
    // draws a thumb.
    std::string document;
    for (int i = 0; i < 80; ++i) {
        document += "line " + std::to_string(i) + " of the document\n";
    }

    auto makeSnapshot = [&](bool pickerOpen) {
        return ssg::test::SessionSnapshotBuilder{}
            .document(document)
            .viewport(120, 40)
            .panel(true)
            .panelFocused(false)
            .shellRequest([pickerOpen](ssg::ShellLayoutRequest& request) {
                request.headerFields = {
                    {"cwd", "Working directory", "~/project", 0, std::nullopt}};
                request.footerFields = {
                    {"encoding", "Encoding", "UTF-8", 0, std::nullopt}};
                request.footerActions = {{"footer.act", "Save"}};
                request.tabs = {{"a.txt", "Tab a.txt", true, false},
                                {"b.txt", "Tab b.txt", false, false}};
                request.notice =
                    ssg::ShellNotice{"Draft conflict",
                                     {{"diff", "Diff", "draft.diff"}}};
                request.inputLineActive = pickerOpen;
                request.inputLineQuery = "needle";
                request.inputLineGhost = "ghost";
                // A line-number gutter so the LineNumber and current-line roles
                // are exercised (the caret line uses the current-line roles).
                request.lineNumberGutterWidth = 3;
            })
            .sections([&](ssg::SessionSnapshotSections& sections) {
                sections.theme = theme;
                // A tree with a directory node (PanelActive color) and a
                // selected file node (TreeFocus fill); the panel provider node
                // (PanelInactive) and panel fill (TreeBackground) come for free
                // from the shown panel.
                ssg::TreeNode dir{ssg::TreeNodeId{"d"}, std::nullopt, "src",
                                  ssg::TreeNodeKind::Directory};
                dir.expandable = true;
                ssg::TreeNode file{ssg::TreeNodeId{"f"}, ssg::TreeNodeId{"d"},
                                   "main.cpp", ssg::TreeNodeKind::File};
                ssg::TreeProviderView provider{
                    ssg::TreeProviderId{"files"},
                    ssg::TreeProviderKind::Filesystem,
                    {{dir, 0, true}, {file, 1, false}},
                    ssg::TreeNodeId{"f"}};
                sections.tree.providers = {provider};
                // A ranged selection on line 0 paints real Selection-role
                // cells, so Selection is proven consumed at the cell level, not
                // only via grid.selectionFill.
                sections.selection = ssg::SelectionViewState{
                    ssg::SelectionSet{{ssg::Selection{
                        {ssg::ByteOffset{0}, ssg::LineIndex{0},
                         ssg::CellIndex{0}},
                        {ssg::ByteOffset{4}, ssg::LineIndex{0},
                         ssg::CellIndex{4}}}}},
                    0, 0, std::nullopt};
                // A diff overlay tints document rows, so the DiffAdded/
                // DiffModified paths reach real cells (a removed line has no
                // target row to tint; DiffRemoved's cell-level painting is
                // covered by test_renderer_diff_overlay). The document opts in
                // by naming the diff file identity the view carries.
                sections.document.diffFileIdentity = "guard.diff";
                ssg::DiffFileView diffFile{
                    ssg::DiffFileId{"guard.diff"}, {}, std::nullopt, false,
                    ssg::DiffFileStatus::Modified, {}, {}, {}, {}};
                diffFile.changedLines = {
                    {ssg::DiffLineKind::Added, std::nullopt, std::size_t{0},
                     {}, {}, {}, {}},
                    {ssg::DiffLineKind::Modified, std::nullopt, std::size_t{1},
                     {}, {}, {}, {}}};
                sections.diff.revision = sections.document.revision;
                sections.diff.files = {diffFile};
                // An inactive find match (off the selection, on line 1) paints a
                // cell with the SearchMatch background (an active match would use
                // Selection instead).
                sections.findReplace.open = true;
                sections.findReplace.sourceRevision =
                    sections.document.revision;
                sections.findReplace.matches = {
                    {ssg::ByteOffset{25}, ssg::ByteOffset{29}}};
                sections.findReplace.activeMatch = std::nullopt;
            })
            .build();
    };

    // A picker covers the tab bar, so tab roles paint only with it CLOSED and
    // the input-line Prompt role only with it OPEN -- mutually exclusive states.
    // Union both renders so every role has a frame that paints it.
    auto snapshot = makeSnapshot(true);
    auto grid = ssg::Renderer{}.render(snapshot);

    // Cell-level evidence that the selection and diff surfaces actually paint,
    // so treating Selection/Diff* colors (carried on grid.selectionFill/
    // diffTints) as consumed is not vacuous: a regression that stopped painting
    // selected or tinted cells fails here regardless of the grid-level copies.
    bool anySelectionCell = false;
    bool anyTintedCell = false;
    for (auto const& cell : grid.cells) {
        if (cell.role == SemanticRole::Selection) anySelectionCell = true;
        if (cell.tint != ssg::DiffTint::None) anyTintedCell = true;
    }
    ASSERT_TRUE(anySelectionCell);
    ASSERT_TRUE(anyTintedCell);

    // Every color the renderer actually emitted: each cell's resolved
    // foreground and background, plus the diff washes and selection fill, which
    // travel on the grid rather than in a cell's fg/bg slot.
    std::set<std::uint32_t> emitted;
    auto const encode = [](ssg::SrgbColor c) {
        return (static_cast<std::uint32_t>(c.red) << 16) |
               (static_cast<std::uint32_t>(c.green) << 8) | c.blue;
    };
    for (auto const& cell : grid.cells) {
        emitted.insert(encode(grid.colors[cell.foreground]));
        emitted.insert(encode(grid.colors[cell.background]));
    }
    for (auto c : {grid.diffTints.addedRow, grid.diffTints.removedRow,
                   grid.diffTints.modifiedRow, grid.diffTints.addedWord,
                   grid.diffTints.removedWord, grid.diffTints.modifiedWord}) {
        emitted.insert(encode(c));
    }
    emitted.insert(encode(grid.selectionFill));

    // The tab bar is hidden while the picker is open, so render a picker-CLOSED
    // frame too and union its colors -- that is the frame in which the tab roles
    // paint.
    auto tabGrid = ssg::Renderer{}.render(makeSnapshot(false));
    for (auto const& cell : tabGrid.cells) {
        emitted.insert(encode(tabGrid.colors[cell.foreground]));
        emitted.insert(encode(tabGrid.colors[cell.background]));
    }

    for (auto const role : ssg::kAllSemanticRoles) {
        // Caret is a live cell-role TAG whose color is intentionally unread:
        // the primary caret is the terminal hardware cursor and the secondary
        // caret inverts the cell it sits on.
        if (role == SemanticRole::Caret) continue;
        auto const present =
            emitted.count(encode(theme.color(role))) != 0;
        if (!present) {
            std::printf("  role never painted: %.*s\n",
                        static_cast<int>(ssg::semanticRoleName(role).size()),
                        ssg::semanticRoleName(role).data());
        }
        ASSERT_TRUE(present);
    }
}

int main() {
    RUN(everyNonCaretSemanticRoleIsColorConsumedByTheRenderer);
    RUN(chromeBackgroundsAreDistinctShadesAndTheActiveTabMergesWithTheDocument);
    RUN(renderPaintsContentNotAccessibilityLabels);
    RUN(lineNumberGutterPaintsNumbersAndHighlightsTheCaretLine);
    RUN(lineNumberGutterHighlightsEveryCursorLineNotJustThePrimary);
    RUN(renderSegmentsOnlyVisibleLinesNotWholeDocument);
    RUN(wordWrapOffRendersHorizontallyScrolledContent);
    RUN(renderColorsAreInBoundsColorSlots);
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
    RUN(renderPromptControlLabelsAreLowercaseChrome);
    RUN(renderPanelTreeWindowsAndDrawsAThumbWhenTallerThanThePanel);
    RUN(renderPanelTreeReservesAnEmptyGutterWhenItFits);
    RUN(renderPaletteWindowsRowsAndDrawsAThumbWithAbsoluteSelection);
    RUN(renderPaletteReservesAnEmptyGutterWhenTheListFits);
    RUN(renderTooSmallViewportProducesLibraryPlaceholder);
    RUN(renderTooSmallMatchesHandAuthoredGolden);
    RUN(renderTooSmallIsSafeAtOneByOne);
    RUN(anOpenPickerPutsTheCaretAtTheEndOfTheTypedQuery);
    RUN(theInputLineCaretIsPlacedByDisplayWidthNotByteCount);
    RUN(theCaretFollowsAScrolledQueryToTheEndOfTheVisibleText);
    RUN(theRendererDrawsChromeFromTheSnapshotStyleNotFromLiterals);
    RUN(theDocumentReplacementGlyphComesFromStyle);
    RUN(styleDefineRestylesTheLiveSessionChrome);
    RUN(urlsInTheDocumentBecomeClickableRuns);
    RUN(urlDetectionStopsAtSentenceAndBracketBoundaries);
    RUN(lspDiagnosticsUnderlineExactlyTheirRange);
    RUN(staleDiagnosticsAreNotPainted);
    RUN(styleDefineRejectionLeavesTheLiveStyleUnchanged);

    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed > 0 ? 1 : 0;
}
