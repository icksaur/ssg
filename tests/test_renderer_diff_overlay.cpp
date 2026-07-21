#include <ssg/EditorRuntime.h>
#include <ssg/Renderer.h>
#include <ssg/session_snapshot.h>

#include "test_helpers.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace {

namespace fs = std::filesystem;

struct Fixture {
    fs::path root;
    std::unique_ptr<ssg::EditorRuntime> runtime;

    ~Fixture() { fs::remove_all(root); }
};

Fixture makeFixture(std::string_view text) {
    auto root = fs::current_path() /
                ("runtime_renderer_diff_" + std::to_string(std::rand()));
    fs::create_directories(root / "scratch");
    fs::create_directories(root / "recovery");
    std::ofstream{root / "overlay.cpp"} << text;
    auto created = ssg::EditorRuntime::create(
        {root, root / "scratch", root / "recovery"});
    if (!created.accepted()) return {std::move(root), nullptr};
    auto runtime = std::move(created.runtime);
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

ssg::DiffFileView overlayDiff(std::string_view currentContent) {
    return {
        ssg::DiffFileId{"overlay.cpp"},
        "overlay.cpp",
        std::nullopt,
        false,
        "baseline",
        std::string{currentContent},
        {
            ssg::DiffHunk{0, 0, {}, {"added search search\n"}},
            ssg::DiffHunk{0, 1, {"removed baseline\n"}, {}},
            ssg::DiffHunk{1, 1, {"old modified\n"}, {"modified\n"}},
        },
        {
            {ssg::DiffLineKind::Added, std::nullopt, std::size_t{0}},
            {ssg::DiffLineKind::Removed, std::size_t{0}, std::nullopt},
            {ssg::DiffLineKind::Modified, std::size_t{1}, std::size_t{1}},
        },
    };
}

ssg::SessionSnapshot snapshotWith(
    const ssg::SessionSnapshot& base, ssg::SessionSnapshotSections sections,
    ssg::ClientSnapshotState client) {
    return {base.revision(), base.topology(), std::move(client),
            std::move(sections)};
}

}

TEST(rendererComposesDiffOverlayWithSyntaxAndRolePrecedence) {
    const std::string text = "added search search\nmodified\nplain\n";
    auto fixture = makeFixture(text);
    ASSERT_TRUE(fixture.runtime != nullptr);
    if (!fixture.runtime) return;

    auto base = fixture.runtime->snapshot(ssg::ClientId{1}, {40, 10});
    ASSERT_TRUE(base.has_value());
    if (!base) return;

    auto sections = base->sections();
    auto client = base->client();
    auto diffFile = overlayDiff(text);
    sections.document.diffFileIdentity = diffFile.id.value();
    sections.diff =
        ssg::DiffViewState{sections.document.revision, {diffFile}};
    sections.syntax = ssg::SyntaxViewState{
        sections.document.revision,
        ssg::LanguageId::plainText(),
        text.size(),
        {{ssg::ByteOffset{0}, ssg::ByteOffset{5}, ssg::SyntaxScope::Keyword}},
        {},
        {},
        {},
        {},
        {},
    };
    client.viewport = ssg::Viewport{}.computeUnwrapped(
        text, client.viewport.dimensions, 0, 0, 4, &diffFile);
    const auto content = sections.shell.panes.front().content;
    auto overlay =
        snapshotWith(*base, std::move(sections), std::move(client));
    const auto grid = ssg::Renderer{}.render(overlay);

    const auto [addedColumn, addedRow] =
        findText(grid, "added search search");
    ASSERT_TRUE(addedColumn >= 0 && addedRow >= 0);
    if (addedColumn < 0 || addedRow < 0) return;
    ASSERT_EQ(grid.at(addedColumn, addedRow).tint, ssg::DiffTint::AddedRow);
    ASSERT_EQ(
        grid.at(addedColumn, addedRow).foreground,
        overlay.sections().theme.syntaxIndices[
            static_cast<std::size_t>(ssg::SyntaxScope::Keyword)]);
    ASSERT_EQ(grid.at(content.right() - 1, addedRow).tint,
              ssg::DiffTint::AddedRow);

    const auto [modifiedColumn, modifiedRow] = findText(grid, "modified");
    ASSERT_TRUE(modifiedColumn >= 0 && modifiedRow >= 0);
    if (modifiedColumn < 0 || modifiedRow < 0) return;
    ASSERT_EQ(grid.at(modifiedColumn, modifiedRow).tint,
              ssg::DiffTint::ModifiedRow);
    ASSERT_EQ(grid.at(content.right() - 1, modifiedRow).tint,
              ssg::DiffTint::ModifiedRow);

    const auto [removedColumn, removedRow] =
        findText(grid, "removed baseline");
    ASSERT_TRUE(removedColumn >= 0 && removedRow >= 0);
    if (removedColumn < 0 || removedRow < 0) return;
    ASSERT_EQ(grid.at(removedColumn, removedRow).tint,
              ssg::DiffTint::RemovedRow);
    ASSERT_EQ(grid.at(removedColumn, removedRow).foreground,
              overlay.sections().theme.semanticIndices[static_cast<std::size_t>(
                  ssg::SemanticRole::Foreground)]);
    ASSERT_EQ(grid.at(content.right() - 1, removedRow).tint,
              ssg::DiffTint::RemovedRow);

    auto scrolledSections = overlay.sections();
    auto scrolledClient = overlay.client();
    scrolledClient.viewport = ssg::Viewport{}.computeUnwrapped(
        text, scrolledClient.viewport.dimensions, 0, 8, 4, &diffFile);
    auto scrolled = snapshotWith(
        overlay, std::move(scrolledSections), std::move(scrolledClient));
    const auto scrolledGrid = ssg::Renderer{}.render(scrolled);
    const auto [scrolledRemovedColumn, scrolledRemovedRow] =
        findText(scrolledGrid, "baseline");
    ASSERT_EQ(scrolledRemovedColumn, content.x);
    ASSERT_TRUE(scrolledRemovedRow >= 0);
    if (scrolledRemovedRow >= 0) {
        ASSERT_EQ(scrolledGrid.at(scrolledRemovedColumn, scrolledRemovedRow).tint,
                  ssg::DiffTint::RemovedRow);
    }

    auto precedenceSections = overlay.sections();
    auto precedenceClient = overlay.client();
    const auto start = ssg::DocumentPosition{
        ssg::ByteOffset{0}, ssg::LineIndex{0}, ssg::CellIndex{0}};
    const auto end = ssg::DocumentPosition{
        ssg::ByteOffset{19}, ssg::LineIndex{0}, ssg::CellIndex{19}};
    precedenceSections.selection = ssg::SelectionViewState{
        ssg::SelectionSet{{ssg::Selection{start, end}}}, 0, 0, std::nullopt};
    precedenceSections.findReplace.open = true;
    precedenceSections.findReplace.sourceRevision =
        precedenceSections.document.revision;
    precedenceSections.findReplace.matches = {
        {ssg::ByteOffset{6}, ssg::ByteOffset{12}},
        {ssg::ByteOffset{13}, ssg::ByteOffset{19}},
    };
    precedenceSections.findReplace.activeMatch = std::size_t{0};
    auto precedence = snapshotWith(
        overlay, std::move(precedenceSections), std::move(precedenceClient));
    const auto precedenceGrid = ssg::Renderer{}.render(precedence);
    ASSERT_EQ(precedenceGrid.at(addedColumn, addedRow).role,
              ssg::SemanticRole::Selection);
    ASSERT_EQ(precedenceGrid.at(addedColumn, addedRow).tint,
              ssg::DiffTint::None);
    ASSERT_EQ(precedenceGrid.at(addedColumn + 13, addedRow).role,
              ssg::SemanticRole::SearchMatch);
    ASSERT_EQ(precedenceGrid.at(addedColumn + 13, addedRow).tint,
              ssg::DiffTint::None);

    auto noDiffSections = overlay.sections();
    auto noDiffClient = overlay.client();
    noDiffSections.diff = {};
    noDiffSections.document.diffFileIdentity.reset();
    noDiffClient.viewport = ssg::Viewport{}.computeUnwrapped(
        text, noDiffClient.viewport.dimensions, 0, 0, 4);
    auto noDiff = snapshotWith(
        overlay, std::move(noDiffSections), std::move(noDiffClient));
    const auto noDiffGrid = ssg::Renderer{}.render(noDiff);
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

    auto unidentifiedSections = noDiff.sections();
    auto unidentifiedClient = noDiff.client();
    unidentifiedSections.diff = overlay.sections().diff;
    auto unidentified = snapshotWith(
        noDiff, std::move(unidentifiedSections), std::move(unidentifiedClient));
    ASSERT_EQ(ssg::Renderer{}.render(unidentified).canonical(),
              noDiffGrid.canonical());
}

int main() {
    RUN(rendererComposesDiffOverlayWithSyntaxAndRolePrecedence);
    return failed == 0 ? 0 : 1;
}
