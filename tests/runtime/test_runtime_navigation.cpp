#include "../test_helpers.h"

#include <ssg/editor_runtime.h>
#include <ssg/layout.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::filesystem::path unique_root() {
    auto root = std::filesystem::current_path() / "runtime_navigation";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    std::ofstream{root / "workspace" / "needle.txt"} << "alpha needle omega";
    return root;
}

TEST(search_tree_diff_and_follow_sections_use_runtime_state) {
    auto root = unique_root();
    auto created = ssg::EditorRuntime::create({root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::in_process}, ssg::ViewId{1}).accepted());

    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"search.workspace", runtime.revision(), std::string{"needle"}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"follow_edits.pause", runtime.revision(), {}}).accepted());
    auto snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_FALSE(snapshot->sections().search.results.empty());
    ASSERT_FALSE(snapshot->sections().tree.providers.empty());
    ASSERT_EQ(snapshot->sections().follow_edits.mode, ssg::FollowMode::paused);
}

TEST(palette_open_enters_prompt_focus_and_publishes_candidates) {
    auto root = unique_root();
    auto created = ssg::EditorRuntime::create({root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::in_process}, ssg::ViewId{1}).accepted());

    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"palette.open", runtime.revision(), {}}).accepted());
    auto snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_EQ(snapshot->sections().shell.focus, ssg::FocusTarget::prompt);
    ASSERT_FALSE(snapshot->sections().palette.candidates.empty());

    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"palette.close", runtime.revision(), {}}).accepted());
    auto closed = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(closed.has_value());
    if (!closed) return;
    ASSERT_EQ(closed->sections().shell.focus, ssg::FocusTarget::editor);
}

TEST(palette_execute_validates_candidate_membership) {
    auto root = unique_root();
    auto created = ssg::EditorRuntime::create({root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::in_process}, ssg::ViewId{1}).accepted());

    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"needle.txt"}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"palette.open", runtime.revision(), {}}).accepted());

    // A command outside the published candidate set is rejected before dispatch.
    ASSERT_FALSE(runtime.dispatch(ssg::ClientId{1}, {"palette.execute", runtime.revision(), ssg::PaletteExecuteArguments{"not.a.command"}}).accepted());
    // Missing the id payload is rejected.
    ASSERT_FALSE(runtime.dispatch(ssg::ClientId{1}, {"palette.execute", runtime.revision(), {}}).accepted());
    // A published command id validates, executes server-side, and closes the palette.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"palette.execute", runtime.revision(), ssg::PaletteExecuteArguments{"file.save"}}).accepted());
    auto snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    ASSERT_EQ(snapshot->sections().shell.focus, ssg::FocusTarget::editor);
}

TEST(palette_candidates_carry_labels_and_key_detail) {
    auto root = unique_root();
    auto created = ssg::EditorRuntime::create({root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::in_process}, ssg::ViewId{1}).accepted());
    auto snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;

    const auto& candidates = snapshot->sections().palette.candidates;
    ASSERT_FALSE(candidates.empty());

    const ssg::PaletteCandidate* save = nullptr;
    const ssg::PaletteCandidate* undo = nullptr;
    for (const auto& candidate : candidates) {
        // Every candidate carries a human label, never the raw dotted id.
        ASSERT_NE(candidate.label, candidate.id);
        ASSERT_FALSE(candidate.label.empty());
        if (candidate.id == "file.save") save = &candidate;
        if (candidate.id == "edit.undo") undo = &candidate;
    }
    ASSERT_TRUE(save != nullptr);
    ASSERT_TRUE(undo != nullptr);
    if (save) {
        ASSERT_EQ(save->label, std::string{"Save File"});
        ASSERT_EQ(save->detail, std::string{"Esc S"});  // Its bound chord.
    }
    if (undo) {
        ASSERT_EQ(undo->detail, std::string{"Esc Z"});
    }

    // An unbound command shows a label but no key detail.
    const ssg::PaletteCandidate* unbound = nullptr;
    for (const auto& candidate : candidates) {
        if (candidate.id == "edit.sort_lines") unbound = &candidate;
    }
    ASSERT_TRUE(unbound != nullptr);
    if (unbound) ASSERT_TRUE(unbound->detail.empty());
}

TEST(tree_scrolls_to_keep_selection_visible_in_a_short_panel) {
    auto root = std::filesystem::current_path() / "runtime_nav_treescroll";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    // 40 top-level files -> a tree far taller than a short panel.
    for (int i = 0; i < 40; ++i) {
        char name[32];
        std::snprintf(name, sizeof name, "file-%02d.txt", i);
        std::ofstream{root / "workspace" / name} << "x";
    }
    auto created = ssg::EditorRuntime::create({root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::in_process}, ssg::ViewId{1}).accepted());
    // Show the panel; a 12-row terminal gives a panel content height of ~9.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"panel.toggle", runtime.revision(), {}}).accepted());
    // Select the workspace root and expand it so its 40 files become visible.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"tree.select_next", runtime.revision(), {}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"tree.activate", runtime.revision(), {}}).accepted());
    const ssg::ViewportDimensions dims{80, 12};

    // Baseline: selection at the top (root), window pinned to the top with a live
    // thumb.
    {
        auto snap = runtime.snapshot(ssg::ClientId{1}, dims);
        ASSERT_TRUE(snap.has_value());
        if (!snap) return;
        auto const& p = snap->sections().tree.providers.front();
        ASSERT_TRUE(p.nodes.size() >= 40);
        ASSERT_EQ(p.first_visible, std::uint32_t{0});
        ASSERT_TRUE(p.scrollbar.maximum_first_row > 0);          // scrollable
        ASSERT_TRUE(p.scrollbar.thumb_size < p.scrollbar.viewport_rows);
        ASSERT_EQ(p.visible_node_ids.size(),
                  std::size_t{p.scrollbar.viewport_rows});       // window bound
        ASSERT_EQ(p.visible_node_ids.front(), p.nodes.front().node.id);
    }

    // Move the selection to the bottom: the window scrolls to keep it shown.
    for (int i = 0; i < 60; ++i) {
        ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"tree.select_next", runtime.revision(), {}}).accepted());
    }
    std::uint32_t deep_first = 0;
    {
        auto snap = runtime.snapshot(ssg::ClientId{1}, dims);
        ASSERT_TRUE(snap.has_value());
        if (!snap) return;
        auto const& p = snap->sections().tree.providers.front();
        ASSERT_TRUE(p.selected.has_value());
        // The selected node's absolute index lies within the visible window.
        std::optional<std::uint32_t> sel_index;
        for (std::uint32_t i = 0; i < p.nodes.size(); ++i) {
            if (p.nodes[i].node.id == *p.selected) { sel_index = i; break; }
        }
        ASSERT_TRUE(sel_index.has_value());
        ASSERT_TRUE(p.first_visible > 0);
        ASSERT_TRUE(*sel_index >= p.first_visible &&
                    *sel_index < p.first_visible + p.visible_node_ids.size());
        // The hit map maps each viewport row to the correct on-screen node id.
        for (std::size_t row = 0; row < p.visible_node_ids.size(); ++row) {
            ASSERT_EQ(p.visible_node_ids[row],
                      p.nodes[p.first_visible + row].node.id);
        }
        deep_first = p.first_visible;
    }
    ASSERT_TRUE(deep_first > 0);

    // Move back up to the top: the window scrolls back to first_visible == 0.
    for (int i = 0; i < 40; ++i) {
        ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"tree.select_previous", runtime.revision(), {}}).accepted());
    }
    {
        auto snap = runtime.snapshot(ssg::ClientId{1}, dims);
        ASSERT_TRUE(snap.has_value());
        if (!snap) return;
        ASSERT_EQ(snap->sections().tree.providers.front().first_visible, std::uint32_t{0});
    }
    std::filesystem::remove_all(root);
}

TEST(tree_select_sets_selection_to_a_node_and_rejects_unknown_ids) {
    auto root = std::filesystem::current_path() / "runtime_nav_treeselect";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    for (int i = 0; i < 6; ++i) {
        char name[32];
        std::snprintf(name, sizeof name, "file-%02d.txt", i);
        std::ofstream{root / "workspace" / name} << "x";
    }
    auto created = ssg::EditorRuntime::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::in_process},
                               ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"panel.toggle", runtime.revision(), {}}).accepted());
    // Expand the workspace root so its files become visible/selectable nodes.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"tree.select_next", runtime.revision(), {}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"tree.activate", runtime.revision(), {}}).accepted());

    const ssg::ViewportDimensions dims{80, 24};
    auto snap = runtime.snapshot(ssg::ClientId{1}, dims);
    ASSERT_TRUE(snap.has_value());
    if (!snap) return;
    auto const& nodes = snap->sections().tree.providers.front().nodes;
    ASSERT_TRUE(nodes.size() >= 4);
    if (nodes.size() < 4) return;
    // Pick a node that is NOT already selected (the third visible node).
    auto const target = nodes[2].node.id;

    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"tree.select", runtime.revision(),
                                  ssg::TreeSelectArguments{target}}).accepted());
    auto after = runtime.snapshot(ssg::ClientId{1}, dims);
    ASSERT_TRUE(after.has_value());
    if (!after) return;
    ASSERT_TRUE(after->sections().tree.providers.front().selected.has_value());
    ASSERT_EQ(*after->sections().tree.providers.front().selected, target);

    // An id absent from the active provider is rejected; a missing payload too.
    ASSERT_FALSE(runtime.dispatch(ssg::ClientId{1},
                                  {"tree.select", runtime.revision(),
                                   ssg::TreeSelectArguments{ssg::TreeNodeId{"nope"}}}).accepted());
    ASSERT_FALSE(runtime.dispatch(ssg::ClientId{1},
                                  {"tree.select", runtime.revision(), {}}).accepted());
    // The selection is unchanged after the rejected attempts.
    auto again = runtime.snapshot(ssg::ClientId{1}, dims);
    ASSERT_TRUE(again.has_value());
    if (!again) return;
    ASSERT_EQ(*again->sections().tree.providers.front().selected, target);
    std::filesystem::remove_all(root);
}

TEST(tree_select_focuses_the_panel_and_the_click_pair_nets_expected_focus) {
    auto root = std::filesystem::current_path() / "runtime_nav_treefocus";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace" / "dir");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    std::ofstream{root / "workspace" / "dir" / "inner.txt"} << "x";
    std::ofstream{root / "workspace" / "top.txt"} << "hello";
    auto created = ssg::EditorRuntime::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::in_process},
                               ssg::ViewId{1}).accepted());
    const ssg::ViewportDimensions dims{80, 24};
    auto focus = [&] {
        auto snap = runtime.snapshot(ssg::ClientId{1}, dims);
        return snap ? snap->sections().shell.focus : ssg::FocusTarget::editor;
    };
    // Showing the panel now focuses it (QOL); expand the root so a directory node
    // and a file node are both visible/selectable.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"panel.toggle", runtime.revision(), {}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"tree.select_next", runtime.revision(), {}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"tree.activate", runtime.revision(), {}}).accepted());
    ASSERT_EQ(focus(), ssg::FocusTarget::panel);

    auto snap = runtime.snapshot(ssg::ClientId{1}, dims);
    ASSERT_TRUE(snap.has_value());
    if (!snap) return;
    std::optional<ssg::TreeNodeId> dir_id;
    std::optional<ssg::TreeNodeId> file_id;
    for (auto const& view : snap->sections().tree.providers.front().nodes) {
        if (view.node.expandable && !dir_id) dir_id = view.node.id;
        if (!view.node.expandable && view.node.workspace_path && !file_id) file_id = view.node.id;
    }
    ASSERT_TRUE(dir_id.has_value());
    ASSERT_TRUE(file_id.has_value());
    if (!dir_id || !file_id) return;

    // The file click pair [tree.select, tree.activate] ends on the editor (the
    // file opens, so tree.activate's focus_editor wins over tree.select's panel).
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"tree.select", runtime.revision(), ssg::TreeSelectArguments{*file_id}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"tree.activate", runtime.revision(), {}}).accepted());
    ASSERT_EQ(focus(), ssg::FocusTarget::editor);

    // From editor focus, tree.select alone moves keyboard focus to the panel.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"tree.select", runtime.revision(), ssg::TreeSelectArguments{*file_id}}).accepted());
    ASSERT_EQ(focus(), ssg::FocusTarget::panel);

    // The directory click pair ends on the panel (tree.select focuses the panel,
    // tree.activate toggles the directory and leaves focus alone).
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"tree.select", runtime.revision(), ssg::TreeSelectArguments{*dir_id}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"tree.activate", runtime.revision(), {}}).accepted());
    ASSERT_EQ(focus(), ssg::FocusTarget::panel);
    std::filesystem::remove_all(root);
}

TEST(tree_scroll_moves_the_viewport_without_moving_the_selection) {
    auto root = std::filesystem::current_path() / "runtime_nav_treescroll_wheel";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    for (int i = 0; i < 40; ++i) {
        char name[32];
        std::snprintf(name, sizeof name, "file-%02d.txt", i);
        std::ofstream{root / "workspace" / name} << "x";
    }
    auto created = ssg::EditorRuntime::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::in_process},
                               ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"panel.toggle", runtime.revision(), {}}).accepted());
    // Expand the root so the 40 files become a tree taller than a short panel.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"tree.select_next", runtime.revision(), {}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"tree.activate", runtime.revision(), {}}).accepted());
    const ssg::ViewportDimensions dims{80, 12};

    auto baseline = runtime.snapshot(ssg::ClientId{1}, dims);
    ASSERT_TRUE(baseline.has_value());
    if (!baseline) return;
    auto const& p0 = baseline->sections().tree.providers.front();
    ASSERT_EQ(p0.first_visible, std::uint32_t{0});
    ASSERT_TRUE(p0.scrollbar.maximum_first_row > 0);
    auto const selected_before = p0.selected;

    // Wheel down: the viewport offset advances, but the selection does not move.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"tree.scroll", runtime.revision(),
                                  ssg::ScrollLinesArguments{3}}).accepted());
    auto scrolled = runtime.snapshot(ssg::ClientId{1}, dims);
    ASSERT_TRUE(scrolled.has_value());
    if (!scrolled) return;
    auto const& p1 = scrolled->sections().tree.providers.front();
    ASSERT_EQ(p1.first_visible, std::uint32_t{3});
    ASSERT_EQ(p1.selected, selected_before);  // selection unchanged

    // Wheel up past the top clamps at 0.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"tree.scroll", runtime.revision(),
                                  ssg::ScrollLinesArguments{-99}}).accepted());
    auto topped = runtime.snapshot(ssg::ClientId{1}, dims);
    ASSERT_TRUE(topped.has_value());
    if (!topped) return;
    ASSERT_EQ(topped->sections().tree.providers.front().first_visible, std::uint32_t{0});

    // Wheel down past the bottom clamps at maximum_first_row.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"tree.scroll", runtime.revision(),
                                  ssg::ScrollLinesArguments{999}}).accepted());
    auto bottomed = runtime.snapshot(ssg::ClientId{1}, dims);
    ASSERT_TRUE(bottomed.has_value());
    if (!bottomed) return;
    auto const& p3 = bottomed->sections().tree.providers.front();
    ASSERT_EQ(p3.first_visible, p3.scrollbar.maximum_first_row);

    // A missing payload is rejected.
    ASSERT_FALSE(runtime.dispatch(ssg::ClientId{1},
                                  {"tree.scroll", runtime.revision(), {}}).accepted());
    std::filesystem::remove_all(root);
}

} // namespace

namespace {

// M12 VP-H: word-wrap-off horizontal caret reveal. A long line whose caret moves
// past the pane width scrolls horizontally so the caret stays visible; returning
// to the line start resets the offset. A short (fitting) line never scrolls.
TEST(word_wrap_off_reveals_caret_horizontally) {
    auto root = std::filesystem::current_path() / "runtime_hscroll";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    // A single 60-cell line, far wider than the test pane.
    std::ofstream{root / "workspace" / "long.txt"} << std::string(60, 'a') << "\n";
    auto created = ssg::EditorRuntime::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::in_process},
                               ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"file.open", runtime.revision(),
                                  std::string{"long.txt"}}).accepted());

    ssg::ViewportDimensions const dims{24, 6};
    // Prime the pane-size cache the reveal path reads.
    auto primed = runtime.snapshot(ssg::ClientId{1}, dims);
    ASSERT_TRUE(primed.has_value());
    if (!primed) return;
    ASSERT_EQ(primed->client().viewport.first_visual_column, std::uint32_t{0});

    // Move the caret to the end of the long line: it is past the pane width, so
    // the viewport scrolls horizontally to keep it visible.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"cursor.line_end", runtime.revision(), {}})
                    .accepted());
    auto scrolled = runtime.snapshot(ssg::ClientId{1}, dims);
    ASSERT_TRUE(scrolled.has_value());
    if (!scrolled) return;
    auto const offset = scrolled->client().viewport.first_visual_column;
    ASSERT_TRUE(offset > 0);
    // The caret's cell (60) is within the visible horizontal window.
    ASSERT_TRUE(60u >= offset);
    ASSERT_TRUE(60u < offset + dims.columns);

    // Returning to the line start resets the horizontal offset to zero.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"cursor.line_start", runtime.revision(), {}})
                    .accepted());
    auto reset = runtime.snapshot(ssg::ClientId{1}, dims);
    ASSERT_TRUE(reset.has_value());
    if (!reset) return;
    ASSERT_EQ(reset->client().viewport.first_visual_column, std::uint32_t{0});
    std::filesystem::remove_all(root);
}

// VP-3 regression: the word-wrap-ON path stays EXACT — a long line still wraps to
// multiple visual rows through the runtime — while word-wrap-OFF clips it to one
// row and scrolls horizontally.  Locks both directions of the wrap gate so the
// M12 projection can never silently disable wrapping.
TEST(word_wrap_on_wraps_long_lines_off_clips_them) {
    auto root = std::filesystem::current_path() / "runtime_wrap_gate";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    // One 200-cell line (far wider than the 80-col pane) plus a short line.
    std::ofstream{root / "workspace" / "wide.txt"}
        << std::string(200, 'b') << "\nshort\n";
    auto created = ssg::EditorRuntime::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::in_process},
                               ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"file.open", runtime.revision(),
                                  std::string{"wide.txt"}}).accepted());
    ssg::ViewportDimensions const dims{80, 24};

    // Word wrap OFF (default): three logical lines (the trailing newline yields a
    // final empty line) -> three visual rows total; the 200-cell line is ONE
    // clipped visual row.
    auto off = runtime.snapshot(ssg::ClientId{1}, dims);
    ASSERT_TRUE(off.has_value());
    if (!off) return;
    ASSERT_EQ(off->client().viewport.total_visual_rows, std::uint32_t{3});
    std::uint32_t off_rows_for_line0 = 0;
    for (auto const& row : off->client().viewport.visible_rows) {
        if (row.logical_line == 0) ++off_rows_for_line0;
    }
    ASSERT_EQ(off_rows_for_line0, std::uint32_t{1});  // clipped, not wrapped

    // Word wrap ON: the 200-cell line wraps into ceil(200/80) = 3 visual rows, so
    // the total exceeds the OFF total and logical line 0 spans >1 row.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"view.toggle_word_wrap", runtime.revision(), {}})
                    .accepted());
    auto on = runtime.snapshot(ssg::ClientId{1}, dims);
    ASSERT_TRUE(on.has_value());
    if (!on) return;
    ASSERT_TRUE(on->client().viewport.total_visual_rows > 3u);  // wrapped
    std::uint32_t on_rows_for_line0 = 0;
    for (auto const& row : on->client().viewport.visible_rows) {
        if (row.logical_line == 0) ++on_rows_for_line0;
    }
    ASSERT_EQ(on_rows_for_line0, std::uint32_t{3});  // 200 cells / 80 -> 3 rows
    // Wrapped lines never scroll horizontally.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"cursor.line_end", runtime.revision(), {}})
                    .accepted());
    auto wrapped_end = runtime.snapshot(ssg::ClientId{1}, dims);
    ASSERT_TRUE(wrapped_end.has_value());
    if (!wrapped_end) return;
    ASSERT_EQ(wrapped_end->client().viewport.first_visual_column, std::uint32_t{0});
    std::filesystem::remove_all(root);
}

// M12 VP-2b (INV-viewport-bounded-work): with word wrap OFF, a caret navigation
// segments only the visible + moved lines — bounded and INDEPENDENT of document
// length — not the whole document. Proven by the compute_cell_run counter: the
// per-move segmentation count is identical for a 50-line and a 20000-line file.
TEST(word_wrap_off_navigation_is_viewport_bounded) {
    auto root = std::filesystem::current_path() / "runtime_navbound";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    auto make_doc = [](std::size_t lines) {
        std::string text;
        for (std::size_t i = 0; i < lines; ++i) {
            text += "line " + std::to_string(i) + " content\n";
        }
        return text;
    };
    std::ofstream{root / "workspace" / "small.txt"} << make_doc(50);
    std::ofstream{root / "workspace" / "big.txt"} << make_doc(20000);
    auto created = ssg::EditorRuntime::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::in_process},
                               ssg::ViewId{1}).accepted());
    ssg::ViewportDimensions const dims{80, 24};

    auto nav_segmentations = [&](std::string const& file) -> std::uint64_t {
        (void)runtime.dispatch(
            ssg::ClientId{1}, {"file.open", runtime.revision(), file});
        (void)runtime.snapshot(ssg::ClientId{1}, dims);  // prime pane cache
        ssg::reset_cell_run_calls();
        for (int i = 0; i < 4; ++i) {
            (void)runtime.dispatch(
                ssg::ClientId{1},
                {"cursor.line_down", runtime.revision(), {}});
        }
        return ssg::cell_run_calls();
    };

    auto const small_calls = nav_segmentations("small.txt");
    auto const big_calls = nav_segmentations("big.txt");

    ASSERT_TRUE(small_calls > 0);
    // Bounded (~ per move: visible rows + the moved line), and NOT proportional to
    // the 400x-larger document.
    ASSERT_TRUE(small_calls < 200);
    ASSERT_EQ(small_calls, big_calls);
    std::filesystem::remove_all(root);
}

} // namespace

int main() {
    RUN(search_tree_diff_and_follow_sections_use_runtime_state);
    RUN(palette_open_enters_prompt_focus_and_publishes_candidates);
    RUN(palette_execute_validates_candidate_membership);
    RUN(palette_candidates_carry_labels_and_key_detail);
    RUN(tree_scrolls_to_keep_selection_visible_in_a_short_panel);
    RUN(tree_select_sets_selection_to_a_node_and_rejects_unknown_ids);
    RUN(tree_scroll_moves_the_viewport_without_moving_the_selection);
    RUN(tree_select_focuses_the_panel_and_the_click_pair_nets_expected_focus);
    RUN(word_wrap_off_reveals_caret_horizontally);
    RUN(word_wrap_on_wraps_long_lines_off_clips_them);
    RUN(word_wrap_off_navigation_is_viewport_bounded);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
