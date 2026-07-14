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

fs::path unique_root() {
    auto root = fs::current_path() / "hit_test_root";
    fs::remove_all(root);
    fs::create_directories(root / "scratch");
    fs::create_directories(root / "recovery");
    return root;
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

// ---------------------------------------------------------------------------

TEST(editor_cell_maps_to_its_document_byte_offset) {
    auto root = unique_root();
    std::string const text = "alpha\nbeta\ngamma\n";
    std::ofstream{root / "doc.txt"} << text;
    auto runtime = make_runtime(root);
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
    auto const& targets = snapshot->client().viewport.hit_targets;
    ASSERT_FALSE(targets.empty());
    if (targets.empty()) return;

    // Strong oracle: every hit target's byte offset must be DOCUMENT-absolute,
    // not line-relative. Cross-check each against the independent line model
    // (resolve_document_position / TextModel), which the app uses to turn a hit
    // into a caret. A regression to line-relative offsets makes every line's
    // cells resolve to line 0 and fails here immediately.
    bool saw_second_line = false;
    for (auto const& target : targets) {
        int const column = content.x + static_cast<int>(target.viewport_column);
        int const row = content.y + static_cast<int>(target.viewport_row);
        auto hit = ssg::hit_test(*snapshot, column, row);
        ASSERT_EQ(hit.region, ssg::HitRegion::editor);
        ASSERT_EQ(hit.byte_offset, target.byte_offset);
        auto position =
            ssg::resolve_document_position(text, ssg::ByteOffset{hit.byte_offset});
        ASSERT_TRUE(position.has_value());
        if (position) {
            ASSERT_EQ(position->line.value(),
                      static_cast<std::uint64_t>(target.logical_line));
        }
        if (target.logical_line > 0) saw_second_line = true;
    }
    // The document has three lines, so the targets must reach past line 0 (the
    // property above is only meaningful if we actually exercised later lines).
    ASSERT_TRUE(saw_second_line);

    // Column 0 of a later visual row resolves to that line's first byte.
    ssg::CellHitTarget const* line_one_start = nullptr;
    for (auto const& target : targets) {
        if (target.logical_line == 1 && target.viewport_column == 0) {
            line_one_start = &target;
            break;
        }
    }
    ASSERT_TRUE(line_one_start != nullptr);
    if (line_one_start) {
        ASSERT_EQ(line_one_start->byte_offset, std::uint32_t{6});  // after "alpha\n"
    }

    // A cell far past the end of the short first line has no document position.
    auto blank = ssg::hit_test(*snapshot, content.right() - 2, content.y);
    ASSERT_EQ(blank.region, ssg::HitRegion::none);
}

TEST(panel_row_maps_to_its_tree_node_id) {
    auto root = unique_root();
    for (int i = 0; i < 6; ++i) {
        char name[32];
        std::snprintf(name, sizeof name, "file-%02d.txt", i);
        std::ofstream{root / name} << "x";
    }
    auto runtime = make_runtime(root);
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
    ASSERT_FALSE(provider.visible_node_ids.empty());
    if (provider.visible_node_ids.empty()) return;

    // The provider-label row (panel.y) is not a node.
    auto label = ssg::hit_test(*snapshot, shell.panel->x, shell.panel->y);
    ASSERT_EQ(label.region, ssg::HitRegion::none);

    // The first content row maps to the first visible node id.
    auto hit = ssg::hit_test(*snapshot, shell.panel->x, shell.panel->y + 1);
    ASSERT_EQ(hit.region, ssg::HitRegion::panel);
    ASSERT_TRUE(hit.node_id.has_value());
    if (hit.node_id) ASSERT_EQ(*hit.node_id, provider.visible_node_ids.front());

    // A row below the last visible node is empty.
    auto empty = ssg::hit_test(*snapshot, shell.panel->x, shell.panel->bottom() - 1);
    ASSERT_EQ(empty.region, ssg::HitRegion::none);
}

TEST(palette_row_maps_to_its_absolute_rank_index) {
    auto root = unique_root();
    std::ofstream{root / "doc.txt"} << "alpha\n";
    auto runtime = make_runtime(root);
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
    projection.scrollbar_rect = pane.scrollbar;
    projection.first_visible = 20;
    projection.selected = std::uint32_t{25};
    projection.scrollbar = ssg::scrollbar_metrics(40, rows, 20);
    for (std::uint32_t i = 0; i < rows; ++i) {
        projection.rows.push_back({"cmd-" + std::to_string(20 + i), ""});
    }
    sections.shell.palette = projection;
    ssg::SessionSnapshot projected{snapshot->revision(), snapshot->topology(),
                                   snapshot->client(), std::move(sections)};

    auto hit = ssg::hit_test(projected, pane.content.x, pane.content.y + 3);
    ASSERT_EQ(hit.region, ssg::HitRegion::palette);
    ASSERT_EQ(hit.item_index, std::uint32_t{23});

    // The palette overlays the pane: a document cell is inert while it is open.
    auto over_doc = ssg::hit_test(projected, pane.content.x, pane.content.y);
    ASSERT_EQ(over_doc.region, ssg::HitRegion::palette);
    ASSERT_EQ(over_doc.item_index, std::uint32_t{20});
}

TEST(palette_scrollbar_and_empty_area_classify_correctly) {
    auto root = unique_root();
    std::ofstream{root / "doc.txt"} << "alpha\n";
    auto runtime = make_runtime(root);
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
    projection.scrollbar_rect = pane.scrollbar;
    projection.first_visible = 0;
    projection.selected = std::uint32_t{0};
    projection.scrollbar = ssg::scrollbar_metrics(100, rows, 0);
    for (std::uint32_t i = 0; i < rows; ++i) {  // exactly fills the window
        projection.rows.push_back({"cmd-" + std::to_string(i), ""});
    }
    sections.shell.palette = projection;
    ssg::SessionSnapshot projected{snapshot->revision(), snapshot->topology(),
                                   snapshot->client(), std::move(sections)};

    // Top of the palette gutter -> fraction 0; bottom -> fraction ~1.
    auto top = ssg::hit_test(projected, pane.scrollbar.x, pane.scrollbar.y);
    ASSERT_EQ(top.region, ssg::HitRegion::palette_scrollbar);
    ASSERT_EQ(top.scroll_numerator, std::uint32_t{0});
    auto bottom = ssg::hit_test(projected, pane.scrollbar.x, pane.scrollbar.bottom() - 1);
    ASSERT_EQ(bottom.region, ssg::HitRegion::palette_scrollbar);
    ASSERT_EQ(bottom.scroll_numerator, bottom.scroll_denominator);
}

TEST(editor_scrollbar_fraction_feeds_scroll_to_fraction) {
    auto root = unique_root();
    std::string text;
    for (int i = 0; i < 100; ++i) text += "line " + std::to_string(i) + "\n";
    std::ofstream{root / "tall.txt"} << text;
    auto runtime = make_runtime(root);
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
    auto top = ssg::hit_test(*snapshot, gutter.x, gutter.y);
    ASSERT_EQ(top.region, ssg::HitRegion::editor_scrollbar);
    ASSERT_EQ(top.scroll_numerator, std::uint32_t{0});
    ASSERT_EQ(top.scroll_denominator,
              static_cast<std::uint32_t>(gutter.height - 1));

    // Bottom of the gutter -> numerator == denominator (fraction 1.0), which
    // view.scroll_to_fraction turns into maximum_first_row (the document end).
    auto bottom = ssg::hit_test(*snapshot, gutter.x, gutter.bottom() - 1);
    ASSERT_EQ(bottom.region, ssg::HitRegion::editor_scrollbar);
    ASSERT_EQ(bottom.scroll_numerator, bottom.scroll_denominator);
    auto const max_first = snapshot->client().viewport.scrollbar.maximum_first_row;
    auto const resolved = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(max_first) * bottom.scroll_numerator) /
        bottom.scroll_denominator);
    ASSERT_EQ(resolved, max_first);
}

TEST(tab_bar_cell_maps_to_its_tab_index) {
    auto root = unique_root();
    std::ofstream{root / "alpha.txt"} << "a\n";
    std::ofstream{root / "beta.txt"} << "b\n";
    auto runtime = make_runtime(root);
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
    ASSERT_TRUE(shell.tab_hits.size() >= 2);
    if (shell.tab_hits.size() < 2) return;

    // A cell inside each published tab rect resolves to that tab's index.
    for (auto const& tab : shell.tab_hits) {
        auto hit = ssg::hit_test(*snapshot, tab.rect.x, tab.rect.y);
        ASSERT_EQ(hit.region, ssg::HitRegion::tab);
        ASSERT_EQ(hit.tab_index, tab.index);
    }

    // The tab-bar row past the last tab is padding, not a tab.
    auto const& last = shell.tab_hits.back();
    ASSERT_TRUE(last.rect.right() < shell.viewport.columns);
    auto pad = ssg::hit_test(*snapshot, last.rect.right(), last.rect.y);
    ASSERT_TRUE(pad.region != ssg::HitRegion::tab);
}

TEST(out_of_bounds_and_chrome_return_no_target) {
    auto root = unique_root();
    std::ofstream{root / "doc.txt"} << "alpha\n";
    auto runtime = make_runtime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    (void)runtime->dispatch(ssg::ClientId{1},
                            {"file.open", runtime->revision(), std::string{"doc.txt"}});
    auto snapshot = runtime->snapshot(ssg::ClientId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;

    ASSERT_EQ(ssg::hit_test(*snapshot, -1, 5).region, ssg::HitRegion::none);
    ASSERT_EQ(ssg::hit_test(*snapshot, 5, -1).region, ssg::HitRegion::none);
    ASSERT_EQ(ssg::hit_test(*snapshot, 9999, 5).region, ssg::HitRegion::none);
    ASSERT_EQ(ssg::hit_test(*snapshot, 5, 9999).region, ssg::HitRegion::none);
    // The header row (row 0) is chrome, not a region.
    ASSERT_EQ(ssg::hit_test(*snapshot, 0, 0).region, ssg::HitRegion::none);
}

}  // namespace

int main() {
    RUN(editor_cell_maps_to_its_document_byte_offset);
    RUN(panel_row_maps_to_its_tree_node_id);
    RUN(palette_row_maps_to_its_absolute_rank_index);
    RUN(palette_scrollbar_and_empty_area_classify_correctly);
    RUN(editor_scrollbar_fraction_feeds_scroll_to_fraction);
    RUN(tab_bar_cell_maps_to_its_tab_index);
    RUN(out_of_bounds_and_chrome_return_no_target);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed > 0 ? 1 : 0;
}
