#include <tui/Renderer.h>

#include <ssg/EditorSession.h>
#include <ssg/FindReplace.h>
#include <tui/HitTester.h>
#include <ssg/PaletteSearcher.h>
#include <ssg/StatusFields.h>
#include <ssg/StatusQueue.h>
#include <ssg/WholeScreenAssembly.h>
#include <ssg/Selection.h>
#include <ssg/TreeModel.h>
#include <ssg/session_snapshot.h>

#include "session_snapshot_builder.h"
#include "grid_test_frame.h"
#include "grid_test_view.h"
#include "test_helpers.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <variant>

namespace fs = std::filesystem;

namespace {

fs::path uniqueRoot() {
    auto base = fs::temp_directory_path() /
                ("ssg-render-" + std::to_string(::rand()));
    fs::create_directories(base / "scratch");
    fs::create_directories(base / "recovery");
    return base;
}

std::unique_ptr<ssg::EditorSession> makeRuntime(fs::path const& root) {
    ssg::EditorSessionConfig config{
        root, root / "scratch", root / "recovery"};
    config.enableGitDiffWorker = false;
    config.enableFilesystemWatcher = false;
    auto created = ssg::EditorSession::create(config);
    if (!created.accepted()) return nullptr;
    auto runtime = std::move(created.session);
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
ssg::GridPresentation withStyle(ssg::GridPresentation const& base, ssg::Style style) {
    auto projection = base.presentation();
    projection.style = std::move(style);
    return ssg::test::copyGridFrame(base, ssg::ViewId{1}, base.sections(), std::move(projection), base.palette());
}

ssg::UiNode* mutableUiNode(ssg::UiNode& node, std::string_view id) {
    if (node.id.value() == id) return &node;
    if (auto* container = std::get_if<ssg::UiContainer>(&node.content)) {
        for (auto& child : container->children) {
            if (auto* found = mutableUiNode(child, id)) return found;
        }
    }
    return nullptr;
}

ssg::UiNode* mutableUiNodeForWidget(ssg::UiNode& node,
                                    std::string_view widgetId) {
    if (auto* leaf = std::get_if<ssg::UiLeaf>(&node.content);
        leaf && leaf->widget.id == widgetId) {
        return &node;
    }
    if (auto* container = std::get_if<ssg::UiContainer>(&node.content)) {
        for (auto& child : container->children) {
            if (auto* found = mutableUiNodeForWidget(child, widgetId))
                return found;
        }
    }
    return nullptr;
}

ssg::GridPresentation withUiBackgrounds(
    ssg::GridPresentation const& base,
    std::initializer_list<std::pair<std::string_view, ssg::SemanticRole>>
        backgrounds) {
    auto sections = base.sections();
    for (const auto& [id, role] : backgrounds) {
        auto* node = mutableUiNode(sections.uiTree.root, id);
        if (node) node->style.background = role;
    }
    return ssg::test::copyGridFrame(base, ssg::ViewId{1}, std::move(sections), base.presentation(), base.palette());
}

ssg::GridPresentation withUiForeground(
    ssg::GridPresentation const& base, std::string_view id,
    ssg::SemanticRole foreground) {
    auto sections = base.sections();
    auto* node = mutableUiNode(sections.uiTree.root, id);
    if (node) node->style.foreground = foreground;
    return ssg::test::copyGridFrame(base, ssg::ViewId{1}, std::move(sections), base.presentation(), base.palette());
}

ssg::GridPresentation withUiWidgetRole(
    ssg::GridPresentation const& base,
    std::string_view widgetId, std::string role) {
    auto sections = base.sections();
    auto* node = mutableUiNodeForWidget(sections.uiTree.root, widgetId);
    if (node) std::get<ssg::UiLeaf>(node->content).widget.role = std::move(role);
    return ssg::test::copyGridFrame(base, ssg::ViewId{1}, std::move(sections), base.presentation(), base.palette());
}

void showPicker(ssg::SessionSnapshotSections& sections) {
    sections.palette.activePicker = ssg::PickerActivation{
        ssg::SearchMode::Command, ssg::PickerActivationId{1}};
    if (auto* editor =
            mutableUiNode(sections.uiTree.root, ssg::kEditorNodeId)) {
        editor->visible = false;
    }
    if (auto* findResults = mutableUiNode(
            sections.uiTree.root, ssg::kFindResultsViewportNodeId)) {
        findResults->visible = true;
    }
    if (auto* promptInput = mutableUiNode(
            sections.uiTree.root, ssg::kHeaderPromptInputNodeId)) {
        promptInput->visible = true;
    }
    sections.uiTree.focusPath = std::vector<ssg::UiNodeId>{
        ssg::UiNodeId{std::string{ssg::kEditorNodeId}},
        ssg::UiNodeId{std::string{ssg::kHeaderPromptInputNodeId}}};
}

ssg::PaletteReport paletteReport(
    ssg::PaletteProjection const& projection) {
    ssg::PaletteReport report;
    report.selected = projection.selected;
    report.firstVisible = projection.firstVisible;
    report.scrollbar = projection.scrollbar;
    for (auto const& row : projection.rows) {
        report.rows.push_back({"", row.label, row.detail});
    }
    return report;
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
    (void)runtime->dispatch({"file.open", runtime->revision(), std::string{"alpha.txt"}});
    (void)runtime->dispatch({"file.open", runtime->revision(), std::string{"beta.txt"}});
    auto snapshot = ssg::test::projectGridFrame(*runtime, ssg::ViewId{1}, {60, 12});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    const auto* tabBar = snapshot->layout().find(
        ssg::UiNodeId{std::string{ssg::kTabBarNodeId}});
    ASSERT_TRUE(snapshot->header().has_value());
    ASSERT_TRUE(snapshot->footer().has_value());
    ASSERT_TRUE(tabBar != nullptr);
    if (!snapshot->header() || !snapshot->footer() || !tabBar) return;
    auto grid = ssg::Renderer{}.render(*snapshot);

    const auto& theme = snapshot->semantic().sections().theme;
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
    ASSERT_EQ(colorOf(tabBar->rect.right() - 1, tabBar->rect.y), tabBand);
    ASSERT_EQ(colorOf(snapshot->header()->rect.x, snapshot->header()->rect.y),
              headerBand);
    ASSERT_EQ(colorOf(snapshot->footer()->rect.x, snapshot->footer()->rect.y),
              footerBand);

    // The active tab's first cell carries the document Background, not the band,
    // so it merges with the content below.
    const auto solvedTabs = ssg::solveTabBar(
        snapshot->sections().tabs, snapshot->presentation().style.tab,
        tabBar->rect);
    const auto active = std::ranges::find_if(
        solvedTabs.tabs, [](const ssg::SolvedTab& tab) {
            return tab.active;
        });
    const int activeTabX =
        active == solvedTabs.tabs.end() ? -1 : active->rect.x;
    ASSERT_NE(activeTabX, -1);
    if (activeTabX >= 0) {
        ASSERT_EQ(colorOf(activeTabX, tabBar->rect.y), docColor);
    }
}

TEST(headerAndFooterCellsAndHitsUseTheSolvedTree) {
    auto root = uniqueRoot();
    std::ofstream{root / "doc.txt"} << "one\n";
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    (void)runtime->dispatch({"file.open", runtime->revision(), std::string{"doc.txt"}});
    auto projected = ssg::test::projectGridFrame(*runtime, ssg::ViewId{1}, {80, 24});
    ASSERT_TRUE(projected.has_value());
    if (!projected) return;

    auto& frame = *projected;

    const auto* header =
        frame.layout().find(ssg::UiNodeId{std::string{ssg::kHeaderNodeId}});
    const auto* footer =
        frame.layout().find(ssg::UiNodeId{std::string{ssg::kFooterNodeId}});
    ASSERT_TRUE(header != nullptr);
    ASSERT_TRUE(footer != nullptr);
    if (!header || !footer) return;
    ASSERT_EQ(header->rect, (ssg::Rect{0, 0, 80, 1}));
    ASSERT_EQ(footer->rect, (ssg::Rect{0, 23, 80, 1}));

    const auto grid = ssg::Renderer{}.render(frame);
    const auto headerCell = grid.at(79, header->rect.y);
    const auto footerCell = grid.at(40, footer->rect.y);
    ASSERT_EQ(headerCell.role, ssg::SemanticRole::HeaderBackground);
    ASSERT_EQ(footerCell.role, ssg::SemanticRole::FooterBackground);
    ASSERT_EQ(ssg::HitTester{frame}.at(79, header->rect.y).region,
              ssg::HitRegion::None);
    ASSERT_EQ(ssg::HitTester{frame}.at(40, footer->rect.y).region,
              ssg::HitRegion::None);
}

TEST(rendererGetsRegionBackgroundsFromTheUiTree) {
    auto root = uniqueRoot();
    std::ofstream{root / "alpha.txt"} << "one\n";
    std::ofstream{root / "beta.txt"} << "two\n";
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    (void)runtime->dispatch({"file.open", runtime->revision(),
                             std::string{"alpha.txt"}});
    (void)runtime->dispatch({"file.open", runtime->revision(),
                             std::string{"beta.txt"}});
    (void)runtime->dispatch({"panel.toggle", runtime->revision(), {}});
    auto snapshot = ssg::test::projectGridFrame(*runtime, ssg::ViewId{1}, {60, 12});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto styled = withUiBackgrounds(
        *snapshot,
        {{ssg::kHeaderNodeId, ssg::SemanticRole::Selection},
         {ssg::kFooterNodeId, ssg::SemanticRole::SearchMatch},
         {ssg::kPanelNodeId,
          ssg::SemanticRole::CurrentLineNumberBackground},
         {ssg::kTabBarNodeId, ssg::SemanticRole::LineNumberBackground},
         {ssg::kDocumentNodeId, ssg::SemanticRole::FooterBackground}});
    styled = withUiForeground(styled, ssg::kHeaderNodeId,
                             ssg::SemanticRole::CurrentLineNumber);
    const auto grid = ssg::Renderer{}.render(styled);
    const auto* header = styled.layout().find(
        ssg::UiNodeId{std::string{ssg::kHeaderNodeId}});
    const auto* footer = styled.layout().find(
        ssg::UiNodeId{std::string{ssg::kFooterNodeId}});
    const auto* panel = styled.layout().find(
        ssg::UiNodeId{std::string{ssg::kPanelNodeId}});
    const auto* tabBar = styled.layout().find(
        ssg::UiNodeId{std::string{ssg::kTabBarNodeId}});
    ASSERT_TRUE(header && footer && panel && tabBar && styled.document());
    if (!header || !footer || !panel || !tabBar || !styled.document()) return;
    const auto colorAt = [&](int x, int y) {
        return grid.colors[grid.at(x, y).background];
    };
    ASSERT_EQ(colorAt(header->rect.right() - 1, header->rect.y),
              styled.semantic().sections().theme.color(ssg::SemanticRole::Selection));
    ASSERT_EQ(colorAt(footer->rect.x, footer->rect.y),
              styled.semantic().sections().theme.color(ssg::SemanticRole::SearchMatch));
    ASSERT_EQ(colorAt(panel->rect.x, panel->rect.bottom() - 1),
              styled.semantic().sections().theme.color(
                  ssg::SemanticRole::CurrentLineNumberBackground));
    ASSERT_EQ(colorAt(tabBar->rect.right() - 1, tabBar->rect.y),
              styled.semantic().sections().theme.color(
                  ssg::SemanticRole::LineNumberBackground));
    ASSERT_EQ(colorAt(styled.document()->content.right() - 1,
                     styled.document()->content.bottom() - 1),
              styled.semantic().sections().theme.color(
                  ssg::SemanticRole::FooterBackground));
    const auto headerGlyph = std::ranges::find_if(
        styled.header()->items, [](const ssg::SolvedUiItem& item) {
            return !item.content.empty();
        });
    ASSERT_TRUE(headerGlyph != styled.header()->items.end());
    if (headerGlyph != styled.header()->items.end()) {
        ASSERT_EQ(grid.colors[grid.at(headerGlyph->rect.x, headerGlyph->rect.y)
                                 .foreground],
                  styled.semantic().sections().theme.color(
                      ssg::SemanticRole::CurrentLineNumber));
        const auto overridden =
            withUiWidgetRole(styled, headerGlyph->id, "header");
        const auto overriddenGrid =
            ssg::Renderer{}.render(overridden);
        ASSERT_EQ(overriddenGrid.colors[
                      overriddenGrid
                          .at(headerGlyph->rect.x, headerGlyph->rect.y)
                          .foreground],
                  styled.semantic().sections().theme.color(ssg::SemanticRole::Header));
        ASSERT_EQ(overriddenGrid.colors[
                      overriddenGrid
                          .at(headerGlyph->rect.x, headerGlyph->rect.y)
                          .background],
                  styled.semantic().sections().theme.color(ssg::SemanticRole::Selection));
    }
    fs::remove_all(root);
}

TEST(renderPaintsContentNotAccessibilityLabels) {
    auto root = uniqueRoot();
    std::ofstream{root / "hello.txt"} << "alpha\nbeta\n";
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    (void)runtime->dispatch({"file.open", runtime->revision(), std::string{"hello.txt"}});
    auto snapshot = ssg::test::projectGridFrame(*runtime, ssg::ViewId{1}, {80, 24});
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
    (void)runtime->dispatch({"file.open", runtime->revision(), std::string{"n.txt"}});
    (void)runtime->dispatch({"view.toggle_line_numbers", runtime->revision(), {}});
    // Put the caret on line 2 (0-indexed 1) so its number highlights.
    auto atBeta = ssg::SelectionNavigator::resolvePosition("alpha\nbeta\ngamma\n",
                                                           ssg::ByteOffset{6});
    ASSERT_TRUE(atBeta.has_value());
    (void)runtime->dispatch({"cursor.set_position", runtime->revision(),
         ssg::SelectionCommandArguments{atBeta, std::nullopt}});

    auto snapshot = ssg::test::projectGridFrame(*runtime, ssg::ViewId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto& frame = *snapshot;
    ASSERT_TRUE(frame.document().has_value());
    if (!frame.document()) return;
    auto const& document = *frame.document();
    auto grid = ssg::Renderer{}.render(frame);
    ASSERT_EQ(grid.at(document.content.x, document.content.y).text,
              std::string{"a"});
    ASSERT_TRUE(grid.caret.has_value());
    if (grid.caret) {
        ASSERT_EQ(grid.caret->column, document.content.x);
        ASSERT_EQ(grid.caret->row, document.content.y + 1);
    }
    int const gx = document.lineNumbers.x;
    int const gw = document.lineNumbers.width;
    ASSERT_EQ(gw, 2);
    auto gutterText = [&](int row) {
        std::string s;
        for (int c = 0; c < gw; ++c) {
            s += grid.at(gx + c, document.lineNumbers.y + row).text;
        }
        return s;
    };
    // Right-aligned number + trailing space: "1 ", "2 ", "3 ".
    ASSERT_EQ(gutterText(0), std::string{"1 "});
    ASSERT_EQ(gutterText(1), std::string{"2 "});
    ASSERT_EQ(gutterText(2), std::string{"3 "});
    // The caret's line (row 1) uses the current-line roles; others use LineNumber.
    ASSERT_EQ(grid.at(gx, document.lineNumbers.y + 1).role,
              ssg::SemanticRole::CurrentLineNumber);
    ASSERT_EQ(grid.at(gx, document.lineNumbers.y + 0).role,
              ssg::SemanticRole::LineNumber);
    // The inactive gutter has its own background band: distinct from the
    // document content background beside it AND from the current line's band.
    auto gutterBg = [&](int row) {
        return grid.colors[
            grid.at(gx, document.lineNumbers.y + row).background];
    };
    auto const contentBg =
        grid.colors[grid.at(document.content.x, document.content.y).background];
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
    (void)runtime->dispatch({"file.open", runtime->revision(), std::string{"m.txt"}});
    (void)runtime->dispatch({"view.toggle_line_numbers", runtime->revision(), {}});
    // Add a second cursor on the line below: carets now on lines 1 and 2.
    (void)runtime->dispatch({"select.add_cursor_down", runtime->revision(), {}});

    auto snapshot = ssg::test::projectGridFrame(*runtime, ssg::ViewId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_EQ(snapshot->semantic().sections().selection.items().size(),
              std::size_t{2});
    ASSERT_TRUE(snapshot->document().has_value());
    if (!snapshot->document()) return;
    auto const& pane = *snapshot->document();
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
        (void)runtime->dispatch({"file.open", runtime->revision(), file});
        auto snapshot = ssg::test::projectGridFrame(*runtime, ssg::ViewId{1}, {80, 24});
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
    (void)runtime->dispatch({"file.open", runtime->revision(), std::string{"long.txt"}});

    ssg::ViewportDimensions const dims{40, 8};
    ssg::test::GridTestView gridView{ssg::ViewId{1}, dims};
    (void)gridView.present(*runtime);  // prime the pane cache
    (void)runtime->dispatch({"cursor.line_end", runtime->revision(), {}});
    auto snapshot = gridView.present(*runtime);
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_TRUE(snapshot->presentation().viewport.firstVisualColumn > 0);
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
    (void)runtime->dispatch({"file.open", runtime->revision(), std::string{"hello.txt"}});
    auto snapshot = ssg::test::projectGridFrame(*runtime, ssg::ViewId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;

    // Without a palette projection the document content is painted.
    ASSERT_TRUE(gridContains(
        ssg::Renderer{}.render(*snapshot), "alpha"));

    ssg::PaletteProjection projection;
    ASSERT_TRUE(snapshot->document().has_value());
    if (!snapshot->document()) return;
    projection.rect = snapshot->document()->content;
    projection.rows = {{"file.save", "ESC s"}, {"file.quit", "ESC q"}};
    projection.selected = std::uint32_t{1};
    projection.rect = {0, 0, 1, 1};
    auto sections = snapshot->semantic().sections();
    showPicker(sections);
    ssg::PaletteReport report;
    report.selected = projection.selected;
    for (auto const& row : projection.rows) {
        report.rows.push_back({"", row.label, row.detail});
    }
    auto frame = ssg::test::copyGridFrame(*snapshot, ssg::ViewId{1}, std::move(sections), snapshot->presentation(), std::move(report));
    const auto* viewport = frame.layout().find(
        ssg::UiNodeId{
            std::string{ssg::kFindResultsViewportNodeId}});
    ASSERT_TRUE(viewport != nullptr);
    if (!viewport) return;
    const auto solved = ssg::solvePaletteSurface(
        frame.palette(), viewport->rect,
        frame.presentation().style.dimensions.scrollbarGutterWidth);
    ASSERT_EQ(solved.visibleRows.size(), std::size_t{2});
    if (solved.visibleRows.size() < 2) return;
    auto grid = ssg::Renderer{}.render(frame);

    // Results replace the document text in the pane.
    ASSERT_TRUE(gridContains(grid, "file.save"));
    ASSERT_TRUE(gridContains(grid, "ESC q"));
    ASSERT_FALSE(gridContains(grid, "alpha"));

    // The selected row is painted with the selection role, including on the
    // label's glyph cells (not only trailing filler).
    const auto& selected = solved.visibleRows[1].rect;
    ASSERT_EQ(grid.at(selected.x, selected.y).text, std::string{"f"});
    ASSERT_EQ(grid.at(selected.x, selected.y).role,
              ssg::SemanticRole::Selection);
    // The unselected row must not carry the selection role.
    const auto& unselected = solved.visibleRows[0].rect;
    ASSERT_FALSE(grid.at(unselected.x, unselected.y).role ==
                 ssg::SemanticRole::Selection);
}

TEST(activePaletteWithoutSolvedFindResultsIsRejected) {
    const auto baseline =
        ssg::test::SessionSnapshotBuilder{}.viewport(80, 24).build();
    auto sections = baseline.sections();
    sections.palette.activePicker = ssg::PickerActivation{
        ssg::SearchMode::Command, ssg::PickerActivationId{1}};
    ASSERT_THROWS(
        ssg::test::copyGridFrame(baseline, ssg::ViewId{1}, std::move(sections), baseline.presentation()),
        std::logic_error);
}

TEST(renderShowsPaletteQueryAndGhostInHeader) {
    auto root = uniqueRoot();
    std::ofstream{root / "hello.txt"} << "alpha\n";
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    (void)runtime->dispatch({"file.open", runtime->revision(), std::string{"hello.txt"}});
    (void)runtime->dispatch({"palette.open", runtime->revision(), {}});

    ssg::PaletteReport report;
    report.query = "sa";
    report.ghost = "ve File";
    report.rows = {{"file.save", "Save File", ""}};
    report.selected = std::uint32_t{0};
    auto snapshot = ssg::test::projectGridFrame(*runtime, ssg::ViewId{1}, {80, 24}, report);
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
    (void)runtime->dispatch({"file.open", runtime->revision(), std::string{"sel.txt"}});

    // Baseline: no selection -> the document row has no selection-role cells.
    {
        auto snapshot = ssg::test::projectGridFrame(*runtime, ssg::ViewId{1}, {80, 24});
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
    (void)runtime->dispatch({"select.line_end", runtime->revision(), {}});
    auto snapshot = ssg::test::projectGridFrame(*runtime, ssg::ViewId{1}, {80, 24});
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
    (void)runtime->dispatch({"file.open", runtime->revision(), std::string{"ml.txt"}});
    // Anchor at line 0 col 0, extend down into line 1: the selection spans the
    // newline after "alpha", so alpha's end-of-line fills to the pane edge.
    ssg::test::GridTestView presenter{ssg::ViewId{1}, {80, 24}};
    (void)presenter.dispatch(
        *runtime, {"select.line_down", runtime->revision(), {}});
    auto snapshot = presenter.present(*runtime);
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
    (void)runtime->dispatch({"file.open", runtime->revision(), std::string{"w.txt"}});
    (void)runtime->dispatch({"select.all", runtime->revision(), {}});
    auto snapshot = ssg::test::projectGridFrame(*runtime, ssg::ViewId{1}, {80, 24});
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
    (void)runtime->dispatch({"file.open", runtime->revision(), std::string{"rc.txt"}});
    // Select the first word, then add the next occurrence: two RANGED selections,
    // each with an active caret. The secondary (non-primary) ranged selection's
    // caret must render as a caret cell even though it is not a bare caret.
    (void)runtime->dispatch({"select.word_right", runtime->revision(), {}});
    (void)runtime->dispatch({"select.add_next_occurrence", runtime->revision(), {}});
    auto snapshot = ssg::test::projectGridFrame(*runtime, ssg::ViewId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto const& items = snapshot->semantic().sections().selection.items();
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
    (void)runtime->dispatch({"file.open", runtime->revision(), std::string{"car.txt"}});

    // Two carets (primary + one below): the primary uses the hardware cursor,
    // the other renders as a caret-role cell.
    (void)runtime->dispatch({"select.add_cursor_down", runtime->revision(), {}});
    auto snapshot = ssg::test::projectGridFrame(*runtime, ssg::ViewId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_EQ(snapshot->semantic().sections().selection.items().size(),
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
    (void)runtime->dispatch({"file.open", runtime->revision(), std::string{"find.txt"}});
    (void)runtime->dispatch({"find.open", runtime->revision(), {}});
    (void)runtime->dispatch({"find.update_query", runtime->revision(), ssg::FindQueryArguments{"cat"}});
    auto snapshot = ssg::test::projectGridFrame(*runtime, ssg::ViewId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_EQ(snapshot->semantic().sections().findReplace.matches.size(),
              std::size_t{3});
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
    (void)runtime->dispatch({"file.open", runtime->revision(), std::string{"stale.txt"}});
    (void)runtime->dispatch({"find.open", runtime->revision(), {}});
    (void)runtime->dispatch({"find.update_query", runtime->revision(), ssg::FindQueryArguments{"cat"}});

    // Editing the document advances its revision without re-evaluating find, so
    // the controller is stale: reconcile closes it and no matches are painted.
    (void)runtime->dispatch({"text.insert", runtime->revision(), ssg::TextInputArguments{"z"}});
    auto snapshot = ssg::test::projectGridFrame(*runtime, ssg::ViewId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_FALSE(snapshot->semantic().sections().findReplace.open);
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
    (void)runtime->dispatch({"file.open", runtime->revision(), std::string{"rep.txt"}});
    (void)runtime->dispatch({"replace.open", runtime->revision(), {}});
    (void)runtime->dispatch({"find.update_query", runtime->revision(), ssg::FindQueryArguments{"cat"}});
    (void)runtime->dispatch({"replace.update_replacement", runtime->revision(), ssg::FindQueryArguments{"dog"}});
    auto snapshot = ssg::test::projectGridFrame(*runtime, ssg::ViewId{1}, {80, 24});
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
    (void)runtime->dispatch({"file.open", runtime->revision(), std::string{"opt.txt"}});
    (void)runtime->dispatch({"find.open", runtime->revision(), {}});
    (void)runtime->dispatch({"find.update_query", runtime->revision(), ssg::FindQueryArguments{"cat"}});

    // Default options: all three indicators render unchecked.
    {
        auto snapshot = ssg::test::projectGridFrame(*runtime, ssg::ViewId{1}, {80, 24});
        ASSERT_TRUE(snapshot.has_value());
        if (!snapshot) return;
        auto& frame = *snapshot;
        const auto* input = frame.layout().find(
            ssg::footerPromptControlNodeId("find.query"));
        const auto* toggle = frame.layout().find(
            ssg::footerPromptControlNodeId("find.toggle_case"));
        ASSERT_TRUE(input != nullptr);
        ASSERT_TRUE(toggle != nullptr);
        if (!input || !toggle) return;
        auto grid = ssg::Renderer{}.render(frame);
        ASSERT_EQ(grid.at(input->rect.x, input->rect.y).text,
                  std::string{"f"});
        ASSERT_EQ(grid.at(toggle->rect.x, toggle->rect.y).text,
                  std::string{"["});
        ASSERT_TRUE(gridContains(grid, "[ ] case"));
        ASSERT_TRUE(gridContains(grid, "[ ] word"));
        ASSERT_TRUE(gridContains(grid, "[ ] regex"));
        // The query input label is chrome and renders lowercase.
        ASSERT_TRUE(gridContains(grid, "find query"));
        ASSERT_FALSE(gridContains(grid, "Find query"));
    }

    // Toggling case flips its indicator to checked.
    (void)runtime->dispatch({"find.toggle_case", runtime->revision(), {}});
    auto snapshot = ssg::test::projectGridFrame(*runtime, ssg::ViewId{1}, {80, 24});
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
    (void)runtime->dispatch({"file.open", runtime->revision(), std::string{"p.txt"}});

    struct Case {
        const char* command;
        const char* lower;
        const char* title;
    };
    for (auto const& c : {Case{"settings.open", "settings query", "Settings query"},
                          Case{"goto.line", "line number", "Line number"},
                          Case{"file.open", "open file", "Open file"}}) {
        (void)runtime->dispatch({c.command, runtime->revision(), {}});
        auto snapshot = ssg::test::projectGridFrame(*runtime, ssg::ViewId{1}, {80, 24});
        ASSERT_TRUE(snapshot.has_value());
        if (!snapshot) continue;
        auto grid = ssg::Renderer{}.render(*snapshot);
        ASSERT_TRUE(gridContains(grid, c.lower));
        ASSERT_FALSE(gridContains(grid, c.title));
        (void)runtime->dispatch({"prompt.cancel", runtime->revision(), {}});
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
    (void)runtime->dispatch({"panel.toggle", runtime->revision(), {}});
    // Expand the workspace root, then drive the selection to the bottom.
    (void)runtime->dispatch({"tree.select_next", runtime->revision(), {}});
    (void)runtime->dispatch({"tree.activate", runtime->revision(), {}});
    ssg::test::GridTestView gridView{ssg::ViewId{1}, {80, 12}};
    // Prime the cached panel height (the command-path keep-visible reads it).
    (void)gridView.present(*runtime);
    for (int i = 0; i < 60; ++i) {
        (void)runtime->dispatch({"tree.select_next", runtime->revision(), {}});
    }
    auto snapshot = gridView.present(*runtime);
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_TRUE(snapshot->panel().has_value());
    if (!snapshot->panel() || !snapshot->panel()->scrollbarGutter) return;
    const auto& panel = *snapshot->panel();
    auto grid = ssg::Renderer{}.render(*snapshot);

    // A thumb (the default thumb glyph) is drawn in the reserved gutter column.
    int const gx = panel.scrollbarGutter->x;
    const int thumbY =
        panel.scrollbarGutter->y +
        static_cast<int>(panel.scrollbar.thumbStart);
    const int thumbBottom =
        thumbY + static_cast<int>(panel.scrollbar.thumbSize);
    bool hasThumb = false;
    for (int y = thumbY; y < thumbBottom; ++y) {
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
    (void)runtime->dispatch({"panel.toggle", runtime->revision(), {}});
    // Expand the root so its two files are visible; the tree still fits.
    (void)runtime->dispatch({"tree.select_next", runtime->revision(), {}});
    (void)runtime->dispatch({"tree.activate", runtime->revision(), {}});
    auto snapshot = ssg::test::projectGridFrame(*runtime, ssg::ViewId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_TRUE(snapshot->panel().has_value());
    if (!snapshot->panel() || !snapshot->panel()->scrollbarGutter) return;
    auto grid = ssg::Renderer{}.render(*snapshot);
    // The gutter is reserved (column exists) but blank: no thumb or track glyphs,
    // so the tree's content width never changes as items are added or removed.
    auto const gutter = *snapshot->panel()->scrollbarGutter;
    int const gx = gutter.x;
    for (int y = gutter.y; y < gutter.bottom(); ++y) {
        ASSERT_NE(grid.at(gx, y).text, std::string{"#"});
        ASSERT_NE(grid.at(gx, y).text, std::string{"|"});
    }
    ASSERT_TRUE(gridContains(grid, "a.txt"));
}

TEST(renderPanelUsesSolvedPanelGeometryAndWindow) {
    auto root = uniqueRoot();
    std::ofstream{root / "visible.txt"} << "x";
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    (void)runtime->dispatch({"panel.toggle", runtime->revision(), {}});
    (void)runtime->dispatch({"tree.select_next", runtime->revision(), {}});
    (void)runtime->dispatch({"tree.activate", runtime->revision(), {}});
    auto snapshot = ssg::test::projectGridFrame(*runtime, ssg::ViewId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto& frame = *snapshot;
    ASSERT_TRUE(frame.panel().has_value());
    if (!frame.panel()) return;

    auto grid = ssg::Renderer{}.render(frame);
    auto const& panel = *frame.panel();
    ASSERT_EQ(rowText(grid, panel.providerLabel.y).substr(
                  static_cast<std::size_t>(panel.providerLabel.x),
                  panel.providerText.size()),
              panel.providerText);
    ASSERT_TRUE(gridContains(grid, "visible.txt"));
}

TEST(renderPaletteWindowsRowsAndDrawsAThumbWithAbsoluteSelection) {
    auto root = uniqueRoot();
    std::ofstream{root / "hello.txt"} << "alpha\nbeta\n";
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    (void)runtime->dispatch({"file.open", runtime->revision(), std::string{"hello.txt"}});
    auto snapshot = ssg::test::projectGridFrame(*runtime, ssg::ViewId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_TRUE(snapshot->document().has_value());
    if (!snapshot->document()) return;

    // A 40-item ranked list windowed to rows [20, 20+height); the absolute
    // selection is 25, so the on-screen highlight is at window row 5.
    auto const& pane = *snapshot->document();
    std::uint32_t const rows = static_cast<std::uint32_t>(pane.content.height);
    ssg::PaletteProjection projection;
    projection.rect = pane.content;
    projection.scrollbarRect = pane.scrollbarGutter;
    projection.firstVisible = 20;
    projection.selected = std::uint32_t{25};
    projection.scrollbar = ssg::Viewport{}.scrollbarMetrics(40, rows, 20);
    for (std::uint32_t i = 0; i < rows; ++i) {
        projection.rows.push_back(
            {"cmd-" + std::to_string(20 + i), ""});
    }
    auto sections = snapshot->semantic().sections();
    showPicker(sections);
    auto frame = ssg::test::copyGridFrame(*snapshot, ssg::ViewId{1}, std::move(sections), snapshot->presentation(), paletteReport(projection));
    auto grid = ssg::Renderer{}.render(frame);

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
    (void)runtime->dispatch({"file.open", runtime->revision(), std::string{"hello.txt"}});
    auto snapshot = ssg::test::projectGridFrame(*runtime, ssg::ViewId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_TRUE(snapshot->document().has_value());
    if (!snapshot->document()) return;
    auto const& pane = *snapshot->document();
    ssg::PaletteProjection projection;
    projection.rect = pane.content;
    projection.scrollbarRect = pane.scrollbarGutter;
    projection.firstVisible = 0;
    projection.selected = std::uint32_t{0};
    projection.scrollbar =
        ssg::Viewport{}.scrollbarMetrics(2, static_cast<std::uint32_t>(pane.content.height), 0);
    projection.rows = {{"a", ""}, {"b", ""}};
    auto sections = snapshot->semantic().sections();
    showPicker(sections);
    auto frame = ssg::test::copyGridFrame(*snapshot, ssg::ViewId{1}, std::move(sections), snapshot->presentation(), paletteReport(projection));
    auto grid = ssg::Renderer{}.render(frame);
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
    ASSERT_TRUE(snapshot.layout().find(
                    ssg::UiNodeId{std::string{ssg::kRootNodeId}}) == nullptr);
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
    auto snapshot = ssg::test::projectGridFrame(*runtime, ssg::ViewId{1}, {24, 3});
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
    ASSERT_EQ(ssg::Renderer{}
                  .render(*snapshot)
                  .canonical(),
              grid.canonical());
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
    ASSERT_TRUE(runtime->dispatch({"palette.open", runtime->revision(), {}})
                    .accepted());

    // The client owns the query text and reports it through the palette report,
    // exactly as the app does.
    ssg::PaletteReport report;
    report.query = "save";
    auto snapshot = ssg::test::projectGridFrame(*runtime, ssg::ViewId{1}, {80, 24}, report);
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto grid = ssg::Renderer{}.render(*snapshot);

    ASSERT_TRUE(grid.caret.has_value());
    if (!grid.caret) return;
    // In the header row, not wherever painting stopped.
    ASSERT_EQ(grid.caret->row, std::uint32_t{0});

    // And exactly one cell past the last drawn character of "> save".
    const auto* query = snapshot->header() && snapshot->header()->input
                            ? &*snapshot->header()->input
                            : nullptr;
    ASSERT_TRUE(query != nullptr);
    if (query) {
        ASSERT_EQ(grid.caret->column,
                  static_cast<std::uint32_t>(
                      query->query.x + query->queryText.size()));
    }
}

// Display width, not byte count: a multi-byte query would otherwise place the
// caret several columns short of the text.
TEST(theInputLineCaretIsPlacedByDisplayWidthNotByteCount) {
    auto root = uniqueRoot();
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    ASSERT_TRUE(runtime->dispatch({"palette.open", runtime->revision(), {}})
                    .accepted());

    ssg::PaletteReport report;
    report.query = "\u00e9\u00e9\u00e9";  // 3 characters, 6 bytes, 3 columns.
    auto snapshot = ssg::test::projectGridFrame(*runtime, ssg::ViewId{1}, {80, 24}, report);
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto grid = ssg::Renderer{}.render(*snapshot);
    ASSERT_TRUE(grid.caret.has_value());
    if (!grid.caret) return;

    const auto* query = snapshot->header() && snapshot->header()->input
                            ? &*snapshot->header()->input
                            : nullptr;
    ASSERT_TRUE(query != nullptr);
    if (query) {
        // "> " plus three single-width characters = 5 columns, NOT 8 bytes.
        ASSERT_EQ(grid.caret->column,
                  static_cast<std::uint32_t>(query->query.x + 5));
    }
}

// With a scrolled query the caret must sit at the end of the VISIBLE text --
// still inside the header, still marking where the next keystroke lands.
TEST(theCaretFollowsAScrolledQueryToTheEndOfTheVisibleText) {
    auto root = uniqueRoot();
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    ASSERT_TRUE(runtime->dispatch({"palette.open", runtime->revision(), {}})
                    .accepted());

    ssg::PaletteReport report;
    report.query = std::string(300, 'x');
    auto snapshot = ssg::test::projectGridFrame(*runtime, ssg::ViewId{1}, {80, 24}, report);
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto grid = ssg::Renderer{}.render(*snapshot);

    ASSERT_TRUE(grid.caret.has_value());
    if (!grid.caret) return;
    ASSERT_EQ(grid.caret->row, std::uint32_t{0});
    // Inside the header, not run off the right edge by the untruncated query.
    ASSERT_TRUE(grid.caret->column < std::uint32_t{80});

    const auto* query = snapshot->header() && snapshot->header()->input
                            ? &*snapshot->header()->input
                            : nullptr;
    ASSERT_TRUE(query != nullptr);
    if (query) {
        // At the end of what is actually drawn.
        ASSERT_EQ(grid.caret->column,
                  static_cast<std::uint32_t>(
                      query->query.x + query->query.width));
    }
}

TEST(inputLineCaretUsesTheLoweredPromptInputGeometry) {
    const auto assertCaret = [](int width, std::string query) {
        auto snapshot = ssg::test::SessionSnapshotBuilder{}
                            .viewport(width, 8)
                            .promptInput(true, std::move(query))
                            .sections([](ssg::SessionSnapshotSections& sections) {
                                sections.uiTree.focusPath =
                                    std::vector<ssg::UiNodeId>{
                                        ssg::UiNodeId{std::string{
                                            ssg::kEditorNodeId}},
                                        ssg::UiNodeId{std::string{
                                            ssg::kHeaderPromptInputNodeId}}};
                            })
                            .build();
        ASSERT_TRUE(snapshot.header().has_value());
        const auto* input = snapshot.header() && snapshot.header()->input
                                ? &*snapshot.header()->input
                                : nullptr;
        ASSERT_TRUE(input != nullptr);

        auto grid = ssg::Renderer{}.render(snapshot);
        ASSERT_TRUE(grid.caret.has_value());
        if (!input || !grid.caret) return;
        ASSERT_EQ(grid.caret->row, input->caret.y);
        ASSERT_EQ(grid.caret->column, input->caret.x);
        ASSERT_EQ(grid.at(input->query.x, input->query.y).text,
                  input->queryText.substr(0, 1));
    };

    assertCaret(80, "save");
    assertCaret(30, std::string(200, 'x'));
}

TEST(emptyEditorMessageUsesSolvedDocumentGeometryWithoutLegacyNode) {
    auto frame =
        ssg::test::SessionSnapshotBuilder{}
            .viewport(40, 8)
            .revision(ssg::Revision{0})
            .build();
    ASSERT_TRUE(frame.document().has_value());
    if (!frame.document()) return;
    const auto grid = ssg::Renderer{}.render(frame);
    ASSERT_EQ(grid.at(frame.document()->content.x,
                      frame.document()->content.y)
                  .text,
              std::string{"e"});
    ASSERT_TRUE(gridContains(grid, "empty editor"));
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
    (void)runtime->dispatch({"panel.toggle", runtime->revision(), {}});
    (void)runtime->dispatch({"tree.select_next", runtime->revision(), {}});
    (void)runtime->dispatch({"tree.activate", runtime->revision(), {}});
    auto snapshot = ssg::test::projectGridFrame(*runtime, ssg::ViewId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_TRUE(snapshot->panel().has_value());
    if (!snapshot->panel() || !snapshot->panel()->scrollbarGutter) return;

    ssg::Style style;
    style.scrollbar.track = ":";
    style.scrollbar.single = "@";
    style.scrollbar.top = "@";
    style.scrollbar.body = "@";
    style.scrollbar.bottom = "@";
    style.tree.collapsed = "+ ";
    style.tree.expanded = "- ";
    style.cwdPrefix = "cwd: ";
    auto const grid = ssg::Renderer{}.render(withStyle(*snapshot, style));

    auto const gutter = *snapshot->panel()->scrollbarGutter;
    int const gx = gutter.x;
    bool restyledThumb = false;
    bool restyledTrack = false;
    for (int y = gutter.y; y < gutter.bottom(); ++y) {
        auto const& text = grid.at(gx, y).text;
        if (text == "@") restyledThumb = true;
        if (text == ":") restyledTrack = true;
        // The shipped glyphs must be gone entirely, not merely joined.
        ASSERT_NE(text, std::string{"#"});
        ASSERT_NE(text, std::string{"|"});
    }
    ASSERT_TRUE(restyledThumb);
    ASSERT_TRUE(restyledTrack);
    ASSERT_TRUE(gridContains(grid, "cwd: "));

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
    (void)runtime->dispatch({"panel.toggle", runtime->revision(), {}});
    (void)runtime->dispatch({"tree.select_next", runtime->revision(), {}});
    (void)runtime->dispatch({"tree.activate", runtime->revision(), {}});

    ssg::StyleDefineArguments args;
    args.values = {{"scrollbar_track", ":"},
                   {"scrollbar_single", "@"},
                   {"scrollbar_top", "@"},
                   {"scrollbar_body", "@"},
                   {"scrollbar_bottom", "@"},
                   {"tree_collapsed", "+ "},
                   {"tree_expanded", "- "},
                   {"input_line_sigil", "! "}};
    auto const applied =
        runtime->dispatch({"style.define", runtime->revision(), args});
    ASSERT_TRUE(applied.accepted());

    auto snapshot = ssg::test::projectGridFrame(*runtime, ssg::ViewId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    // The published section carries the new glyphs.
    ASSERT_EQ(snapshot->presentation().style.scrollbar.track, std::string{":"});
    ASSERT_EQ(snapshot->presentation().style.tree.expanded, std::string{"- "});
    ASSERT_EQ(snapshot->presentation().style.inputLineSigil, std::string{"! "});
    const ssg::UiNode* inputLine = nullptr;
    if (const auto* root = std::get_if<ssg::UiContainer>(
            &snapshot->semantic().sections().uiTree.root.content)) {
        for (const auto& area : root->children) {
            if (area.id.value() != ssg::kHeaderNodeId) continue;
            if (const auto* header =
                    std::get_if<ssg::UiContainer>(&area.content)) {
                for (const auto& child : header->children) {
                    if (child.id.value() == ssg::kHeaderPromptInputNodeId) {
                        inputLine = &child;
                    }
                }
            }
        }
    }
    ASSERT_TRUE(inputLine != nullptr);
    if (inputLine) {
        const auto* leaf = std::get_if<ssg::UiLeaf>(&inputLine->content);
        ASSERT_TRUE(leaf != nullptr);
        if (leaf) ASSERT_EQ(leaf->widget.sigil, std::string{"! "});
    }

    // And the rendered screen shows them, with the shipped glyphs gone.
    auto const grid = ssg::Renderer{}.render(*snapshot);
    ASSERT_TRUE(snapshot->panel().has_value());
    if (!snapshot->panel() || !snapshot->panel()->scrollbarGutter) return;
    auto const gutter = *snapshot->panel()->scrollbarGutter;
    int const gx = gutter.x;
    bool restyled = false;
    for (int y = gutter.y; y < gutter.bottom(); ++y) {
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
        runtime->dispatch({"style.define", runtime->revision(), args});
    ASSERT_FALSE(rejected.accepted());

    auto snapshot = ssg::test::projectGridFrame(*runtime, ssg::ViewId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    // The good key in the rejected table did NOT leak into the live style.
    ASSERT_EQ(snapshot->presentation().style.tree.expanded, ssg::Style{}.tree.expanded);
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

TEST(noticeAndExternalRowsPaintTheirPublishedRolesAtTheirRects) {
    auto snapshot =
        ssg::test::SessionSnapshotBuilder{}
            .document("content\n")
            .viewport(80, 16)
            .noticePresent()
            .externalModificationPresent()
            .sections([](ssg::SessionSnapshotSections& sections) {
                sections.noticeView = ssg::NoticeView{
                    "Draft conflict",
                    {{"diff", "Diff", "draft.diff"}}};
                sections.externalModification = {
                    ssg::Revision{1},
                    "Files changed on disk",
                    {{ssg::DiffFileId{"a"}, "a.txt",
                      ssg::ExternalDocumentStatus::ExternallyModified,
                      "modified", "M",
                      {ssg::externalActionAffordance(
                          ssg::ExternalAction::Reload)}},
                     {ssg::DiffFileId{"b"}, "b.txt",
                      ssg::ExternalDocumentStatus::ExternallyModified,
                      "modified", "M",
                      {ssg::externalActionAffordance(
                          ssg::ExternalAction::Reload)}}},
                    ssg::DiffFileId{"b"}};
            })
            .build();
    const auto* solvedNotice = snapshot.layout().find(
        ssg::UiNodeId{std::string{ssg::kNoticeNodeId}});
    const auto* solvedExternal = snapshot.layout().find(
        ssg::UiNodeId{std::string{ssg::kExternalModNodeId}});
    ASSERT_TRUE(solvedNotice != nullptr);
    ASSERT_TRUE(solvedExternal != nullptr);
    if (!solvedNotice || !solvedExternal) {
        return;
    }
    const auto noticeSurface =
        ssg::solveNoticeSurface(*snapshot.sections().noticeView,
                                solvedNotice->rect);
    ASSERT_TRUE(!noticeSurface.actions.empty());
    if (noticeSurface.actions.empty()) return;
    const auto externalSurface = ssg::solveExternalModificationSurface(
        snapshot.sections().externalModification, solvedExternal->rect);
    ASSERT_EQ(externalSurface.rows.size(), std::size_t{2});
    if (externalSurface.rows.size() < 2) return;

    auto const grid = ssg::Renderer{}.render(snapshot);
    ASSERT_EQ(grid.at(solvedNotice->rect.x, solvedNotice->rect.y).text,
              std::string{"D"});
    ASSERT_EQ(grid.at(solvedNotice->rect.x, solvedNotice->rect.y).role,
              ssg::SemanticRole::StatusWarning);
    ASSERT_EQ(grid.at(noticeSurface.actions.front().rect.x,
                      noticeSurface.actions.front().rect.y).text,
              std::string{"["});
    ASSERT_EQ(grid.at(noticeSurface.actions.front().rect.x,
                      noticeSurface.actions.front().rect.y).role,
              ssg::SemanticRole::StatusWarning);
    ASSERT_EQ(grid.at(externalSurface.rows[0].rect.x,
                      externalSurface.rows[0].rect.y).role,
              ssg::SemanticRole::StatusWarning);
    ASSERT_EQ(grid.at(externalSurface.rows[1].rect.x,
                      externalSurface.rows[1].rect.y).role,
              ssg::SemanticRole::Selection);
}

TEST(tabRenderingUsesSemanticTabsAndSolvedGeometry) {
    auto frame =
        ssg::test::SessionSnapshotBuilder{}
            .viewport(50, 10)
            .tabs({{"alpha.txt", "Alpha", true, false},
                   {"beta.txt", "Beta", false, false}})
            .build();
    const auto* node = frame.layout().find(
        ssg::UiNodeId{std::string{ssg::kTabBarNodeId}});
    ASSERT_TRUE(node != nullptr);
    if (!node) return;
    const auto solved = ssg::solveTabBar(
        frame.sections().tabs, frame.presentation().style.tab, node->rect);
    ASSERT_EQ(solved.tabs.size(), std::size_t{2});
    if (solved.tabs.size() < 2) return;
    const auto grid = ssg::Renderer{}.render(frame);
    ASSERT_EQ(grid.at(solved.tabs[0].rect.x,
                      solved.tabs[0].rect.y).text,
              std::string{"a"});
    ASSERT_EQ(grid.at(solved.tabs[0].rect.x,
                      solved.tabs[0].rect.y).role,
              ssg::SemanticRole::TabActive);
    ASSERT_EQ(grid.at(solved.tabs[1].rect.x,
                      solved.tabs[1].rect.y).text,
              std::string{"b"});
    ASSERT_EQ(grid.at(solved.tabs[1].rect.x,
                      solved.tabs[1].rect.y).role,
              ssg::SemanticRole::TabInactive);
}

TEST(externalIntrinsicShrinksToPreserveAnEditorRow) {
    auto snapshot =
        ssg::test::SessionSnapshotBuilder{}
            .document("content\n")
            .viewport(40, 6)
            .externalModificationPresent()
            .sections([](ssg::SessionSnapshotSections& sections) {
                sections.externalModification.revision = ssg::Revision{1};
                sections.externalModification.message =
                    "Files changed on disk";
                for (int index = 0; index < 6; ++index) {
                    ssg::ExternalDocumentView file{
                        ssg::DiffFileId{"file-" + std::to_string(index)}};
                    file.path =
                        "file-" + std::to_string(index) + ".txt";
                    file.statusLabel = "M";
                    file.actions.push_back(
                        ssg::externalActionAffordance(
                            ssg::ExternalAction::Reload));
                    sections.externalModification.files.push_back(
                        std::move(file));
                }
                sections.externalModification.selected =
                    ssg::DiffFileId{"file-4"};
            })
            .build();
    const auto* external = snapshot.layout().find(
        ssg::UiNodeId{std::string{ssg::kExternalModNodeId}});
    const auto* footer = snapshot.layout().find(
        ssg::UiNodeId{std::string{ssg::kFooterNodeId}});
    ASSERT_TRUE(external != nullptr);
    ASSERT_TRUE(footer != nullptr);
    if (!external || !footer) return;
    ASSERT_TRUE(external->rect.height <
                ssg::measureExternalModificationSurface(
                    snapshot.sections().externalModification)
                    .rows);
    ASSERT_TRUE(external->rect.bottom() < footer->rect.y);
    const auto grid = ssg::Renderer{}.render(snapshot);
    ASSERT_EQ(grid.at(external->rect.x, external->rect.y).text,
              std::string{"F"});
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
        std::vector<ssg::StatusFieldCatalogEntry> catalog{
            {"cwd", "Working directory", ssg::StatusFieldRegion::Header, 0},
            {"encoding", "Encoding", ssg::StatusFieldRegion::Footer, 0},
        };
        ssg::UiSchema schema;
        schema.root = ssg::assembleWholeScreen(
                          catalog, "help.open", ssg::StyleDimensions{},
                          ssg::Style{}.inputLineSigil)
                          .root;
        auto validation = ssg::validateUiSchema(schema);
        ASSERT_TRUE(validation.ok());
        return ssg::test::SessionSnapshotBuilder{}
            .document(document)
            .viewport(120, 40)
            .panel(true)
            .panelFocused(false)
            .schema(std::move(schema))
            .status(ssg::StatusViewState{{ssg::StatusItemView{
                ssg::StatusId{1}, ssg::StatusPriority::Information, 1,
                "Status message",
                {ssg::UiAction{"footer.act", "Save", "footer.act"}}}}, 0})
            .tabs({{"a.txt", "Tab a.txt", true, false},
                   {"b.txt", "Tab b.txt", false, false}})
            .noticePresent()
            .widgetProviderResolver(
                [](std::string_view id)
                    -> std::optional<ssg::ResolvedProvider> {
                if (id == "cwd") {
                    return ssg::ResolvedProvider{
                        "~/project", "Working directory", std::nullopt};
                }
                if (id == "encoding") {
                    return ssg::ResolvedProvider{"UTF-8", "Encoding",
                                                 std::nullopt};
                }
                if (id == "footer.hint") {
                    return ssg::ResolvedProvider{
                        "help", "help", std::string{"help.open"}};
                }
                return std::nullopt;
            })
            .promptInput(pickerOpen, "needle", "ghost")
            .paletteReport(ssg::PaletteReport{
                "", "", {{"candidate", "Candidate", ""}},
                std::uint32_t{0}, 0, {}})
            .sections([&](ssg::SessionSnapshotSections& sections) {
                sections.theme = theme;
                sections.settings.entries.back() = {
                    ssg::SettingKey::LineNumbers,
                    {true, ssg::SettingScope::Workspace}};
                sections.noticeView = ssg::NoticeView{
                    "Draft conflict",
                    {{"diff", "Diff", "draft.diff"}}};
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
                sections.tree.activeBinding = ssg::TreeProviderBinding{
                    provider.providerId, provider.kind};
                // A ranged selection on line 0 paints real Selection-role
                // cells, so Selection is proven consumed at the cell level, not
                // only via grid.selectionFill.
                sections.selection = ssg::SelectionSet{{ssg::Selection{
                        {ssg::ByteOffset{0}, ssg::LineIndex{0},
                         ssg::CellIndex{0}},
                        {ssg::ByteOffset{4}, ssg::LineIndex{0},
                         ssg::CellIndex{4}}}}};
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
    auto documentGrid = ssg::Renderer{}.render(makeSnapshot(false));

    // Cell-level evidence that the selection and diff surfaces actually paint,
    // so treating Selection/Diff* colors (carried on grid.selectionFill/
    // diffTints) as consumed is not vacuous: a regression that stopped painting
    // selected or tinted cells fails here regardless of the grid-level copies.
    bool anySelectionCell = false;
    bool anyTintedCell = false;
    for (auto const& cell : grid.cells) {
        if (cell.role == SemanticRole::Selection) anySelectionCell = true;
    }
    for (auto const& cell : documentGrid.cells) {
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

// Lever 2 (visible-line cache): render re-shapes the visible document lines each
// frame. With a borrowed LineLayoutCache a cached render segments strictly fewer
// lines than an uncached one (the document lines are reused; only the uncached
// chrome shaping remains), and the two grids are byte-identical.
TEST(cachedRenderReusesDocumentLineShapingAndMatchesUncached) {
    auto root = uniqueRoot();
    std::string doc;
    for (int i = 0; i < 30; ++i) doc += "content line " + std::to_string(i) + "\n";
    std::ofstream{root / "doc.txt"} << doc;
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    (void)runtime->dispatch({"file.open", runtime->revision(), std::string{"doc.txt"}});
    auto snapshot = ssg::test::projectGridFrame(*runtime, ssg::ViewId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;

    ssg::LineLayoutCache cache;
    auto warm = ssg::Renderer{}.render(*snapshot, &cache);  // warm
    ssg::GraphemeLayout::resetCellRunCalls();
    auto cached =
        ssg::Renderer{}.render(*snapshot, &cache);
    auto const cachedCalls = ssg::GraphemeLayout::cellRunCalls();
    ssg::GraphemeLayout::resetCellRunCalls();
    auto uncached = ssg::Renderer{}.render(*snapshot);
    auto const uncachedCalls = ssg::GraphemeLayout::cellRunCalls();

    // The cache reused the visible document lines: strictly fewer segmentations.
    ASSERT_TRUE(cachedCalls < uncachedCalls);
    // Byte-identical output whether or not the cache served the lines.
    ASSERT_TRUE(cached.canonical() == uncached.canonical());
    ASSERT_TRUE(warm.canonical() == uncached.canonical());
    fs::remove_all(root);
}

SSG_TEST_SUITE(test_render) {
    RUN(everyNonCaretSemanticRoleIsColorConsumedByTheRenderer);
    RUN(chromeBackgroundsAreDistinctShadesAndTheActiveTabMergesWithTheDocument);
    RUN(headerAndFooterCellsAndHitsUseTheSolvedTree);
    RUN(rendererGetsRegionBackgroundsFromTheUiTree);
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
    RUN(renderPanelUsesSolvedPanelGeometryAndWindow);
    RUN(renderPaletteWindowsRowsAndDrawsAThumbWithAbsoluteSelection);
    RUN(renderPaletteReservesAnEmptyGutterWhenTheListFits);
    RUN(renderTooSmallViewportProducesLibraryPlaceholder);
    RUN(renderTooSmallMatchesHandAuthoredGolden);
    RUN(renderTooSmallIsSafeAtOneByOne);
    RUN(anOpenPickerPutsTheCaretAtTheEndOfTheTypedQuery);
    RUN(theInputLineCaretIsPlacedByDisplayWidthNotByteCount);
    RUN(theCaretFollowsAScrolledQueryToTheEndOfTheVisibleText);
    RUN(inputLineCaretUsesTheLoweredPromptInputGeometry);
    RUN(emptyEditorMessageUsesSolvedDocumentGeometryWithoutLegacyNode);
    RUN(theRendererDrawsChromeFromTheSnapshotStyleNotFromLiterals);
    RUN(theDocumentReplacementGlyphComesFromStyle);
    RUN(styleDefineRestylesTheLiveSessionChrome);
    RUN(urlsInTheDocumentBecomeClickableRuns);
    RUN(urlDetectionStopsAtSentenceAndBracketBoundaries);
    RUN(tabRenderingUsesSemanticTabsAndSolvedGeometry);
    RUN(externalIntrinsicShrinksToPreserveAnEditorRow);
    RUN(noticeAndExternalRowsPaintTheirPublishedRolesAtTheirRects);
    RUN(lspDiagnosticsUnderlineExactlyTheirRange);
    RUN(staleDiagnosticsAreNotPainted);
    RUN(styleDefineRejectionLeavesTheLiveStyleUnchanged);
    RUN(cachedRenderReusesDocumentLineShapingAndMatchesUncached);

    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed > 0 ? 1 : 0;
}
