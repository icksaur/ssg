#include "test_helpers.h"

#include <ssg/file_commands.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        path_ = std::filesystem::temp_directory_path() /
                ("ssg-file-commands-" + std::to_string(
                                            std::chrono::steady_clock::now()
                                                .time_since_epoch()
                                                .count()));
        std::filesystem::create_directories(path_);
    }
    ~TemporaryDirectory() { std::filesystem::remove_all(path_); }
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

TEST(command_set_owns_every_normative_file_command) {
    const auto commands = ssg::file_commands_command_set();
    const std::array<std::string_view, 12> expected{{
        "workspace.open_directory",
        "file.new",
        "file.open",
        "file.open_recent",
        "file.open_dropped_content",
        "file.save",
        "file.save_all",
        "file.save_as",
        "file.reload",
        "file.rename",
        "file.delete",
        "file.new_directory",
    }};
    ASSERT_EQ(commands.descriptors().size(), expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        ASSERT_EQ(commands.descriptors()[index].id, expected[index]);
    }
    const auto& drop = commands.descriptors()[4];
    ASSERT_FALSE(drop.lua);
    ASSERT_EQ(drop.required_capability,
              std::optional<std::string_view>{"local_file_drop"});
}

TEST(path_commands_open_non_modal_path_prompts) {
    for (const auto command :
         {ssg::FileCommand::Open, ssg::FileCommand::SaveAs,
          ssg::FileCommand::Rename, ssg::FileCommand::NewDirectory}) {
        const auto request = ssg::file_path_prompt(command);
        ASSERT_EQ(request.kind, ssg::PromptKind::Path);
        ASSERT_EQ(request.inputs.size(), std::size_t{1});
        ASSERT_FALSE(request.inputs[0].accessible_label.empty());
    }
}

TEST(local_drop_requires_host_capability_and_sanitizes_label) {
    TemporaryDirectory temporary;
    auto recovery =
        ssg::RecoveryActions::create(temporary.path() / ".recovery");
    auto workspace = ssg::Workspace::create(temporary.path(), recovery);
    const std::array<std::uint8_t, 5> bytes{{'h', 'i', '\r', '\n', '!'}};
    const ssg::InvocationPrincipal local{
        ssg::ClientId{1}, ssg::InvocationOrigin::Websocket,
        {ssg::CapabilityId{"local_file_drop"}}};
    const ssg::InvocationPrincipal remote{
        ssg::ClientId{2}, ssg::InvocationOrigin::Websocket};
    const ssg::InvocationPrincipal lua{
        ssg::ClientId{3}, ssg::InvocationOrigin::Lua,
        {ssg::CapabilityId{"local_file_drop"}}};

    const auto accepted =
        workspace.open_dropped_content(local, bytes, "../../bad/name.txt");
    ASSERT_TRUE(accepted.accepted());
    const auto state = workspace.state(*accepted.document);
    ASSERT_EQ(state->key.kind(), ssg::JournalDocumentKeyKind::Untitled);
    ASSERT_EQ(state->display_label, std::string{"name.txt"});
    ASSERT_EQ(workspace.document(*accepted.document).snapshot().text,
              std::string{"hi\n!"});

    ASSERT_EQ(workspace.open_dropped_content(remote, bytes, "x").error,
              ssg::WorkspaceError::CapabilityDenied);
    ASSERT_EQ(workspace.open_dropped_content(lua, bytes, "x").error,
              ssg::WorkspaceError::CapabilityDenied);
    ASSERT_EQ(workspace.documents().size(), std::size_t{1});
}

TEST(binary_and_invalid_text_drops_open_read_only_without_path_authority) {
    TemporaryDirectory temporary;
    auto recovery =
        ssg::RecoveryActions::create(temporary.path() / ".recovery");
    auto workspace = ssg::Workspace::create(temporary.path(), recovery);
    const ssg::InvocationPrincipal local{
        ssg::ClientId{1}, ssg::InvocationOrigin::InProcess,
        {ssg::CapabilityId{"local_file_drop"}}};
    const std::array<std::uint8_t, 3> binary{{'a', 0, 'b'}};
    const std::array<std::uint8_t, 2> invalid{{0xc3, 0x28}};

    const auto binary_result =
        workspace.open_dropped_content(local, binary, "/tmp/a.bin");
    const auto invalid_result =
        workspace.open_dropped_content(local, invalid, "bad.txt");

    ASSERT_TRUE(binary_result.accepted());
    ASSERT_TRUE(invalid_result.accepted());
    const auto binary_state = workspace.state(*binary_result.document);
    const auto invalid_state = workspace.state(*invalid_result.document);
    ASSERT_EQ(binary_state->content_kind, ssg::FileContentKind::Binary);
    ASSERT_EQ(invalid_state->content_kind,
              ssg::FileContentKind::DecodeFailure);
    ASSERT_EQ(workspace.document(*binary_result.document).mode(),
              ssg::DocumentMode::ReadOnly);
    ASSERT_EQ(workspace.document(*invalid_result.document).mode(),
              ssg::DocumentMode::ReadOnly);
}

TEST(save_all_attempts_every_document_and_reports_failures) {
    TemporaryDirectory temporary;
    std::ofstream(temporary.path() / "one.txt") << "one";
    std::ofstream(temporary.path() / "two.txt") << "two";
    auto recovery =
        ssg::RecoveryActions::create(temporary.path() / ".recovery");
    auto workspace = ssg::Workspace::create(temporary.path(), recovery);
    const auto one = *workspace.open_file("one.txt").document;
    const auto two = *workspace.open_file("two.txt").document;
    ASSERT_TRUE(workspace
                    .apply(one, {workspace.document(one).revision(),
                                 {{ssg::ByteOffset{3}, 0, "!"}}})
                    .accepted());
    ASSERT_TRUE(workspace
                    .apply(two, {workspace.document(two).revision(),
                                 {{ssg::ByteOffset{3}, 0, "!"}}})
                    .accepted());
    std::filesystem::remove(temporary.path() / "two.txt");
    std::filesystem::create_directory(temporary.path() / "two.txt");

    const auto result = workspace.save_all();

    ASSERT_FALSE(result.accepted());
    ASSERT_EQ(result.failures.size(), std::size_t{1});
    const auto one_state = workspace.state(one);
    const auto two_state = workspace.state(two);
    ASSERT_FALSE(one_state->dirty);
    ASSERT_TRUE(two_state->dirty);
}

TEST(save_all_ignores_untitled_documents) {
    TemporaryDirectory temporary;
    std::ofstream(temporary.path() / "saved.txt") << "saved";
    auto recovery =
        ssg::RecoveryActions::create(temporary.path() / ".recovery");
    auto workspace = ssg::Workspace::create(temporary.path(), recovery);
    const auto saved = *workspace.open_file("saved.txt").document;
    ASSERT_TRUE(workspace.new_document().accepted());
    ASSERT_TRUE(workspace
                    .apply(saved, {workspace.document(saved).revision(),
                                   {{ssg::ByteOffset{5}, 0, "!"}}})
                    .accepted());

    const auto result = workspace.save_all();

    ASSERT_TRUE(result.accepted());
    ASSERT_TRUE(result.failures.empty());
}

}  // namespace

int main() {
    RUN(command_set_owns_every_normative_file_command);
    RUN(path_commands_open_non_modal_path_prompts);
    RUN(local_drop_requires_host_capability_and_sanitizes_label);
    RUN(binary_and_invalid_text_drops_open_read_only_without_path_authority);
    RUN(save_all_attempts_every_document_and_reports_failures);
    RUN(save_all_ignores_untitled_documents);
    std::cout << "Passed: " << passed << " Failed: " << failed << '\n';
    return failed == 0 ? 0 : 1;
}
