#include "../test_helpers.h"

#include <ssg/editor_runtime.h>
#include <ssg/find_replace.h>
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

} // namespace

int main() {
    RUN(runtime_text_selection_and_history_match_feature_operations);
    RUN(typing_undo_breaks_on_word_and_line_boundaries);
    RUN(workspace_replace_dispatch_matches_feature_preview_and_disk_apply);
    RUN(workspace_replace_rejects_stale_and_out_of_bounds_preview);
    RUN(workspace_replace_updates_open_document_snapshot_and_disk);
    RUN(workspace_search_and_replace_exclude_runtime_state_roots);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
