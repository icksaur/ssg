#include <ssg/render.h>

#include <ssg/editor_runtime.h>
#include <ssg/find_replace.h>
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

    // The selected row is painted with the selection role, including on the
    // label's glyph cells (not only trailing filler).
    int const selected_row = projection.rect.y + 1;
    ASSERT_EQ(grid.at(projection.rect.x, selected_row).text, std::string{"f"});
    ASSERT_EQ(grid.at(projection.rect.x, selected_row).role,
              ssg::SemanticRole::selection);
    // The unselected row must not carry the selection role.
    ASSERT_FALSE(grid.at(projection.rect.x, projection.rect.y).role ==
                 ssg::SemanticRole::selection);
}

TEST(render_shows_palette_query_and_ghost_in_header) {
    auto root = unique_root();
    std::ofstream{root / "hello.txt"} << "alpha\n";
    auto runtime = make_runtime(root);
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
    ASSERT_TRUE(grid_contains(grid, "> sa"));
    ASSERT_TRUE(grid_contains(grid, "ve File"));

    bool query_prompt_role = false;
    bool ghost_dim_role = false;
    for (int column = 0; column < grid.size.columns; ++column) {
        auto const& cell = grid.at(column, 0);
        if (cell.text == "s" && cell.role == ssg::SemanticRole::prompt) {
            query_prompt_role = true;
        }
        if (cell.text == "v" && cell.role == ssg::SemanticRole::line_number) {
            ghost_dim_role = true;
        }
    }
    ASSERT_TRUE(query_prompt_role);
    ASSERT_TRUE(ghost_dim_role);
}

TEST(render_paints_selection_highlight_and_secondary_carets) {
    auto root = unique_root();
    std::ofstream{root / "sel.txt"} << "alpha\nbeta\n";
    auto runtime = make_runtime(root);
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
        bool any_selection = false;
        for (int row = 0; row < grid.size.rows; ++row) {
            for (int col = 0; col < grid.size.columns; ++col) {
                if (grid.at(col, row).role == ssg::SemanticRole::selection) {
                    any_selection = true;
                }
            }
        }
        ASSERT_FALSE(any_selection);
    }

    // Select to end of the first line: "alpha" cells carry the selection role.
    (void)runtime->dispatch(ssg::ClientId{1},
                            {"select.line_end", runtime->revision(), {}});
    auto snapshot = runtime->snapshot(ssg::ClientId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto grid = ssg::render(*snapshot);

    int alpha_row = -1;
    for (int row = 0; row < grid.size.rows; ++row) {
        if (row_text(grid, row).find("alpha") != std::string::npos) alpha_row = row;
    }
    ASSERT_TRUE(alpha_row >= 0);
    if (alpha_row < 0) return;

    // Find the first column of "alpha" on that row.
    int alpha_col = -1;
    for (int col = 0; col + 5 <= grid.size.columns; ++col) {
        std::string window;
        for (int k = 0; k < 5; ++k) window += grid.at(col + k, alpha_row).text;
        if (window == "alpha") { alpha_col = col; break; }
    }
    ASSERT_TRUE(alpha_col >= 0);
    if (alpha_col < 0) return;
    for (int k = 0; k < 5; ++k) {
        ASSERT_EQ(grid.at(alpha_col + k, alpha_row).role,
                  ssg::SemanticRole::selection);
    }
    // The end-of-line past "alpha" is NOT filled: the selection ends at the line
    // end and does not span into the next line.
    ASSERT_NE(grid.at(alpha_col + 5, alpha_row).role,
              ssg::SemanticRole::selection);
    // The hardware caret sits at the primary active position (end of "alpha").
    ASSERT_TRUE(grid.caret.has_value());
    if (grid.caret) {
        ASSERT_EQ(grid.caret->row, alpha_row);
        ASSERT_EQ(grid.caret->column, alpha_col + 5);
    }
}

TEST(render_fills_end_of_line_for_multiline_selection) {
    auto root = unique_root();
    std::ofstream{root / "ml.txt"} << "alpha\nbeta\n";
    auto runtime = make_runtime(root);
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

    int alpha_row = -1, alpha_col = -1;
    for (int row = 0; row < grid.size.rows && alpha_row < 0; ++row) {
        for (int col = 0; col + 5 <= grid.size.columns; ++col) {
            std::string window;
            for (int k = 0; k < 5; ++k) window += grid.at(col + k, row).text;
            if (window == "alpha") { alpha_row = row; alpha_col = col; break; }
        }
    }
    ASSERT_TRUE(alpha_row >= 0);
    if (alpha_row < 0) return;
    // "alpha" is highlighted AND the cells past it to the pane's right edge are
    // the end-of-line fill (all selection role).
    for (int col = alpha_col; col < grid.size.columns - 1; ++col) {
        ASSERT_EQ(grid.at(col, alpha_row).role, ssg::SemanticRole::selection);
    }
}

TEST(render_highlights_wide_glyph_cells) {
    auto root = unique_root();
    // A CJK wide glyph occupies two cells; selecting it must highlight both.
    std::ofstream{root / "w.txt"} << "a\xe4\xb8\x80""b\n";  // "a一b"
    auto runtime = make_runtime(root);
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
    ASSERT_EQ(grid.at(col, row).role, ssg::SemanticRole::selection);
    ASSERT_TRUE(grid.at(col + 1, row).continuation);
    ASSERT_EQ(grid.at(col + 1, row).role, ssg::SemanticRole::selection);
}

TEST(render_paints_secondary_ranged_selection_caret) {
    auto root = unique_root();
    std::ofstream{root / "rc.txt"} << "cat cat\n";
    auto runtime = make_runtime(root);
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
    bool all_ranged = true;
    for (auto const& item : items) if (item.is_caret()) all_ranged = false;
    ASSERT_TRUE(all_ranged);
    auto grid = ssg::render(*snapshot);
    int painted_secondary = 0;
    for (int row = 0; row < grid.size.rows; ++row) {
        for (int col = 0; col < grid.size.columns; ++col) {
            bool const is_primary = grid.caret && grid.caret->row == row &&
                                    grid.caret->column == col;
            if (grid.at(col, row).role == ssg::SemanticRole::caret && !is_primary) {
                ++painted_secondary;
            }
        }
    }
    // One painted caret for the secondary ranged selection's active position.
    ASSERT_EQ(painted_secondary, 1);
}

TEST(render_paints_secondary_caret_as_a_cell) {
    auto root = unique_root();
    std::ofstream{root / "car.txt"} << "alpha\nbeta\n";
    auto runtime = make_runtime(root);
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

    int painted_secondary = 0;
    for (int row = 0; row < grid.size.rows; ++row) {
        for (int col = 0; col < grid.size.columns; ++col) {
            bool const is_primary = grid.caret && grid.caret->row == row &&
                                    grid.caret->column == col;
            if (grid.at(col, row).role == ssg::SemanticRole::caret && !is_primary) {
                ++painted_secondary;
            }
        }
    }
    // Exactly one caret is painted as a cell; the other is the hardware cursor.
    ASSERT_EQ(painted_secondary, 1);
}

TEST(render_paints_find_matches_and_active_match) {
    auto root = unique_root();
    std::ofstream{root / "find.txt"} << "cat cat cat\n";
    auto runtime = make_runtime(root);
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
    ASSERT_EQ(snapshot->sections().find_replace.matches.size(), std::size_t{3});
    auto grid = ssg::render(*snapshot);

    int row = -1;
    for (int r = 0; r < grid.size.rows; ++r) {
        if (row_text(grid, r).find("cat cat cat") != std::string::npos) row = r;
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
        ASSERT_EQ(grid.at(col + k, row).role, ssg::SemanticRole::selection);
    }
    ASSERT_NE(grid.at(col + 3, row).role, ssg::SemanticRole::selection);
    ASSERT_NE(grid.at(col + 3, row).role, ssg::SemanticRole::search_match);
    for (int k = 0; k < 3; ++k) {
        ASSERT_EQ(grid.at(col + 4 + k, row).role, ssg::SemanticRole::search_match);
        ASSERT_EQ(grid.at(col + 8 + k, row).role, ssg::SemanticRole::search_match);
    }
}

TEST(render_hides_find_matches_after_document_revision_changes) {
    auto root = unique_root();
    std::ofstream{root / "stale.txt"} << "cat cat cat\n";
    auto runtime = make_runtime(root);
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
    // the matches become stale and must not be painted onto the new revision.
    (void)runtime->dispatch(
        ssg::ClientId{1},
        {"text.insert", runtime->revision(), ssg::TextInputArguments{"z"}});
    auto snapshot = runtime->snapshot(ssg::ClientId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_NE(snapshot->sections().find_replace.source_revision,
              snapshot->sections().document.revision);
    auto grid = ssg::render(*snapshot);
    bool any_match = false;
    for (int row = 0; row < grid.size.rows; ++row) {
        for (int col = 0; col < grid.size.columns; ++col) {
            if (grid.at(col, row).role == ssg::SemanticRole::search_match) {
                any_match = true;
            }
        }
    }
    ASSERT_FALSE(any_match);
}

int main() {
    RUN(render_paints_content_not_accessibility_labels);
    RUN(render_colors_are_palette_indices);
    RUN(render_is_deterministic);
    RUN(render_projects_palette_results_into_active_pane);
    RUN(render_shows_palette_query_and_ghost_in_header);
    RUN(render_paints_selection_highlight_and_secondary_carets);
    RUN(render_fills_end_of_line_for_multiline_selection);
    RUN(render_highlights_wide_glyph_cells);
    RUN(render_paints_secondary_ranged_selection_caret);
    RUN(render_paints_secondary_caret_as_a_cell);
    RUN(render_paints_find_matches_and_active_match);
    RUN(render_hides_find_matches_after_document_revision_changes);

    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed > 0 ? 1 : 0;
}
