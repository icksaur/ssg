#include <ssg/EditorSession.h>
#include <ssg/Renderer.h>
#include <ssg/session_snapshot.h>

#include "test_helpers.h"
#include "grid_test_frame.h"

#include <cstdlib>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace {

namespace fs = std::filesystem;

struct Fixture {
    fs::path root;
    std::unique_ptr<ssg::EditorSession> runtime;

    ~Fixture() { fs::remove_all(root); }
};

Fixture makeFixture(std::string_view text) {
    auto root = fs::current_path() /
                ("runtime_renderer_diff_" + std::to_string(std::rand()));
    fs::create_directories(root / "scratch");
    fs::create_directories(root / "recovery");
    std::ofstream{root / "overlay.cpp"} << text;
    auto created = ssg::EditorSession::create(
        {root, root / "scratch", root / "recovery"});
    if (!created.accepted()) return {std::move(root), nullptr};
    auto runtime = std::move(created.session);
    (void)runtime->attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                          ssg::ViewId{1});
    (void)runtime->dispatch(
        ssg::ClientId{1},
        {"file.open", runtime->revision(), std::string{"overlay.cpp"}});
    return {std::move(root), std::move(runtime)};
}

std::pair<int, int> findText(const ssg::CellGrid& grid,
                             std::string_view text) {
    for (int row = 0; row < grid.size.rows; ++row) {
        for (int column = 0; column < grid.size.columns; ++column) {
            std::string candidate;
            for (int offset = 0;
                 column + offset < grid.size.columns &&
                 candidate.size() < text.size();
                 ++offset) {
                candidate += grid.at(column + offset, row).text;
            }
            if (candidate == text) return {column, row};
        }
    }
    return {-1, -1};
}

bool hasDiffTintInPane(const ssg::CellGrid& grid, const ssg::Rect& paneContent) {
    for (int row = paneContent.y; row < paneContent.bottom(); ++row) {
        for (int column = paneContent.x; column < paneContent.right(); ++column) {
            if (grid.at(column, row).tint != ssg::DiffTint::None) {
                return true;
            }
        }
    }
    return false;
}

ssg::DiffFileView overlayDiff(std::string_view currentContent) {
    return {
        .id = ssg::DiffFileId{"overlay.cpp"},
        .path = "overlay.cpp",
        .baselineIdentity = "baseline",
        .currentContent = std::string{currentContent},
        .hunks = {
            ssg::DiffHunk{0, 0, {}, {"added search search\n"}},
            ssg::DiffHunk{0, 1, {"removed baseline\n"}, {}},
            ssg::DiffHunk{1, 1, {"int foo = 1;\n"},
                          {"int new foo = 42;\n"}},
        },
        .changedLines = {
            {ssg::DiffLineKind::Added, std::nullopt, std::size_t{0}},
            {ssg::DiffLineKind::Removed, std::size_t{0}, std::nullopt},
            {ssg::DiffLineKind::Modified,
             std::size_t{1},
             std::size_t{1},
             {{4, 4}},
             {{10, 1}},
             {{14, 2}}},
        },
    };
}

ssg::GridPresentation snapshotWith(
    const ssg::GridPresentation& base,
    ssg::SessionSnapshotSections sections,
    ssg::GridProjection projection) {
    return ssg::test::copyGridFrame(
        base, std::move(sections), std::move(projection), base.palette());
}

}

TEST(rendererPaintsDiffTintForRuntimeOpenedLiveDiffTab) {
    const std::string baseline = "int value = 1;\n";
    const std::string working = "int value = 42;\n";
    auto fixture = makeFixture(working);
    ASSERT_TRUE(fixture.runtime != nullptr);
    if (!fixture.runtime) return;
    auto& runtime = *fixture.runtime;

    ASSERT_TRUE(runtime
                    .applyGitDiffScan(
                        {.revision = ssg::Revision{1},
                         .baselineIdentity = "head-1:index-1",
                         .files = {{.id = ssg::DiffFileId{"overlay-id"},
                                    .path = "overlay.cpp",
                                    .baselineContent = baseline,
                                    .workingContent = working}}})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"panel.show_git_status", runtime.revision(), {}})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"tree.select_next", runtime.revision(), {}})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"tree.activate", runtime.revision(), {}})
                    .accepted());

    auto snapshot = ssg::test::projectGridFrame(
        runtime, ssg::ClientId{1}, ssg::ViewId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_TRUE(snapshot->semantic().sections().tabs.active.has_value());
    const auto active = std::find_if(
        snapshot->semantic().sections().tabs.tabs.begin(),
        snapshot->semantic().sections().tabs.tabs.end(),
        [&](const ssg::TabState& tab) {
            return snapshot->semantic().sections().tabs.active &&
                   tab.id == *snapshot->semantic().sections().tabs.active;
        });
    ASSERT_TRUE(active != snapshot->semantic().sections().tabs.tabs.end());
    if (active != snapshot->semantic().sections().tabs.tabs.end()) {
        ASSERT_EQ(active->kind, ssg::TabKind::LiveDiff);
    }
    ASSERT_EQ(snapshot->semantic().sections().document.diffFileIdentity,
              std::optional<std::string>{"overlay-id"});
    ASSERT_TRUE(snapshot->document().has_value());
    if (!snapshot->document()) return;
    const auto grid = ssg::Renderer{}.render(*snapshot);
    const auto paneContent = snapshot->document()->content;
    ASSERT_TRUE(hasDiffTintInPane(grid, paneContent));

    auto sections = snapshot->semantic().sections();
    sections.document.revision =
        ssg::Revision{sections.document.revision.value() + 1};
    auto mismatched = snapshotWith(
        *snapshot, std::move(sections), snapshot->presentation());
    const auto mismatchGrid = ssg::Renderer{}.render(mismatched);
    ASSERT_TRUE(hasDiffTintInPane(mismatchGrid, paneContent));
}

TEST(rendererComposesDiffOverlayWithSyntaxAndRolePrecedence) {
    const std::string text = "added search search\nint new foo = 42;\nplain\n";
    auto fixture = makeFixture(text);
    ASSERT_TRUE(fixture.runtime != nullptr);
    if (!fixture.runtime) return;

    auto base = ssg::test::projectGridFrame(
        *fixture.runtime, ssg::ClientId{1}, ssg::ViewId{1}, {40, 10});
    ASSERT_TRUE(base.has_value());
    if (!base) return;

    auto sections = base->semantic().sections();
    auto presentation = base->presentation();
    auto diffFile = overlayDiff(text);
    sections.document.diffFileIdentity = diffFile.id.value();
    sections.diff =
        ssg::DiffViewState{sections.document.revision, {diffFile}};
    sections.syntax = ssg::SyntaxViewState{
        sections.document.revision,
        ssg::LanguageId::plainText(),
        text.size(),
        {{ssg::ByteOffset{0}, ssg::ByteOffset{5}, ssg::SyntaxScope::Keyword},
         {ssg::ByteOffset{34}, ssg::ByteOffset{36}, ssg::SyntaxScope::Number}},
        {},
        {},
        {},
        {},
        {},
    };
    presentation.viewport = ssg::Viewport{}.computeUnwrapped(
        text, presentation.viewport.dimensions, 0, 0, 4, &diffFile);
    ASSERT_TRUE(base->document().has_value());
    if (!base->document()) return;
    const auto content = base->document()->content;
    auto overlay =
        snapshotWith(*base, std::move(sections), std::move(presentation));
    const auto grid = ssg::Renderer{}.render(overlay);

    const auto [addedColumn, addedRow] =
        findText(grid, "added search search");
    ASSERT_TRUE(addedColumn >= 0 && addedRow >= 0);
    if (addedColumn < 0 || addedRow < 0) return;
    ASSERT_EQ(grid.at(addedColumn, addedRow).tint, ssg::DiffTint::AddedRow);
    ASSERT_EQ(
        grid.at(addedColumn, addedRow).foreground,
        ssg::kSemanticRoleCount +
            static_cast<std::size_t>(ssg::SyntaxScope::Keyword));
    ASSERT_EQ(grid.at(content.right() - 1, addedRow).tint,
              ssg::DiffTint::AddedRow);

    const auto [modifiedColumn, modifiedRow] =
        findText(grid, "int new foo = 42;");
    ASSERT_TRUE(modifiedColumn >= 0 && modifiedRow >= 0);
    if (modifiedColumn < 0 || modifiedRow < 0) return;
    ASSERT_EQ(grid.at(modifiedColumn, modifiedRow).tint,
              ssg::DiffTint::ModifiedRow);
    ASSERT_EQ(grid.at(modifiedColumn + 4, modifiedRow).tint,
              ssg::DiffTint::AddedWord);
    // A changed-in-place token (e.g. "1" -> "42") reuses the AddedWord tint
    // directly -- there is no separate ModifiedWord color (only three diff
    // colors exist: added/removed/modified; see Renderer.cpp).
    ASSERT_EQ(grid.at(modifiedColumn + 14, modifiedRow).tint,
              ssg::DiffTint::AddedWord);
    ASSERT_EQ(
        grid.at(modifiedColumn + 14, modifiedRow).foreground,
        ssg::kSemanticRoleCount +
            static_cast<std::size_t>(ssg::SyntaxScope::Number));
    ASSERT_EQ(grid.at(content.right() - 1, modifiedRow).tint,
              ssg::DiffTint::ModifiedRow);

    const auto [removedColumn, removedRow] =
        findText(grid, "removed baseline");
    ASSERT_TRUE(removedColumn >= 0 && removedRow >= 0);
    if (removedColumn < 0 || removedRow < 0) return;
    ASSERT_EQ(grid.at(removedColumn, removedRow).tint,
              ssg::DiffTint::RemovedRow);
    ASSERT_EQ(grid.at(removedColumn, removedRow).foreground,
              static_cast<std::size_t>(ssg::SemanticRole::Text));
    ASSERT_EQ(grid.at(content.right() - 1, removedRow).tint,
              ssg::DiffTint::RemovedRow);

    auto scrolledSections = overlay.semantic().sections();
    auto scrolledPresentation = overlay.presentation();
    scrolledPresentation.viewport = ssg::Viewport{}.computeUnwrapped(
        text, scrolledPresentation.viewport.dimensions, 0, 8, 4, &diffFile);
    auto scrolled = snapshotWith(
        overlay, std::move(scrolledSections), std::move(scrolledPresentation));
    const auto scrolledGrid =
        ssg::Renderer{}.render(scrolled);
    const auto [scrolledRemovedColumn, scrolledRemovedRow] =
        findText(scrolledGrid, "baseline");
    ASSERT_EQ(scrolledRemovedColumn, content.x);
    ASSERT_TRUE(scrolledRemovedRow >= 0);
    if (scrolledRemovedRow >= 0) {
        ASSERT_EQ(scrolledGrid.at(scrolledRemovedColumn, scrolledRemovedRow).tint,
                  ssg::DiffTint::RemovedRow);
    }

    auto precedenceSections = overlay.semantic().sections();
    const auto start = ssg::DocumentPosition{
        ssg::ByteOffset{0}, ssg::LineIndex{0}, ssg::CellIndex{0}};
    const auto end = ssg::DocumentPosition{
        ssg::ByteOffset{19}, ssg::LineIndex{0}, ssg::CellIndex{19}};
    precedenceSections.selection =
        ssg::SelectionSet{{ssg::Selection{start, end}}};
    precedenceSections.findReplace.open = true;
    precedenceSections.findReplace.sourceRevision =
        precedenceSections.document.revision;
    precedenceSections.findReplace.matches = {
        {ssg::ByteOffset{6}, ssg::ByteOffset{12}},
        {ssg::ByteOffset{13}, ssg::ByteOffset{19}},
        {ssg::ByteOffset{34}, ssg::ByteOffset{36}},
    };
    precedenceSections.findReplace.activeMatch = std::size_t{0};
    auto precedence = snapshotWith(
        overlay, std::move(precedenceSections), overlay.presentation());
    const auto precedenceGrid =
        ssg::Renderer{}.render(precedence);
    ASSERT_EQ(precedenceGrid.at(addedColumn, addedRow).role,
              ssg::SemanticRole::Selection);
    ASSERT_EQ(precedenceGrid.at(addedColumn, addedRow).tint,
              ssg::DiffTint::None);
    ASSERT_EQ(precedenceGrid.at(addedColumn + 13, addedRow).role,
              ssg::SemanticRole::SearchMatch);
    ASSERT_EQ(precedenceGrid.at(addedColumn + 13, addedRow).tint,
              ssg::DiffTint::None);
    ASSERT_EQ(precedenceGrid.at(modifiedColumn + 14, modifiedRow).role,
              ssg::SemanticRole::SearchMatch);
    ASSERT_EQ(precedenceGrid.at(modifiedColumn + 14, modifiedRow).tint,
              ssg::DiffTint::None);

    auto wordSelectionSections = overlay.semantic().sections();
    const auto wordStart = ssg::DocumentPosition{
        ssg::ByteOffset{34}, ssg::LineIndex{1}, ssg::CellIndex{14}};
    const auto wordEnd = ssg::DocumentPosition{
        ssg::ByteOffset{36}, ssg::LineIndex{1}, ssg::CellIndex{16}};
    wordSelectionSections.selection =
        ssg::SelectionSet{{ssg::Selection{wordStart, wordEnd}}};
    auto wordSelection =
        snapshotWith(overlay, std::move(wordSelectionSections),
                     overlay.presentation());
    const auto wordSelectionGrid =
        ssg::Renderer{}.render(wordSelection);
    ASSERT_EQ(wordSelectionGrid.at(modifiedColumn + 14, modifiedRow).role,
              ssg::SemanticRole::Selection);
    ASSERT_EQ(wordSelectionGrid.at(modifiedColumn + 14, modifiedRow).tint,
              ssg::DiffTint::None);

    auto noDiffSections = overlay.semantic().sections();
    auto noDiffPresentation = overlay.presentation();
    noDiffSections.diff = {};
    noDiffSections.document.diffFileIdentity.reset();
    noDiffPresentation.viewport = ssg::Viewport{}.computeUnwrapped(
        text, noDiffPresentation.viewport.dimensions, 0, 0, 4);
    auto noDiff = snapshotWith(
        overlay, std::move(noDiffSections), std::move(noDiffPresentation));
    const auto noDiffGrid =
        ssg::Renderer{}.render(noDiff);
    for (const auto& cell : noDiffGrid.cells) {
        ASSERT_EQ(cell.tint, ssg::DiffTint::None);
    }
    const auto [plainAddedColumn, plainAddedRow] =
        findText(noDiffGrid, "added search search");
    ASSERT_TRUE(plainAddedColumn >= 0 && plainAddedRow >= 0);
    if (plainAddedColumn >= 0 && plainAddedRow >= 0) {
        ASSERT_EQ(noDiffGrid.at(plainAddedColumn, plainAddedRow).foreground,
                  grid.at(addedColumn, addedRow).foreground);
    }

    auto unidentifiedSections = noDiff.semantic().sections();
    unidentifiedSections.diff = overlay.semantic().sections().diff;
    auto unidentified = snapshotWith(
        noDiff, std::move(unidentifiedSections), noDiff.presentation());
    ASSERT_EQ(ssg::Renderer{}
                 .render(unidentified)
                 .canonical(),
              noDiffGrid.canonical());
}

int main() {
    RUN(rendererPaintsDiffTintForRuntimeOpenedLiveDiffTab);
    RUN(rendererComposesDiffOverlayWithSyntaxAndRolePrecedence);
    return failed == 0 ? 0 : 1;
}
