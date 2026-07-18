#include "../test_helpers.h"

#include <ssg/editor_runtime.h>
#include <ssg/find_replace.h>
#include <ssg/input.h>
#include <ssg/selection.h>
#include <ssg/prompt.h>
#include <ssg/text_input_commands.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace {

std::filesystem::path unique_root() {
    auto root = std::filesystem::current_path() / "runtime_editing";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    std::ofstream{root / "workspace" / "edit.txt"} << "abc";
    return root;
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

class DiskPreviewWorkspace final : public ssg::FindReplaceWorkspace {
public:
    DiskPreviewWorkspace(std::filesystem::path root, ssg::Revision revision)
        : root_{std::move(root)}, revision_{revision} {}

    ssg::WorkspaceSnapshot snapshot(ssg::Revision revision) const override {
        ssg::WorkspaceSnapshot snapshot;
        snapshot.revision = revision_;
        if (revision != revision_) return snapshot;
        for (auto const& entry : std::filesystem::recursive_directory_iterator{root_}) {
            if (!entry.is_regular_file()) continue;
            auto relative = entry.path().lexically_relative(root_).generic_string();
            snapshot.files.push_back({relative, read_text(entry.path())});
        }
        return snapshot;
    }

    ssg::WorkspaceApplyResult apply(const ssg::WorkspaceReplacePreview&, ssg::WorkspaceRecoverySink&) override {
        return {ssg::FindReplaceError::workspace_rejected, revision_, "not used"};
    }

    ssg::WorkspaceApplyResult recover(const ssg::WorkspaceRecoveryRecord&) override {
        return {ssg::FindReplaceError::workspace_rejected, revision_, "not used"};
    }

private:
    std::filesystem::path root_;
    ssg::Revision revision_;
};

TEST(runtime_text_selection_and_history_match_feature_operations) {
    auto root = unique_root();
    auto created = ssg::EditorRuntime::create({root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::in_process}, ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"edit.txt"}}).accepted());

    auto set_position = runtime.dispatch(ssg::ClientId{1}, {"cursor.set_position", runtime.revision(), ssg::SelectionCommandArguments{ssg::resolve_document_position("abc", ssg::ByteOffset{3}), std::nullopt}});
    ASSERT_TRUE(set_position.accepted());
    auto typed = runtime.dispatch(ssg::ClientId{1}, {"text.insert", runtime.revision(), ssg::TextInputArguments{"d"}});
    ASSERT_TRUE(typed.accepted());
    ASSERT_EQ(runtime.active_document_text(), std::string{"abcd"});

    auto undo = runtime.dispatch(ssg::ClientId{1}, {"edit.undo", runtime.revision(), {}});
    ASSERT_TRUE(undo.accepted());
    ASSERT_EQ(runtime.active_document_text(), std::string{"abc"});
    auto redo = runtime.dispatch(ssg::ClientId{1}, {"edit.redo", runtime.revision(), {}});
    ASSERT_TRUE(redo.accepted());
    ASSERT_EQ(runtime.active_document_text(), std::string{"abcd"});
}

TEST(typing_undo_breaks_on_word_and_line_boundaries) {
    auto root = unique_root();
    auto created = ssg::EditorRuntime::create({root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::in_process}, ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"edit.txt"}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"cursor.set_position", runtime.revision(), ssg::SelectionCommandArguments{ssg::resolve_document_position("abc", ssg::ByteOffset{3}), std::nullopt}}).accepted());

    const auto type = [&](char character) {
        return runtime.dispatch(ssg::ClientId{1}, {"text.insert", runtime.revision(), ssg::TextInputArguments{std::string{character}}}).accepted();
    };
    for (char character : std::string{"foo bar"}) ASSERT_TRUE(type(character));
    ASSERT_EQ(runtime.active_document_text(), std::string{"abcfoo bar"});

    // The space sealed the "foo " unit, so the first undo removes only "bar".
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"edit.undo", runtime.revision(), {}}).accepted());
    ASSERT_EQ(runtime.active_document_text(), std::string{"abcfoo "});
    // The second undo removes the "foo " word unit.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"edit.undo", runtime.revision(), {}}).accepted());
    ASSERT_EQ(runtime.active_document_text(), std::string{"abc"});

    // Newlines seal a unit per line.
    ASSERT_TRUE(type('x'));
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"text.newline", runtime.revision(), {}}).accepted());
    ASSERT_TRUE(type('y'));
    ASSERT_EQ(runtime.active_document_text(), std::string{"abcx\ny"});
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"edit.undo", runtime.revision(), {}}).accepted());
    ASSERT_EQ(runtime.active_document_text(), std::string{"abcx\n"});
}

TEST(workspace_replace_dispatch_matches_feature_preview_and_disk_apply) {
    auto root = unique_root();
    std::ofstream{root / "workspace" / "other.txt"} << "cat";
    auto created = ssg::EditorRuntime::create({root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::in_process}, ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"edit.txt"}}).accepted());

    ssg::FindRequest request{"cat", {}, std::nullopt, 100000, nullptr};
    DiskPreviewWorkspace oracle_workspace{root / "workspace", runtime.revision()};
    auto oracle = ssg::preview_workspace_replace(oracle_workspace, runtime.revision(), request, "dog");
    ASSERT_TRUE(oracle.accepted());
    ASSERT_EQ(oracle.preview->changes.size(), std::size_t{1});

    auto preview = runtime.dispatch(ssg::ClientId{1}, {"replace.workspace_preview", runtime.revision(), ssg::WorkspaceReplaceArguments{request, "dog"}});
    ASSERT_TRUE(preview.accepted());
    auto apply = runtime.dispatch(ssg::ClientId{1}, {"replace.workspace_apply", runtime.revision(), {}});
    ASSERT_TRUE(apply.accepted());
    ASSERT_EQ(read_text(root / "workspace" / "other.txt"), std::string{"dog"});
    ASSERT_EQ(read_text(root / "workspace" / "edit.txt"), std::string{"abc"});
    ASSERT_EQ(runtime.active_document_text(), std::string{"abc"});
}

TEST(workspace_replace_rejects_stale_and_out_of_bounds_preview) {
    auto root = unique_root();
    auto workspace = root / "workspace";
    std::filesystem::create_directories(workspace / ".ssg" / "scratch");
    std::filesystem::create_directories(workspace / ".ssg" / "recovery");
    std::ofstream{workspace / "other.txt"} << "cat";
    auto created = ssg::EditorRuntime::create({
        workspace, workspace / ".ssg" / "scratch",
        workspace / ".ssg" / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::in_process}, ssg::ViewId{1}).accepted());

    ssg::FindRequest request{"cat", {}, std::nullopt, 100000, nullptr};
    DiskPreviewWorkspace oracle_workspace{workspace, runtime.revision()};
    auto oracle = ssg::preview_workspace_replace(oracle_workspace, runtime.revision(), request, "dog");
    ASSERT_TRUE(oracle.accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"replace.workspace_preview", runtime.revision(), ssg::WorkspaceReplaceArguments{request, "dog"}}).accepted());

    auto escaped = *oracle.preview;
    escaped.changes.front().path = "../outside.txt";
    auto rejected_path = runtime.dispatch(ssg::ClientId{1}, {"replace.workspace_apply", runtime.revision(), escaped});
    ASSERT_FALSE(rejected_path.accepted());
    ASSERT_FALSE(std::filesystem::exists(root / "outside.txt"));

    std::ofstream{workspace / ".ssg" / "scratch" / "hidden.txt"} << "cat";
    auto runtime_state = *oracle.preview;
    runtime_state.changes.front().path = ".ssg/scratch/hidden.txt";
    runtime_state.changes.front().before = "cat";
    runtime_state.changes.front().after = "dog";
    auto rejected_state = runtime.dispatch(ssg::ClientId{1}, {"replace.workspace_apply", runtime.revision(), runtime_state});
    ASSERT_FALSE(rejected_state.accepted());
    ASSERT_EQ(read_text(workspace / ".ssg" / "scratch" / "hidden.txt"), std::string{"cat"});

    auto rejected_type = runtime.dispatch(ssg::ClientId{1}, {"replace.workspace_apply", runtime.revision(), std::string{"wrong"}});
    ASSERT_FALSE(rejected_type.accepted());
    ASSERT_EQ(read_text(workspace / "other.txt"), std::string{"cat"});

    std::ofstream{workspace / "other.txt", std::ios::binary | std::ios::trunc} << "fresh";
    auto rejected_stale = runtime.dispatch(ssg::ClientId{1}, {"replace.workspace_apply", runtime.revision(), {}});
    ASSERT_FALSE(rejected_stale.accepted());
    ASSERT_EQ(read_text(workspace / "other.txt"), std::string{"fresh"});
}

TEST(workspace_replace_updates_open_document_snapshot_and_disk) {
    auto root = unique_root();
    std::ofstream{root / "workspace" / "edit.txt"} << "cat cat";
    auto created = ssg::EditorRuntime::create({root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::in_process}, ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"edit.txt"}}).accepted());

    ssg::FindRequest request{"cat", {}, std::nullopt, 100000, nullptr};
    DiskPreviewWorkspace oracle_workspace{root / "workspace", runtime.revision()};
    auto oracle = ssg::preview_workspace_replace(oracle_workspace, runtime.revision(), request, "dog");
    ASSERT_TRUE(oracle.accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"replace.workspace_preview", runtime.revision(), ssg::WorkspaceReplaceArguments{request, "dog"}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"replace.workspace_apply", runtime.revision(), {}}).accepted());
    ASSERT_EQ(read_text(root / "workspace" / "edit.txt"), std::string{"dog dog"});
    ASSERT_EQ(runtime.active_document_text(), std::string{"dog dog"});
    auto snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 12});
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_EQ(snapshot->sections().document.text, std::string{"dog dog"});
}

TEST(workspace_search_and_replace_exclude_runtime_state_roots) {
    auto root = unique_root();
    auto workspace = root / "workspace";
    std::filesystem::create_directories(workspace / ".ssg" / "scratch");
    std::filesystem::create_directories(workspace / ".ssg" / "recovery");
    std::ofstream{workspace / "visible.txt"} << "secret";
    std::ofstream{workspace / ".ssg" / "scratch" / "hidden.txt"} << "secret";
    std::ofstream{workspace / ".ssg" / "recovery" / "journal.txt"} << "secret";

    auto created = ssg::EditorRuntime::create({
        workspace, workspace / ".ssg" / "scratch",
        workspace / ".ssg" / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::in_process}, ssg::ViewId{1}).accepted());

    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"search.workspace", runtime.revision(), std::string{"#secret"}}).accepted());
    auto search_snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 12});
    ASSERT_TRUE(search_snapshot.has_value());
    ASSERT_EQ(search_snapshot->sections().search.results.size(), std::size_t{1});
    if (!search_snapshot->sections().search.results.empty()) {
        ASSERT_EQ(search_snapshot->sections().search.results[0].path, std::string{"visible.txt"});
    }

    ssg::FindRequest request{"secret", {}, std::nullopt, 100000, nullptr};
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"replace.workspace_preview", runtime.revision(), ssg::WorkspaceReplaceArguments{request, "public"}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"replace.workspace_apply", runtime.revision(), {}}).accepted());
    ASSERT_EQ(read_text(workspace / "visible.txt"), std::string{"public"});
    ASSERT_EQ(read_text(workspace / ".ssg" / "scratch" / "hidden.txt"), std::string{"secret"});
    ASSERT_EQ(read_text(workspace / ".ssg" / "recovery" / "journal.txt"), std::string{"secret"});
}

TEST(find_update_query_projects_matches_and_prompt_and_next_cycles) {
    auto root = unique_root();
    auto workspace = root / "workspace";
    std::filesystem::create_directories(workspace);
    std::ofstream{workspace / "hits.txt"} << "cat cat cat";

    auto created = ssg::EditorRuntime::create({
        workspace, root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::in_process}, ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"hits.txt"}}).accepted());

    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"find.open", runtime.revision(), {}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"find.update_query", runtime.revision(), ssg::FindQueryArguments{"cat"}}).accepted());

    auto snapshot = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 12});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) return;
    auto const& find = snapshot->sections().find_replace;
    ASSERT_TRUE(find.open);
    ASSERT_EQ(find.query, std::string{"cat"});
    ASSERT_EQ(find.matches.size(), std::size_t{3});
    ASSERT_EQ(find.matches[0].begin.value(), std::uint64_t{0});
    ASSERT_EQ(find.matches[0].end.value(), std::uint64_t{3});
    ASSERT_EQ(find.matches[1].begin.value(), std::uint64_t{4});
    ASSERT_EQ(find.matches[1].end.value(), std::uint64_t{7});
    ASSERT_EQ(find.matches[2].begin.value(), std::uint64_t{8});
    ASSERT_EQ(find.matches[2].end.value(), std::uint64_t{11});

    // The find prompt projects the controller query and the 1-based match count.
    auto const& prompt = snapshot->sections().prompt_status.prompt;
    ASSERT_TRUE(prompt.has_value());
    if (prompt) {
        std::string query_value;
        std::string count_value;
        for (auto const& control : prompt->controls) {
            if (control.kind == ssg::PromptControlKind::input) query_value = control.value;
            if (control.kind == ssg::PromptControlKind::count) count_value = control.value;
        }
        ASSERT_EQ(query_value, std::string{"cat"});
        ASSERT_EQ(count_value, std::string{"1/3"});
    }

    const auto active_after = [&](int advances) -> std::size_t {
        for (int i = 0; i < advances; ++i) {
            (void)runtime.dispatch(ssg::ClientId{1}, {"find.next", runtime.revision(), {}});
        }
        auto snap = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 12});
        return snap->sections().find_replace.active_match.value_or(999);
    };
    ASSERT_EQ(active_after(1), std::size_t{1});
    ASSERT_EQ(active_after(1), std::size_t{2});
    ASSERT_EQ(active_after(1), std::size_t{0});
}

TEST(find_close_succeeds_without_an_active_document) {
    auto root = unique_root();
    auto workspace = root / "workspace";
    std::filesystem::create_directories(workspace);
    auto created = ssg::EditorRuntime::create({
        workspace, root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::in_process}, ssg::ViewId{1}).accepted());

    // No document is open: find.close (and next/previous) must not be rejected by
    // the active-document guard, so a find opened before the last tab closed can
    // still be dismissed.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"find.close", runtime.revision(), {}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"find.next", runtime.revision(), {}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"find.previous", runtime.revision(), {}}).accepted());
}

TEST(find_close_does_not_cancel_an_unrelated_prompt) {
    auto root = unique_root();
    auto workspace = root / "workspace";
    std::filesystem::create_directories(workspace);
    std::ofstream{workspace / "doc.txt"} << "hello";
    auto created = ssg::EditorRuntime::create({
        workspace, root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::in_process}, ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"doc.txt"}}).accepted());

    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"palette.open", runtime.revision(), {}}).accepted());
    auto before = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(before.has_value());
    if (before) {
        ASSERT_TRUE(before->sections().prompt_status.prompt.has_value());
        if (before->sections().prompt_status.prompt) {
            ASSERT_EQ(before->sections().prompt_status.prompt->kind, ssg::PromptKind::palette);
        }
    }

    // A find.close while the palette prompt is active must leave the palette
    // prompt intact (it only owns the find prompt).
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"find.close", runtime.revision(), {}}).accepted());
    auto after = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(after.has_value());
    if (after) {
        ASSERT_TRUE(after->sections().prompt_status.prompt.has_value());
        if (after->sections().prompt_status.prompt) {
            ASSERT_EQ(after->sections().prompt_status.prompt->kind, ssg::PromptKind::palette);
        }
    }
}

TEST(find_closes_when_switching_to_a_different_document) {
    auto root = unique_root();
    auto workspace = root / "workspace";
    std::filesystem::create_directories(workspace);
    std::ofstream{workspace / "a.txt"} << "cat cat cat";
    std::ofstream{workspace / "b.txt"} << "dog dog dog";
    auto created = ssg::EditorRuntime::create({
        workspace, root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::in_process}, ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"a.txt"}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"find.open", runtime.revision(), {}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"find.update_query", runtime.revision(), ssg::FindQueryArguments{"cat"}}).accepted());
    {
        auto snap = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
        ASSERT_TRUE(snap.has_value());
        if (snap) ASSERT_EQ(snap->sections().find_replace.matches.size(), std::size_t{3});
    }

    // Switching to another freshly opened document (which shares revision 1 with
    // a.txt) must dismiss find: identity, not revision equality, binds the state.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"b.txt"}}).accepted());
    auto after = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(after.has_value());
    if (after) {
        ASSERT_FALSE(after->sections().find_replace.open);
        ASSERT_FALSE(after->sections().prompt_status.prompt.has_value());
    }
}

TEST(find_scrolls_the_viewport_to_follow_the_active_match) {
    auto root = unique_root();
    auto workspace = root / "workspace";
    std::filesystem::create_directories(workspace);
    std::string text;
    for (int line = 0; line < 50; ++line) {
        text += (line == 40) ? "target here" : "filler";
        text += '\n';
    }
    std::ofstream{workspace / "tall.txt"} << text;
    auto created = ssg::EditorRuntime::create({
        workspace, root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::in_process}, ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"tall.txt"}}).accepted());

    // Baseline: the viewport starts at the top.
    {
        auto snap = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
        ASSERT_TRUE(snap.has_value());
        if (snap) ASSERT_EQ(snap->client().viewport.first_visual_row, std::uint32_t{0});
    }

    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"find.open", runtime.revision(), {}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"find.update_query", runtime.revision(), ssg::FindQueryArguments{"target"}}).accepted());

    // The match on line 40 lies below the initial 24-row viewport, so revealing
    // it must scroll down and the match's logical line must be visible.
    auto snap = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snap.has_value());
    if (!snap) return;
    auto const& viewport = snap->client().viewport;
    ASSERT_TRUE(viewport.first_visual_row > std::uint32_t{0});
    bool match_line_visible = false;
    for (auto const& row : viewport.visible_rows) {
        if (row.logical_line == 40) match_line_visible = true;
    }
    ASSERT_TRUE(match_line_visible);
}

TEST(replace_current_replaces_active_match_and_resets_to_first) {
    auto root = unique_root();
    auto workspace = root / "workspace";
    std::filesystem::create_directories(workspace);
    std::ofstream{workspace / "r.txt"} << "cat cat cat";
    auto created = ssg::EditorRuntime::create({
        workspace, root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::in_process}, ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"r.txt"}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"replace.open", runtime.revision(), {}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"find.update_query", runtime.revision(), ssg::FindQueryArguments{"cat"}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"replace.update_replacement", runtime.revision(), ssg::FindQueryArguments{"dog"}}).accepted());

    {
        auto snap = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
        ASSERT_TRUE(snap.has_value());
        if (snap) {
            ASSERT_EQ(snap->sections().find_replace.replacement, std::string{"dog"});
            ASSERT_EQ(snap->sections().find_replace.matches.size(), std::size_t{3});
            // The replace prompt row 1 projects the replacement.
            auto const& prompt = snap->sections().prompt_status.prompt;
            ASSERT_TRUE(prompt.has_value());
            if (prompt) {
                std::string replacement_value;
                for (auto const& control : prompt->controls) {
                    if (control.kind == ssg::PromptControlKind::input &&
                        control.id == "replace.replacement") {
                        replacement_value = control.value;
                    }
                }
                ASSERT_EQ(replacement_value, std::string{"dog"});
            }
        }
    }

    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"replace.current", runtime.revision(), {}}).accepted());
    ASSERT_EQ(runtime.active_document_text(), std::string{"dog cat cat"});
    auto after = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(after.has_value());
    if (after) {
        auto const& fr = after->sections().find_replace;
        ASSERT_EQ(fr.matches.size(), std::size_t{2});
        // Reset-to-first: active index is 0, now pointing at the match at [4,7).
        ASSERT_TRUE(fr.active_match.has_value());
        if (fr.active_match) ASSERT_EQ(*fr.active_match, std::size_t{0});
        if (fr.matches.size() == 2) {
            ASSERT_EQ(fr.matches[0].begin.value(), std::uint64_t{4});
            ASSERT_EQ(fr.matches[0].end.value(), std::uint64_t{7});
        }
    }
}

TEST(replace_all_replaces_every_match) {
    auto root = unique_root();
    auto workspace = root / "workspace";
    std::filesystem::create_directories(workspace);
    std::ofstream{workspace / "r.txt"} << "cat cat cat";
    auto created = ssg::EditorRuntime::create({
        workspace, root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::in_process}, ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"r.txt"}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"replace.open", runtime.revision(), {}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"find.update_query", runtime.revision(), ssg::FindQueryArguments{"cat"}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"replace.update_replacement", runtime.revision(), ssg::FindQueryArguments{"dog"}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"replace.all", runtime.revision(), {}}).accepted());
    ASSERT_EQ(runtime.active_document_text(), std::string{"dog dog dog"});
    auto after = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(after.has_value());
    if (after) {
        ASSERT_TRUE(after->sections().find_replace.matches.empty());
        ASSERT_FALSE(after->sections().find_replace.active_match.has_value());
    }
}

TEST(replace_commands_are_benign_no_ops_without_a_replace_prompt) {
    auto root = unique_root();
    auto workspace = root / "workspace";
    std::filesystem::create_directories(workspace);
    std::ofstream{workspace / "r.txt"} << "cat cat cat";
    auto created = ssg::EditorRuntime::create({
        workspace, root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::in_process}, ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"r.txt"}}).accepted());

    // A find prompt (not replace) is open: replace commands must be benign
    // success no-ops that do not mutate the document or controller state.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"find.open", runtime.revision(), {}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"find.update_query", runtime.revision(), ssg::FindQueryArguments{"cat"}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"replace.update_replacement", runtime.revision(), ssg::FindQueryArguments{"dog"}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"replace.current", runtime.revision(), {}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"replace.all", runtime.revision(), {}}).accepted());
    ASSERT_EQ(runtime.active_document_text(), std::string{"cat cat cat"});
    auto snap = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snap.has_value());
    if (snap) ASSERT_TRUE(snap->sections().find_replace.replacement.empty());
}

TEST(find_toggle_case_flips_option_and_changes_matches_and_guards_when_no_prompt) {
    auto root = unique_root();
    auto workspace = root / "workspace";
    std::filesystem::create_directories(workspace);
    std::ofstream{workspace / "m.txt"} << "Cat cat CAT";
    auto created = ssg::EditorRuntime::create({
        workspace, root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::in_process}, ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"m.txt"}}).accepted());

    // No find/replace prompt yet: find.toggle_case must be a benign no-op that
    // leaves the (default) options untouched.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"find.toggle_case", runtime.revision(), {}}).accepted());
    {
        auto snap = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
        ASSERT_TRUE(snap.has_value());
        if (snap) ASSERT_FALSE(snap->sections().find_replace.options.case_sensitive);
    }

    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"find.open", runtime.revision(), {}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"find.update_query", runtime.revision(), ssg::FindQueryArguments{"cat"}}).accepted());
    {
        // Case-insensitive (default): all three "cat"s match.
        auto snap = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
        ASSERT_TRUE(snap.has_value());
        if (snap) ASSERT_EQ(snap->sections().find_replace.matches.size(), std::size_t{3});
    }

    // Toggle case sensitivity: now only the lowercase "cat" matches.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"find.toggle_case", runtime.revision(), {}}).accepted());
    auto snap = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snap.has_value());
    if (snap) {
        ASSERT_TRUE(snap->sections().find_replace.options.case_sensitive);
        ASSERT_EQ(snap->sections().find_replace.matches.size(), std::size_t{1});
    }
}

TEST(replace_open_preserves_find_options) {
    auto root = unique_root();
    auto workspace = root / "workspace";
    std::filesystem::create_directories(workspace);
    std::ofstream{workspace / "m.txt"} << "Cat cat CAT";
    auto created = ssg::EditorRuntime::create({
        workspace, root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::in_process}, ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"m.txt"}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"find.open", runtime.revision(), {}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"find.update_query", runtime.revision(), ssg::FindQueryArguments{"cat"}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"find.toggle_case", runtime.revision(), {}}).accepted());
    // Case-sensitive find matched only the lowercase "cat".
    {
        auto snap = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
        ASSERT_TRUE(snap.has_value());
        if (snap) ASSERT_EQ(snap->sections().find_replace.matches.size(), std::size_t{1});
    }
    // Opening replace must NOT widen the match population: options carry over so
    // replace.all acts on exactly what the user reviewed.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"replace.open", runtime.revision(), {}}).accepted());
    auto snap = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snap.has_value());
    if (snap) {
        ASSERT_TRUE(snap->sections().find_replace.options.case_sensitive);
        ASSERT_EQ(snap->sections().find_replace.matches.size(), std::size_t{1});
    }
}

TEST(find_close_dismisses_the_replace_prompt) {
    auto root = unique_root();
    auto workspace = root / "workspace";
    std::filesystem::create_directories(workspace);
    std::ofstream{workspace / "m.txt"} << "cat cat";
    auto created = ssg::EditorRuntime::create({
        workspace, root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::in_process}, ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"m.txt"}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"replace.open", runtime.revision(), {}}).accepted());
    {
        auto snap = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
        ASSERT_TRUE(snap.has_value());
        if (snap) {
            ASSERT_TRUE(snap->sections().find_replace.open);
            ASSERT_TRUE(snap->sections().prompt_status.prompt.has_value());
        }
    }
    // A single find.close must close the controller AND dismiss the replace
    // prompt (no stale prompt requiring a second cancel).
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"find.close", runtime.revision(), {}}).accepted());
    auto snap = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snap.has_value());
    if (snap) {
        ASSERT_FALSE(snap->sections().find_replace.open);
        ASSERT_FALSE(snap->sections().prompt_status.prompt.has_value());
    }
}

TEST(pointer_selection_commands_focus_the_editor_keyboard_motion_does_not) {
    auto root = unique_root();
    auto created = ssg::EditorRuntime::create({root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::in_process}, ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"edit.txt"}}).accepted());
    const ssg::ViewportDimensions dims{80, 24};
    auto focus = [&] {
        auto snap = runtime.snapshot(ssg::ClientId{1}, dims);
        return snap ? snap->sections().shell.focus : ssg::FocusTarget::editor;
    };
    auto focus_panel = [&] {
        auto snap = runtime.snapshot(ssg::ClientId{1}, dims);
        bool const shown = snap && snap->sections().shell.panel.has_value();
        if (!shown) {
            ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"panel.toggle", runtime.revision(), {}}).accepted());
        }
        ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"panel.focus", runtime.revision(), {}}).accepted());
    };

    // A pointer click-to-caret (cursor.set_position) from panel focus moves focus
    // to the editor.
    focus_panel();
    ASSERT_EQ(focus(), ssg::FocusTarget::panel);
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"cursor.set_position", runtime.revision(), ssg::SelectionCommandArguments{ssg::resolve_document_position("abc", ssg::ByteOffset{1}), std::nullopt}}).accepted());
    ASSERT_EQ(focus(), ssg::FocusTarget::editor);

    // A pointer drag (select.set_range) likewise focuses the editor.
    focus_panel();
    ASSERT_EQ(focus(), ssg::FocusTarget::panel);
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"select.set_range", runtime.revision(),
        ssg::SelectionCommandArguments{std::nullopt, ssg::Selection{ssg::resolve_document_position("abc", ssg::ByteOffset{0}).value(), ssg::resolve_document_position("abc", ssg::ByteOffset{2}).value()}}}).accepted());
    ASSERT_EQ(focus(), ssg::FocusTarget::editor);

    // A KEYBOARD caret motion (a different SelectionCommand) does NOT change focus:
    // dispatched from panel focus, the caret moves but the keyboard stays on the panel.
    focus_panel();
    ASSERT_EQ(focus(), ssg::FocusTarget::panel);
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"cursor.left", runtime.revision(), {}}).accepted());
    ASSERT_EQ(focus(), ssg::FocusTarget::panel);
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"select.line_down", runtime.revision(), {}}).accepted());
    ASSERT_EQ(focus(), ssg::FocusTarget::panel);
}

TEST(edit_reveals_the_primary_caret_free_scroll_does_not_and_follows_primary) {
    // A document taller than the pane, one single-cell line per row.
    auto root = std::filesystem::current_path() / "runtime_editing_reveal";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    std::string text;
    for (int i = 0; i < 100; ++i) text += "a\n";  // line L starts at byte L*2
    std::ofstream{root / "workspace" / "tall.txt", std::ios::binary} << text;
    auto created = ssg::EditorRuntime::create({root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::in_process}, ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"tall.txt"}}).accepted());

    const ssg::ViewportDimensions dims{80, 24};
    auto first_row = [&] {
        auto snap = runtime.snapshot(ssg::ClientId{1}, dims);
        return snap ? snap->client().viewport.first_visual_row : 0U;
    };
    auto maximum = [&] {
        auto snap = runtime.snapshot(ssg::ClientId{1}, dims);
        return snap ? snap->client().viewport.scrollbar.maximum_first_row : 0U;
    };
    // Snapshot once to populate the pane-height cache; the caret is at the top.
    ASSERT_EQ(first_row(), 0U);
    auto const max_first = maximum();
    ASSERT_TRUE(max_first > 0);  // the document is scrollable

    // Free scroll DOWN with no edit: the offset moves and does NOT snap back.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"view.scroll_lines", runtime.revision(), ssg::ScrollLinesArguments{40}}).accepted());
    ASSERT_EQ(first_row(), 40U);   // caret (line 0) is now off-screen above
    ASSERT_EQ(first_row(), 40U);   // a second read without an edit stays put

    // Typing at the (off-screen) caret reveals it: minimal offset to show line 0.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"text.insert", runtime.revision(), ssg::TextInputArguments{"x"}}).accepted());
    ASSERT_EQ(first_row(), 0U);

    // Move the caret to the last line (navigation reveals it to the bottom), then
    // free-scroll to the top so the caret is off-screen below.
    auto doc = runtime.active_document_text();
    auto end_pos = ssg::resolve_document_position(doc, ssg::ByteOffset{static_cast<std::uint32_t>(doc.size())});
    ASSERT_TRUE(end_pos.has_value());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"cursor.set_position", runtime.revision(), ssg::SelectionCommandArguments{end_pos, std::nullopt}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"view.scroll_lines", runtime.revision(), ssg::ScrollLinesArguments{-200}}).accepted());
    ASSERT_EQ(first_row(), 0U);
    // An edit at the bottom caret reveals it to the maximum offset (last line shown).
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"text.insert", runtime.revision(), ssg::TextInputArguments{"y"}}).accepted());
    ASSERT_EQ(first_row(), maximum());

    // Two cursors: secondary near the top (line 0), PRIMARY near the bottom (the
    // back selection). select.add_range pushes the new range to the back.
    doc = runtime.active_document_text();
    auto top = ssg::resolve_document_position(doc, ssg::ByteOffset{0});
    auto bottom_line_start = ssg::resolve_document_position(doc, ssg::ByteOffset{static_cast<std::uint32_t>(doc.size()) - 2});
    ASSERT_TRUE(top.has_value());
    ASSERT_TRUE(bottom_line_start.has_value());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"cursor.set_position", runtime.revision(), ssg::SelectionCommandArguments{top, std::nullopt}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"select.add_range", runtime.revision(), ssg::SelectionCommandArguments{std::nullopt, ssg::Selection{*bottom_line_start, *bottom_line_start}}}).accepted());
    // Free-scroll to the top so the primary (bottom) caret is off-screen below.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"view.scroll_lines", runtime.revision(), ssg::ScrollLinesArguments{-200}}).accepted());
    ASSERT_EQ(first_row(), 0U);
    // A multi-cursor insert reveals the PRIMARY caret (bottom), not the secondary
    // (top): the offset jumps to the maximum, not staying at 0.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"text.insert", runtime.revision(), ssg::TextInputArguments{"z"}}).accepted());
    ASSERT_EQ(first_row(), maximum());
    std::filesystem::remove_all(root);
}

TEST(undo_and_paste_reveal_the_caret) {
    auto root = std::filesystem::current_path() / "runtime_editing_reveal2";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    std::string text;
    for (int i = 0; i < 100; ++i) text += "a\n";
    std::ofstream{root / "workspace" / "tall.txt", std::ios::binary} << text;
    auto created = ssg::EditorRuntime::create({root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::in_process}, ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"tall.txt"}}).accepted());
    const ssg::ViewportDimensions dims{80, 24};
    auto first_row = [&] {
        auto snap = runtime.snapshot(ssg::ClientId{1}, dims);
        return snap ? snap->client().viewport.first_visual_row : 0U;
    };
    ASSERT_EQ(first_row(), 0U);

    // Type a character (caret at top), then scroll away and UNDO: undo reveals.
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"text.insert", runtime.revision(), ssg::TextInputArguments{"x"}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"view.scroll_lines", runtime.revision(), ssg::ScrollLinesArguments{40}}).accepted());
    ASSERT_EQ(first_row(), 40U);
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"edit.undo", runtime.revision(), {}}).accepted());
    ASSERT_EQ(first_row(), 0U);

    // Copy a line, collapse the caret to the top, scroll away, and PASTE: the
    // paste inserts a duplicate (a real document mutation) and reveals the caret.
    // (Pasting over the same selection would reproduce identical bytes — a no-op
    // that correctly does not mutate or reveal.)
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"select.line_down", runtime.revision(), {}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"clipboard.copy", runtime.revision(), {}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"cursor.set_position", runtime.revision(), ssg::SelectionCommandArguments{ssg::resolve_document_position(text, ssg::ByteOffset{0}), std::nullopt}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"view.scroll_lines", runtime.revision(), ssg::ScrollLinesArguments{40}}).accepted());
    ASSERT_EQ(first_row(), 40U);
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"clipboard.paste", runtime.revision(), {}}).accepted());
    // The pasted "a\n" pushes the caret to line 1; revealing from row 40 scrolls
    // up so the caret's row sits at the viewport top (first_row == its row).
    ASSERT_EQ(first_row(), 1U);
    std::filesystem::remove_all(root);
}

TEST(multi_cursor_paste_preserves_all_cursors) {
    auto root = std::filesystem::current_path() / "runtime_editing_mcpaste";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    std::ofstream{root / "workspace" / "m.txt", std::ios::binary} << "aaa\nbbb\nccc\n";
    auto created = ssg::EditorRuntime::create({root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::in_process}, ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"m.txt"}}).accepted());
    const ssg::ViewportDimensions dims{80, 24};
    auto selection_count = [&] {
        auto snap = runtime.snapshot(ssg::ClientId{1}, dims);
        return snap ? snap->sections().selection.selections.items().size() : std::size_t{0};
    };

    // Build two cursors (top of line 0 and top of line 1), copy, then paste. The
    // paste must not collapse the multi-cursor set to a single caret.
    auto doc = runtime.active_document_text();
    auto p0 = ssg::resolve_document_position(doc, ssg::ByteOffset{0});
    auto p1 = ssg::resolve_document_position(doc, ssg::ByteOffset{4});
    ASSERT_TRUE(p0.has_value() && p1.has_value());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"cursor.set_position", runtime.revision(), ssg::SelectionCommandArguments{p0, std::nullopt}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"select.add_range", runtime.revision(), ssg::SelectionCommandArguments{std::nullopt, ssg::Selection{*p1, *p1}}}).accepted());
    ASSERT_EQ(selection_count(), std::size_t{2});
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"select.line_end", runtime.revision(), {}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"clipboard.copy", runtime.revision(), {}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"clipboard.paste", runtime.revision(), {}}).accepted());
    ASSERT_EQ(selection_count(), std::size_t{2});
    std::filesystem::remove_all(root);
}

TEST(replace_all_reveals_the_caret_when_no_match_remains) {
    auto root = std::filesystem::current_path() / "runtime_editing_replacereveal";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    // A tall doc with the only match near the bottom.
    std::string text;
    for (int i = 0; i < 90; ++i) text += "filler\n";
    text += "needle\n";
    std::ofstream{root / "workspace" / "t.txt", std::ios::binary} << text;
    auto created = ssg::EditorRuntime::create({root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::in_process}, ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(), std::string{"t.txt"}}).accepted());
    const ssg::ViewportDimensions dims{80, 24};
    auto first_row = [&] {
        auto snap = runtime.snapshot(ssg::ClientId{1}, dims);
        return snap ? snap->client().viewport.first_visual_row : 0U;
    };
    ASSERT_EQ(first_row(), 0U);  // caret at top; the match is off-screen far below

    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"replace.open", runtime.revision(), {}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"find.update_query", runtime.revision(), ssg::FindQueryArguments{"needle"}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"replace.update_replacement", runtime.revision(), ssg::FindQueryArguments{"pin"}}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1}, {"replace.all", runtime.revision(), {}}).accepted());
    // No match remains, but the caret (now at the replaced text near the bottom)
    // is revealed rather than left off-screen.
    ASSERT_TRUE(first_row() > 0U);
    std::filesystem::remove_all(root);
}

} // namespace

int main() {
    RUN(runtime_text_selection_and_history_match_feature_operations);
    RUN(typing_undo_breaks_on_word_and_line_boundaries);
    RUN(workspace_replace_dispatch_matches_feature_preview_and_disk_apply);
    RUN(workspace_replace_rejects_stale_and_out_of_bounds_preview);
    RUN(workspace_replace_updates_open_document_snapshot_and_disk);
    RUN(workspace_search_and_replace_exclude_runtime_state_roots);
    RUN(pointer_selection_commands_focus_the_editor_keyboard_motion_does_not);
    RUN(edit_reveals_the_primary_caret_free_scroll_does_not_and_follows_primary);
    RUN(undo_and_paste_reveal_the_caret);
    RUN(multi_cursor_paste_preserves_all_cursors);
    RUN(replace_all_reveals_the_caret_when_no_match_remains);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
