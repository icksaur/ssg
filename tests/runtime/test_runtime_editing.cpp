#include "../test_helpers.h"

#include <ssg/editor_runtime.h>
#include <ssg/find_replace.h>
#include <ssg/selection.h>
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
