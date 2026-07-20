#include <ssg/hit_test.h>

#include <ssg/editor_runtime.h>
#include <ssg/selection.h>
#include <ssg/session_snapshot.h>

#include "test_helpers.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace {

namespace fs = std::filesystem;

fs::path uniqueRoot() {
    auto root = fs::current_path() / "hit_test_root";
    fs::remove_all(root);
    fs::create_directories(root / "scratch");
    fs::create_directories(root / "recovery");
    return root;
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

// ---------------------------------------------------------------------------

TEST(editorCellMapsToItsDocumentByteOffset) {
    auto root = uniqueRoot();
    std::string const text = "alpha\nbeta\ngamma\n";
    std::ofstream{root / "doc.txt"} << text;
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    (void)runtime->dispatch(ssg::ClientId{1},
                            {"file.open", runtime->revision(), std::string{"doc.txt"}});
    auto snapshot = runtime->snapshot(ssg::ClientId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto const& shell = snapshot->sections().shell;
    ASSERT_FALSE(shell.panes.empty());
    if (shell.panes.empty()) return;
    auto const content = shell.panes.front().content;
    auto const& targets = snapshot->client().viewport.hitTargets;
    ASSERT_FALSE(targets.empty());
    if (targets.empty()) return;

    // Strong oracle: every hit target's byte offset must be DOCUMENT-absolute,
    // not line-relative. Cross-check each against the independent line model
    // (resolve_document_position / TextModel), which the app uses to turn a hit
    // into a caret. A regression to line-relative offsets makes every line's
    // cells resolve to line 0 and fails here immediately.
    bool sawSecondLine = false;
    for (auto const& target : targets) {
        int const column = content.x + static_cast<int>(target.viewportColumn);
        int const row = content.y + static_cast<int>(target.viewportRow);
        auto hit = ssg::hitTest(*snapshot, column, row);
        ASSERT_EQ(hit.region, ssg::HitRegion::Editor);
        ASSERT_EQ(hit.byteOffset, target.byteOffset);
        auto position =
            ssg::resolveDocumentPosition(text, ssg::ByteOffset{hit.byteOffset});
        ASSERT_TRUE(position.has_value());
        if (position) {
            ASSERT_EQ(position->line.value(),
                      static_cast<std::uint64_t>(target.logicalLine));
        }
        if (target.logicalLine > 0) sawSecondLine = true;
    }
    // The document has three lines, so the targets must reach past line 0 (the
    // property above is only meaningful if we actually exercised later lines).
    ASSERT_TRUE(sawSecondLine);

    // Column 0 of a later visual row resolves to that line's first byte.
    ssg::CellHitTarget const* lineOneStart = nullptr;
    for (auto const& target : targets) {
        if (target.logicalLine == 1 && target.viewportColumn == 0) {
            lineOneStart = &target;
            break;
        }
    }
    ASSERT_TRUE(lineOneStart != nullptr);
    if (lineOneStart) {
        ASSERT_EQ(lineOneStart->byteOffset, std::uint32_t{6});  // after "alpha\n"
    }

    // A cell far past the end of the short first line ("alpha", 5 cells) now
    // clamps to that line's end (M8 click-past-EOL): an editor hit at the newline
    // byte after "alpha" (offset 5), zero-width.
    auto pastEol = ssg::hitTest(*snapshot, content.right() - 2, content.y);
    ASSERT_EQ(pastEol.region, ssg::HitRegion::Editor);
    ASSERT_EQ(pastEol.byteOffset, std::uint32_t{5});
    ASSERT_EQ(pastEol.byteLen, std::uint32_t{0});
    {
        auto position = ssg::resolveDocumentPosition(
            text, ssg::ByteOffset{pastEol.byteOffset});
        ASSERT_TRUE(position.has_value());
        if (position) ASSERT_EQ(position->line.value(), std::uint64_t{0});
    }
}

TEST(clickPastEolBlankLineAndBelowDocumentClampToLineEnd) {
    // M8 click-past-EOL: a document with a blank (newline-only) line and short
    // lines. Clicking past content, on the blank line, and below the last line all
    // place the caret at the appropriate line end.
    auto root = uniqueRoot();
    //             offsets: a=0 b=1 \n=2 | (blank) \n=3 | c=4 d=5 e=6 \n=7
    std::string const text = "ab\n\ncde\n";
    std::ofstream{root / "doc.txt"} << text;
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    (void)runtime->dispatch(ssg::ClientId{1},
                            {"file.open", runtime->revision(), std::string{"doc.txt"}});
    auto snapshot = runtime->snapshot(ssg::ClientId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto const& shell = snapshot->sections().shell;
    ASSERT_FALSE(shell.panes.empty());
    if (shell.panes.empty()) return;
    auto const content = shell.panes.front().content;

    auto resolveLine = [&](std::uint32_t offset) -> std::uint64_t {
        auto p = ssg::resolveDocumentPosition(text, ssg::ByteOffset{offset});
        return p ? p->line.value() : 9999;
    };

    // Exact cell unchanged: 'a' at row 0 col 0 -> offset 0.
    auto exact = ssg::hitTest(*snapshot, content.x, content.y);
    ASSERT_EQ(exact.region, ssg::HitRegion::Editor);
    ASSERT_EQ(exact.byteOffset, std::uint32_t{0});
    ASSERT_TRUE(exact.byteLen > 0);

    // Past the end of line 0 ("ab") -> the newline at offset 2, on line 0.
    auto past0 = ssg::hitTest(*snapshot, content.x + 30, content.y);
    ASSERT_EQ(past0.region, ssg::HitRegion::Editor);
    ASSERT_EQ(past0.byteOffset, std::uint32_t{2});
    ASSERT_EQ(past0.byteLen, std::uint32_t{0});
    ASSERT_EQ(resolveLine(past0.byteOffset), std::uint64_t{0});

    // The blank line (row 1) — anywhere on it, including column 0 — resolves to the
    // blank line's own offset (3), on line 1. A blank row has no hit targets, so
    // this is purely the clamp.
    auto blank = ssg::hitTest(*snapshot, content.x + 5, content.y + 1);
    ASSERT_EQ(blank.region, ssg::HitRegion::Editor);
    ASSERT_EQ(blank.byteOffset, std::uint32_t{3});
    ASSERT_EQ(blank.byteLen, std::uint32_t{0});
    ASSERT_EQ(resolveLine(blank.byteOffset), std::uint64_t{1});

    // A row BELOW the last line (Decision B) clamps to the LAST visible row's end.
    // The document's last visual row is the trailing empty line (offset 8 == the
    // text end after "cde\n").
    auto const lastRowEnd =
        snapshot->client().viewport.visibleRows.back().endByteOffset;
    auto below = ssg::hitTest(*snapshot, content.x + 10, content.bottom() - 1);
    ASSERT_EQ(below.region, ssg::HitRegion::Editor);
    ASSERT_EQ(below.byteOffset, lastRowEnd);
    ASSERT_EQ(below.byteLen, std::uint32_t{0});
    auto belowPos =
        ssg::resolveDocumentPosition(text, ssg::ByteOffset{below.byteOffset});
    ASSERT_TRUE(belowPos.has_value());
}

TEST(clickPastEolIntegrationLandsCaretAtLineEnd) {
    // CE-2 (reproducible headless integration): a click past a line's content, on
    // a blank line, and below the document flows hit_test -> resolve_document_
    // position -> cursor.set_position and lands the caret at the row's end.
    auto root = uniqueRoot();
    std::string const text = "ab\n\ncde\n";  // ends: line0=2, blank=3, line2=7, tail=8
    std::ofstream{root / "doc.txt"} << text;
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    (void)runtime->dispatch(ssg::ClientId{1},
                            {"file.open", runtime->revision(), std::string{"doc.txt"}});

    auto caretOffsetAfterClick = [&](int column, int row) -> std::uint64_t {
        auto snapshot = runtime->snapshot(ssg::ClientId{1}, {80, 24});
        if (!snapshot) return 9999;
        auto const content = snapshot->sections().shell.panes.front().content;
        auto hit = ssg::hitTest(*snapshot, column, row);
        if (hit.region != ssg::HitRegion::Editor) return 9999;
        auto pos = ssg::resolveDocumentPosition(text, ssg::ByteOffset{hit.byteOffset});
        if (!pos) return 9999;
        (void)runtime->dispatch(
            ssg::ClientId{1},
            {"cursor.set_position", runtime->revision(),
             ssg::SelectionCommandArguments{pos, std::nullopt}});
        auto after = runtime->snapshot(ssg::ClientId{1}, {80, 24});
        if (!after) return 9999;
        return after->sections()
            .selection.selections.primary()
            .active.byteOffset.value();
    };

    auto snapshot = runtime->snapshot(ssg::ClientId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto const content = snapshot->sections().shell.panes.front().content;

    // Click far right of line 0 ("ab") -> caret at its end (offset 2).
    ASSERT_EQ(caretOffsetAfterClick(content.x + 40, content.y), std::uint64_t{2});
    // Click on the blank line -> caret on the blank line (offset 3).
    ASSERT_EQ(caretOffsetAfterClick(content.x + 5, content.y + 1), std::uint64_t{3});
    // Click below the last line -> caret at the last visual row's end (offset 8).
    ASSERT_EQ(caretOffsetAfterClick(content.x + 10, content.bottom() - 1),
              std::uint64_t{8});
}

TEST(panelRowMapsToItsTreeNodeId) {
    auto root = uniqueRoot();
    for (int i = 0; i < 6; ++i) {
        char name[32];
        std::snprintf(name, sizeof name, "file-%02d.txt", i);
        std::ofstream{root / name} << "x";
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
    ASSERT_TRUE(shell.panel.has_value());
    if (!shell.panel) return;
    auto const& provider = snapshot->sections().tree.providers.front();
    ASSERT_FALSE(provider.visibleNodeIds.empty());
    if (provider.visibleNodeIds.empty()) return;

    // The provider-label row (panel.y) is not a node.
    auto label = ssg::hitTest(*snapshot, shell.panel->x, shell.panel->y);
    ASSERT_EQ(label.region, ssg::HitRegion::None);

    // The first content row maps to the first visible node id.
    auto hit = ssg::hitTest(*snapshot, shell.panel->x, shell.panel->y + 1);
    ASSERT_EQ(hit.region, ssg::HitRegion::Panel);
    ASSERT_TRUE(hit.nodeId.has_value());
    if (hit.nodeId) ASSERT_EQ(*hit.nodeId, provider.visibleNodeIds.front());

    // A row below the last visible node is empty.
    auto empty = ssg::hitTest(*snapshot, shell.panel->x, shell.panel->bottom() - 1);
    ASSERT_EQ(empty.region, ssg::HitRegion::None);
}

TEST(paletteRowMapsToItsAbsoluteRankIndex) {
    auto root = uniqueRoot();
    std::ofstream{root / "doc.txt"} << "alpha\n";
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    (void)runtime->dispatch(ssg::ClientId{1},
                            {"file.open", runtime->revision(), std::string{"doc.txt"}});
    auto snapshot = runtime->snapshot(ssg::ClientId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto sections = snapshot->sections();
    ASSERT_FALSE(sections.shell.panes.empty());
    if (sections.shell.panes.empty()) return;
    auto const& pane = sections.shell.panes.front();

    // A 40-item ranked list windowed to [20, 20+h): the on-screen row 3 is the
    // absolute candidate 23.
    std::uint32_t const rows = static_cast<std::uint32_t>(pane.content.height);
    ssg::PaletteProjection projection;
    projection.rect = pane.content;
    projection.scrollbarRect = pane.scrollbar;
    projection.firstVisible = 20;
    projection.selected = std::uint32_t{25};
    projection.scrollbar = ssg::scrollbarMetrics(40, rows, 20);
    for (std::uint32_t i = 0; i < rows; ++i) {
        projection.rows.push_back({"cmd-" + std::to_string(20 + i), ""});
    }
    sections.shell.palette = projection;
    ssg::SessionSnapshot projected{snapshot->revision(), snapshot->topology(),
                                   snapshot->client(), std::move(sections)};

    auto hit = ssg::hitTest(projected, pane.content.x, pane.content.y + 3);
    ASSERT_EQ(hit.region, ssg::HitRegion::Palette);
    ASSERT_EQ(hit.itemIndex, std::uint32_t{23});

    // The palette overlays the pane: a document cell is inert while it is open.
    auto overDoc = ssg::hitTest(projected, pane.content.x, pane.content.y);
    ASSERT_EQ(overDoc.region, ssg::HitRegion::Palette);
    ASSERT_EQ(overDoc.itemIndex, std::uint32_t{20});
}

TEST(paletteScrollbarAndEmptyAreaClassifyCorrectly) {
    auto root = uniqueRoot();
    std::ofstream{root / "doc.txt"} << "alpha\n";
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    (void)runtime->dispatch(ssg::ClientId{1},
                            {"file.open", runtime->revision(), std::string{"doc.txt"}});
    auto snapshot = runtime->snapshot(ssg::ClientId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto sections = snapshot->sections();
    auto const& pane = sections.shell.panes.front();
    std::uint32_t const rows = static_cast<std::uint32_t>(pane.content.height);

    ssg::PaletteProjection projection;
    projection.rect = pane.content;
    projection.scrollbarRect = pane.scrollbar;
    projection.firstVisible = 0;
    projection.selected = std::uint32_t{0};
    projection.scrollbar = ssg::scrollbarMetrics(100, rows, 0);
    for (std::uint32_t i = 0; i < rows; ++i) {  // exactly fills the window
        projection.rows.push_back({"cmd-" + std::to_string(i), ""});
    }
    sections.shell.palette = projection;
    ssg::SessionSnapshot projected{snapshot->revision(), snapshot->topology(),
                                   snapshot->client(), std::move(sections)};

    // Top of the palette gutter -> fraction 0; bottom -> fraction ~1.
    auto top = ssg::hitTest(projected, pane.scrollbar.x, pane.scrollbar.y);
    ASSERT_EQ(top.region, ssg::HitRegion::PaletteScrollbar);
    ASSERT_EQ(top.scrollNumerator, std::uint32_t{0});
    auto bottom = ssg::hitTest(projected, pane.scrollbar.x, pane.scrollbar.bottom() - 1);
    ASSERT_EQ(bottom.region, ssg::HitRegion::PaletteScrollbar);
    ASSERT_EQ(bottom.scrollNumerator, bottom.scrollDenominator);
}

TEST(editorScrollbarFractionFeedsScrollToFraction) {
    auto root = uniqueRoot();
    std::string text;
    for (int i = 0; i < 100; ++i) text += "line " + std::to_string(i) + "\n";
    std::ofstream{root / "tall.txt"} << text;
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    (void)runtime->dispatch(ssg::ClientId{1},
                            {"file.open", runtime->revision(), std::string{"tall.txt"}});
    auto snapshot = runtime->snapshot(ssg::ClientId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto const& shell = snapshot->sections().shell;
    ASSERT_FALSE(shell.panes.empty());
    if (shell.panes.empty()) return;
    auto const gutter = shell.panes.front().scrollbar;
    ASSERT_TRUE(gutter.height > 1);

    // Top of the gutter -> numerator 0 (scroll to the document top).
    auto top = ssg::hitTest(*snapshot, gutter.x, gutter.y);
    ASSERT_EQ(top.region, ssg::HitRegion::EditorScrollbar);
    ASSERT_EQ(top.scrollNumerator, std::uint32_t{0});
    ASSERT_EQ(top.scrollDenominator,
              static_cast<std::uint32_t>(gutter.height - 1));

    // Bottom of the gutter -> numerator == denominator (fraction 1.0), which
    // view.scroll_to_fraction turns into maximum_first_row (the document end).
    auto bottom = ssg::hitTest(*snapshot, gutter.x, gutter.bottom() - 1);
    ASSERT_EQ(bottom.region, ssg::HitRegion::EditorScrollbar);
    ASSERT_EQ(bottom.scrollNumerator, bottom.scrollDenominator);
    auto const maxFirst = snapshot->client().viewport.scrollbar.maximumFirstRow;
    auto const resolved = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(maxFirst) * bottom.scrollNumerator) /
        bottom.scrollDenominator);
    ASSERT_EQ(resolved, maxFirst);
}

TEST(tabBarCellMapsToItsTabIndex) {
    auto root = uniqueRoot();
    std::ofstream{root / "alpha.txt"} << "a\n";
    std::ofstream{root / "beta.txt"} << "b\n";
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    (void)runtime->dispatch(
        ssg::ClientId{1},
        {"file.open", runtime->revision(), std::string{"alpha.txt"}});
    (void)runtime->dispatch(
        ssg::ClientId{1},
        {"file.open", runtime->revision(), std::string{"beta.txt"}});
    auto snapshot = runtime->snapshot(ssg::ClientId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto const& shell = snapshot->sections().shell;
    ASSERT_TRUE(shell.tabHits.size() >= 2);
    if (shell.tabHits.size() < 2) return;

    // A cell inside each published tab rect resolves to that tab's index.
    for (auto const& tab : shell.tabHits) {
        auto hit = ssg::hitTest(*snapshot, tab.rect.x, tab.rect.y);
        ASSERT_EQ(hit.region, ssg::HitRegion::Tab);
        ASSERT_EQ(hit.tabIndex, tab.index);
    }

    // The tab-bar row past the last tab is padding, not a tab.
    auto const& last = shell.tabHits.back();
    ASSERT_TRUE(last.rect.right() < shell.viewport.columns);
    auto pad = ssg::hitTest(*snapshot, last.rect.right(), last.rect.y);
    ASSERT_TRUE(pad.region != ssg::HitRegion::Tab);
}

TEST(outOfBoundsAndChromeReturnNoTarget) {
    auto root = uniqueRoot();
    std::ofstream{root / "doc.txt"} << "alpha\n";
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    (void)runtime->dispatch(ssg::ClientId{1},
                            {"file.open", runtime->revision(), std::string{"doc.txt"}});
    auto snapshot = runtime->snapshot(ssg::ClientId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;

    ASSERT_EQ(ssg::hitTest(*snapshot, -1, 5).region, ssg::HitRegion::None);
    ASSERT_EQ(ssg::hitTest(*snapshot, 5, -1).region, ssg::HitRegion::None);
    ASSERT_EQ(ssg::hitTest(*snapshot, 9999, 5).region, ssg::HitRegion::None);
    ASSERT_EQ(ssg::hitTest(*snapshot, 5, 9999).region, ssg::HitRegion::None);
    // The header row (row 0) is chrome, not a region.
    ASSERT_EQ(ssg::hitTest(*snapshot, 0, 0).region, ssg::HitRegion::None);
}

}  // namespace

int main() {
    RUN(editorCellMapsToItsDocumentByteOffset);
    RUN(clickPastEolBlankLineAndBelowDocumentClampToLineEnd);
    RUN(clickPastEolIntegrationLandsCaretAtLineEnd);
    RUN(panelRowMapsToItsTreeNodeId);
    RUN(paletteRowMapsToItsAbsoluteRankIndex);
    RUN(paletteScrollbarAndEmptyAreaClassifyCorrectly);
    RUN(editorScrollbarFractionFeedsScrollToFraction);
    RUN(tabBarCellMapsToItsTabIndex);
    RUN(outOfBoundsAndChromeReturnNoTarget);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed > 0 ? 1 : 0;
}
