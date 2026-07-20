#include "test_helpers.h"

#include <ssg/FileCommands.h>

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

TEST(commandSetOwnsEveryNormativeFileCommand) {
    const auto commands = ssg::fileCommandsCommandSet();
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
    ASSERT_EQ(drop.requiredCapability,
              std::optional<std::string_view>{"local_file_drop"});
}

TEST(pathCommandsOpenNonModalPathPrompts) {
    for (const auto command :
         {ssg::FileCommand::Open, ssg::FileCommand::SaveAs,
          ssg::FileCommand::Rename, ssg::FileCommand::NewDirectory}) {
        const auto request = ssg::fileCommandsCommandSet().pathPrompt(command);
        ASSERT_EQ(request.kind, ssg::PromptKind::Path);
        ASSERT_EQ(request.inputs.size(), std::size_t{1});
        ASSERT_FALSE(request.inputs[0].accessibleLabel.empty());
    }
}

TEST(localDropRequiresHostCapabilityAndSanitizesLabel) {
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
        workspace.openDroppedContent(local, bytes, "../../bad/name.txt");
    ASSERT_TRUE(accepted.accepted());
    const auto state = workspace.state(*accepted.document);
    ASSERT_EQ(state->key.kind(), ssg::JournalDocumentKeyKind::Untitled);
    ASSERT_EQ(state->displayLabel, std::string{"name.txt"});
    ASSERT_EQ(workspace.document(*accepted.document).snapshot().text,
              std::string{"hi\n!"});

    ASSERT_EQ(workspace.openDroppedContent(remote, bytes, "x").error,
              ssg::WorkspaceError::CapabilityDenied);
    ASSERT_EQ(workspace.openDroppedContent(lua, bytes, "x").error,
              ssg::WorkspaceError::CapabilityDenied);
    ASSERT_EQ(workspace.documents().size(), std::size_t{1});
}

TEST(binaryAndInvalidTextDropsOpenReadOnlyWithoutPathAuthority) {
    TemporaryDirectory temporary;
    auto recovery =
        ssg::RecoveryActions::create(temporary.path() / ".recovery");
    auto workspace = ssg::Workspace::create(temporary.path(), recovery);
    const ssg::InvocationPrincipal local{
        ssg::ClientId{1}, ssg::InvocationOrigin::InProcess,
        {ssg::CapabilityId{"local_file_drop"}}};
    const std::array<std::uint8_t, 3> binary{{'a', 0, 'b'}};
    const std::array<std::uint8_t, 2> invalid{{0xc3, 0x28}};

    const auto binaryResult =
        workspace.openDroppedContent(local, binary, "/tmp/a.bin");
    const auto invalidResult =
        workspace.openDroppedContent(local, invalid, "bad.txt");

    ASSERT_TRUE(binaryResult.accepted());
    ASSERT_TRUE(invalidResult.accepted());
    const auto binaryState = workspace.state(*binaryResult.document);
    const auto invalidState = workspace.state(*invalidResult.document);
    ASSERT_EQ(binaryState->contentKind, ssg::FileContentKind::Binary);
    ASSERT_EQ(invalidState->contentKind,
              ssg::FileContentKind::DecodeFailure);
    ASSERT_EQ(workspace.document(*binaryResult.document).mode(),
              ssg::DocumentMode::ReadOnly);
    ASSERT_EQ(workspace.document(*invalidResult.document).mode(),
              ssg::DocumentMode::ReadOnly);
}

TEST(saveAllAttemptsEveryDocumentAndReportsFailures) {
    TemporaryDirectory temporary;
    std::ofstream(temporary.path() / "one.txt") << "one";
    std::ofstream(temporary.path() / "two.txt") << "two";
    auto recovery =
        ssg::RecoveryActions::create(temporary.path() / ".recovery");
    auto workspace = ssg::Workspace::create(temporary.path(), recovery);
    const auto one = *workspace.openFile("one.txt").document;
    const auto two = *workspace.openFile("two.txt").document;
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

    const auto result = workspace.saveAll();

    ASSERT_FALSE(result.accepted());
    ASSERT_EQ(result.failures.size(), std::size_t{1});
    const auto oneState = workspace.state(one);
    const auto twoState = workspace.state(two);
    ASSERT_FALSE(oneState->dirty);
    ASSERT_TRUE(twoState->dirty);
}

TEST(saveAllIgnoresUntitledDocuments) {
    TemporaryDirectory temporary;
    std::ofstream(temporary.path() / "saved.txt") << "saved";
    auto recovery =
        ssg::RecoveryActions::create(temporary.path() / ".recovery");
    auto workspace = ssg::Workspace::create(temporary.path(), recovery);
    const auto saved = *workspace.openFile("saved.txt").document;
    ASSERT_TRUE(workspace.newDocument().accepted());
    ASSERT_TRUE(workspace
                    .apply(saved, {workspace.document(saved).revision(),
                                   {{ssg::ByteOffset{5}, 0, "!"}}})
                    .accepted());

    const auto result = workspace.saveAll();

    ASSERT_TRUE(result.accepted());
    ASSERT_TRUE(result.failures.empty());
}

}  // namespace

int main() {
    RUN(commandSetOwnsEveryNormativeFileCommand);
    RUN(pathCommandsOpenNonModalPathPrompts);
    RUN(localDropRequiresHostCapabilityAndSanitizesLabel);
    RUN(binaryAndInvalidTextDropsOpenReadOnlyWithoutPathAuthority);
    RUN(saveAllAttemptsEveryDocumentAndReportsFailures);
    RUN(saveAllIgnoresUntitledDocuments);
    std::cout << "Passed: " << passed << " Failed: " << failed << '\n';
    return failed == 0 ? 0 : 1;
}
