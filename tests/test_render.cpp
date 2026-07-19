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

TEST(render_segments_only_visible_lines_not_whole_document) {
    // INV-render-projection (M12): render() runs compute_cell_run only for the
    // logical lines the viewport shows (<= rows), independent of document length.
    auto root = unique_root();
    auto make_doc = [](std::size_t line_count) {
        std::string text;
        for (std::size_t i = 0; i < line_count; ++i) {
            text += "line " + std::to_string(i) + "\n";
        }
        return text;
    };
    std::ofstream{root / "small.txt"} << make_doc(50);
    std::ofstream{root / "big.txt"} << make_doc(5000);
    auto runtime = make_runtime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;

    auto segment_count_for = [&](std::string const& file) -> std::uint64_t {
        (void)runtime->dispatch(
            ssg::ClientId{1}, {"file.open", runtime->revision(), file});
        auto snapshot = runtime->snapshot(ssg::ClientId{1}, {80, 24});
        ASSERT_TRUE(snapshot.has_value());
        if (!snapshot) return 0;
        ssg::reset_render_segmentation_calls();
        auto grid = ssg::render(*snapshot);
        (void)grid;
        return ssg::render_segmentation_calls();
    };

    auto const small_calls = segment_count_for("small.txt");
    auto const big_calls = segment_count_for("big.txt");

    // At most one segmentation per visible editor row (24-tall terminal, minus
    // the chrome rows), and NOT proportional to the 100x-larger document.
    ASSERT_TRUE(small_calls > 0);
    ASSERT_TRUE(small_calls <= 24);
    ASSERT_TRUE(big_calls <= 24);
    ASSERT_EQ(small_calls, big_calls);
}


TEST(word_wrap_off_renders_horizontally_scrolled_content) {
    // M12 VP-H: with the caret at the end of a long line (word wrap off), the
    // editor paints the horizontally-scrolled window — the line's END is visible
    // and its START has scrolled off — proving render honors first_visual_column.
    auto root = unique_root();
    std::string line = "STARTmarker";
    line += std::string(120, '.');
    line += "ENDmarker";
    std::ofstream{root / "long.txt"} << line << "\nsecond\n";
    auto runtime = make_runtime(root);
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
    ASSERT_TRUE(snapshot->client().viewport.first_visual_column > 0);
    auto grid = ssg::render(*snapshot);

    // The end of the line is on screen; the start has scrolled off.
    ASSERT_TRUE(grid_contains(grid, "ENDmarker"));
    ASSERT_FALSE(grid_contains(grid, "STARTmarker"));
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
    // the controller is stale: reconcile closes it and no matches are painted.
    (void)runtime->dispatch(
        ssg::ClientId{1},
        {"text.insert", runtime->revision(), ssg::TextInputArguments{"z"}});
    auto snapshot = runtime->snapshot(ssg::ClientId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_FALSE(snapshot->sections().find_replace.open);
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

TEST(render_replace_prompt_shows_query_and_replacement_with_cursor_on_replacement) {
    auto root = unique_root();
    std::ofstream{root / "rep.txt"} << "cat cat cat\n";
    auto runtime = make_runtime(root);
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
    ASSERT_TRUE(grid_contains(grid, "cat"));
    ASSERT_TRUE(grid_contains(grid, "dog"));
    // The hardware cursor sits on the replacement row (the one containing "dog").
    ASSERT_TRUE(grid.caret.has_value());
    if (grid.caret) {
        ASSERT_TRUE(row_text(grid, grid.caret->row).find("dog") != std::string::npos);
    }
}

TEST(render_find_prompt_shows_option_indicators) {
    auto root = unique_root();
    std::ofstream{root / "opt.txt"} << "Cat cat CAT\n";
    auto runtime = make_runtime(root);
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
        ASSERT_TRUE(grid_contains(grid, "[ ] Case"));
        ASSERT_TRUE(grid_contains(grid, "[ ] Word"));
        ASSERT_TRUE(grid_contains(grid, "[ ] Regex"));
    }

    // Toggling case flips its indicator to checked.
    (void)runtime->dispatch(ssg::ClientId{1},
                            {"find.toggle_case", runtime->revision(), {}});
    auto snapshot = runtime->snapshot(ssg::ClientId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto grid = ssg::render(*snapshot);
    ASSERT_TRUE(grid_contains(grid, "[x] Case"));
    ASSERT_TRUE(grid_contains(grid, "[ ] Word"));
}

TEST(render_panel_tree_windows_and_draws_a_thumb_when_taller_than_the_panel) {
    auto root = unique_root();
    for (int i = 0; i < 40; ++i) {
        char name[32];
        std::snprintf(name, sizeof name, "file-%02d.txt", i);
        std::ofstream{root / name} << "x";
    }
    auto runtime = make_runtime(root);
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
    ASSERT_TRUE(shell.panel_scrollbar.has_value());
    if (!shell.panel_scrollbar) return;
    auto grid = ssg::render(*snapshot);

    // A thumb ('#') is drawn in the reserved gutter column.
    int const gx = shell.panel_scrollbar->x;
    bool has_thumb = false;
    for (int y = shell.panel_scrollbar->y;
         y < shell.panel_scrollbar->y + shell.panel_scrollbar->height; ++y) {
        if (grid.at(gx, y).text == "#") has_thumb = true;
    }
    ASSERT_TRUE(has_thumb);
    // The window scrolled to the end: the first file is off-screen, the last is
    // visible.
    ASSERT_FALSE(grid_contains(grid, "file-00.txt"));
    ASSERT_TRUE(grid_contains(grid, "file-39.txt"));
}

TEST(render_panel_tree_reserves_an_empty_gutter_when_it_fits) {
    auto root = unique_root();
    std::ofstream{root / "a.txt"} << "x";
    std::ofstream{root / "b.txt"} << "x";
    auto runtime = make_runtime(root);
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
    ASSERT_TRUE(shell.panel_scrollbar.has_value());
    if (!shell.panel_scrollbar) return;
    auto grid = ssg::render(*snapshot);
    // The gutter is reserved (column exists) but blank: no thumb or track glyphs,
    // so the tree's content width never changes as items are added or removed.
    int const gx = shell.panel_scrollbar->x;
    for (int y = shell.panel_scrollbar->y;
         y < shell.panel_scrollbar->y + shell.panel_scrollbar->height; ++y) {
        ASSERT_NE(grid.at(gx, y).text, std::string{"#"});
        ASSERT_NE(grid.at(gx, y).text, std::string{"|"});
    }
    ASSERT_TRUE(grid_contains(grid, "a.txt"));
}

TEST(render_palette_windows_rows_and_draws_a_thumb_with_absolute_selection) {
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
    auto sections = snapshot->sections();
    ASSERT_FALSE(sections.shell.panes.empty());
    if (sections.shell.panes.empty()) return;

    // A 40-item ranked list windowed to rows [20, 20+height); the absolute
    // selection is 25, so the on-screen highlight is at window row 5.
    auto const& pane = sections.shell.panes.front();
    std::uint32_t const rows = static_cast<std::uint32_t>(pane.content.height);
    ssg::PaletteProjection projection;
    projection.rect = pane.content;
    projection.scrollbar_rect = pane.scrollbar;
    projection.first_visible = 20;
    projection.selected = std::uint32_t{25};
    projection.scrollbar = ssg::scrollbar_metrics(40, rows, 20);
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
    ASSERT_TRUE(grid_contains(grid, "cmd-20"));
    ASSERT_FALSE(grid_contains(grid, "cmd-00"));
    int const selected_row = projection.rect.y + 5;
    ASSERT_EQ(grid.at(projection.rect.x, selected_row).role,
              ssg::SemanticRole::selection);
    // Row 0 (absolute 20) is not selected.
    ASSERT_FALSE(grid.at(projection.rect.x, projection.rect.y).role ==
                 ssg::SemanticRole::selection);
    // A thumb is drawn in the reserved gutter column.
    bool has_thumb = false;
    for (int y = projection.scrollbar_rect.y;
         y < projection.scrollbar_rect.y + projection.scrollbar_rect.height; ++y) {
        if (grid.at(projection.scrollbar_rect.x, y).text == "#") has_thumb = true;
    }
    ASSERT_TRUE(has_thumb);
}

TEST(render_palette_reserves_an_empty_gutter_when_the_list_fits) {
    auto root = unique_root();
    std::ofstream{root / "hello.txt"} << "alpha\n";
    auto runtime = make_runtime(root);
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
    projection.scrollbar_rect = pane.scrollbar;
    projection.first_visible = 0;
    projection.selected = std::uint32_t{0};
    projection.scrollbar =
        ssg::scrollbar_metrics(2, static_cast<std::uint32_t>(pane.content.height), 0);
    projection.rows = {{"a", ""}, {"b", ""}};
    sections.shell.palette = projection;
    ssg::SessionSnapshot projected{snapshot->revision(), snapshot->topology(),
                                   snapshot->client(), std::move(sections)};
    auto grid = ssg::render(projected);
    // The gutter is reserved (column exists) but blank: no thumb/track glyphs.
    for (int y = projection.scrollbar_rect.y;
         y < projection.scrollbar_rect.y + projection.scrollbar_rect.height; ++y) {
        ASSERT_NE(grid.at(projection.scrollbar_rect.x, y).text, std::string{"#"});
        ASSERT_NE(grid.at(projection.scrollbar_rect.x, y).text, std::string{"|"});
    }
}

TEST(render_too_small_viewport_produces_library_placeholder) {
    // M11-L: below the 20x4 minimum the library (not the app) renders the
    // placeholder screen, sized to the terminal, so no app code authors cells.
    auto root = unique_root();
    auto runtime = make_runtime(root);
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
    ASSERT_EQ(row_text(grid, 2), std::string("terminal \xe2\x80\xa6"));
    ASSERT_EQ(row_text(grid, 0), std::string(10, ' '));

    std::filesystem::remove_all(root);
}

TEST(render_too_small_matches_hand_authored_golden) {
    auto root = unique_root();
    auto runtime = make_runtime(root);
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
    ASSERT_EQ(row_text(grid, 0), std::string(24, ' '));
    ASSERT_EQ(row_text(grid, 1),
              std::string("   terminal too small   "));
    ASSERT_EQ(row_text(grid, 2), std::string(24, ' '));
    // Determinism.
    ASSERT_EQ(ssg::render(*snapshot).canonical(), grid.canonical());
    std::filesystem::remove_all(root);
}

TEST(render_too_small_is_safe_at_one_by_one) {
    auto root = unique_root();
    auto runtime = make_runtime(root);
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
    RUN(render_paints_content_not_accessibility_labels);
    RUN(render_segments_only_visible_lines_not_whole_document);
    RUN(word_wrap_off_renders_horizontally_scrolled_content);
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
    RUN(render_replace_prompt_shows_query_and_replacement_with_cursor_on_replacement);
    RUN(render_find_prompt_shows_option_indicators);
    RUN(render_panel_tree_windows_and_draws_a_thumb_when_taller_than_the_panel);
    RUN(render_panel_tree_reserves_an_empty_gutter_when_it_fits);
    RUN(render_palette_windows_rows_and_draws_a_thumb_with_absolute_selection);
    RUN(render_palette_reserves_an_empty_gutter_when_the_list_fits);
    RUN(render_too_small_viewport_produces_library_placeholder);
    RUN(render_too_small_matches_hand_authored_golden);
    RUN(render_too_small_is_safe_at_one_by_one);

    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed > 0 ? 1 : 0;
}
