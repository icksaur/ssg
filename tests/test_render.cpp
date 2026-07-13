#include <ssg/render.h>

#include <ssg/editor_runtime.h>
#include <ssg/session_snapshot.h>

#include "test_helpers.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path unique_root() {
    auto base = fs::temp_directory_path() /
                ("ssg-render-" + std::to_string(::rand()));
    fs::create_directories(base / "scratch");
    fs::create_directories(base / "recovery");
    return base;
}

std::unique_ptr<ssg::EditorRuntime> make_runtime(fs::path const& root) {
    auto created = ssg::EditorRuntime::create(
        {root, root / "scratch", root / "recovery"});
    if (!created.accepted()) return nullptr;
    auto runtime = std::move(created.runtime);
    (void)runtime->attach({ssg::ClientId{1}, ssg::InvocationOrigin::in_process},
                          ssg::ViewId{1});
    return runtime;
}

}  // namespace

namespace {

std::string row_text(ssg::CellGrid const& grid, int row) {
    std::string line;
    for (int column = 0; column < grid.size.columns; ++column) {
        auto const& cell = grid.at(column, row);
        if (!cell.continuation) line += cell.text;
    }
    return line;
}

bool grid_contains(ssg::CellGrid const& grid, std::string_view needle) {
    for (int row = 0; row < grid.size.rows; ++row) {
        if (row_text(grid, row).find(needle) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

TEST(render_paints_content_not_accessibility_labels) {
    auto root = unique_root();
    std::ofstream{root / "hello.txt"} << "alpha\nbeta\n";
    auto runtime = make_runtime(root);
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
    ASSERT_FALSE(grid_contains(grid, "Status header"));
    ASSERT_FALSE(grid_contains(grid, "Open tabs"));
    ASSERT_FALSE(grid_contains(grid, "Workspace"));
    // The open document's real content is painted.
    ASSERT_TRUE(grid_contains(grid, "alpha"));
}

TEST(render_colors_are_palette_indices) {
    auto root = unique_root();
    auto runtime = make_runtime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    auto snapshot = runtime->snapshot(ssg::ClientId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto grid = ssg::render(*snapshot);
    bool all_in_palette = true;
    for (auto const& cell : grid.cells) {
        if (cell.foreground >= ssg::theme_palette_size ||
            cell.background >= ssg::theme_palette_size) {
            all_in_palette = false;
        }
    }
    ASSERT_TRUE(all_in_palette);
}

TEST(render_is_deterministic) {
    auto root = unique_root();
    std::ofstream{root / "file.txt"} << "content\n";
    auto runtime = make_runtime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    auto first = runtime->snapshot(ssg::ClientId{1}, {80, 24});
    auto second = runtime->snapshot(ssg::ClientId{1}, {80, 24});
    ASSERT_TRUE(first.has_value() && second.has_value());
    if (!first || !second) return;
    ASSERT_EQ(ssg::render(*first).canonical(), ssg::render(*second).canonical());
}

TEST(render_projects_palette_results_into_active_pane) {
    auto root = unique_root();
    std::ofstream{root / "hello.txt"} << "alpha\nbeta\n";
    auto runtime = make_runtime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    (void)runtime->dispatch(
        ssg::ClientId{1},
        {"file.open", runtime->revision(), std::string{"hello.txt"}});
    auto snapshot = runtime->snapshot(ssg::ClientId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;

    // Without a palette projection the document content is painted.
    ASSERT_TRUE(grid_contains(ssg::render(*snapshot), "alpha"));

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
    ASSERT_TRUE(grid_contains(grid, "file.save"));
    ASSERT_TRUE(grid_contains(grid, "ESC q"));
    ASSERT_FALSE(grid_contains(grid, "alpha"));

    // The selected row is painted with the selection role.
    int const selected_row = projection.rect.y + 1;
    bool selection_painted = false;
    for (int column = projection.rect.x; column < projection.rect.right();
         ++column) {
        if (grid.at(column, selected_row).role == ssg::SemanticRole::selection) {
            selection_painted = true;
            break;
        }
    }
    ASSERT_TRUE(selection_painted);
}

int main() {
    RUN(render_paints_content_not_accessibility_labels);
    RUN(render_colors_are_palette_indices);
    RUN(render_is_deterministic);
    RUN(render_projects_palette_results_into_active_pane);

    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed > 0 ? 1 : 0;
}
