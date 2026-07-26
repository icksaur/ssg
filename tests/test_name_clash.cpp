#include "test_helpers.h"

#include <ssg/EditorRuntime.h>
#include <ssg/FileCommands.h>
#include <ssg/PromptSurface.h>
#include <ssg/RecoveryActions.h>
#include <ssg/Workspace.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

namespace {

namespace fs = std::filesystem;

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        path_ = fs::temp_directory_path() /
                ("ssg-clash-" +
                 std::to_string(
                     std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path_);
    }
    ~TemporaryDirectory() {
        std::error_code code;
        fs::remove_all(path_, code);
    }
    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

std::unique_ptr<ssg::EditorRuntime> makeRuntime(const fs::path& root) {
    auto created = ssg::EditorRuntime::create(
        {root, root / "scratch", root / "recovery"});
    if (!created.accepted()) return nullptr;
    auto runtime = std::move(created.runtime);
    (void)runtime->attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                          ssg::ViewId{1});
    return runtime;
}

ssg::CommandResult run(ssg::EditorRuntime& runtime, std::string id,
                       std::any payload = {}) {
    return runtime.dispatch(
        ssg::ClientId{1},
        {std::move(id), runtime.revision(), std::move(payload)});
}

// Read WITHOUT the seam so the check is independent of the code under test.
std::string readOutOfBand(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

void writeOutOfBand(const fs::path& path, std::string_view text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
}

// The clash rule. Refusing is not enough: a command that truncated the occupant
// and THEN reported failure would satisfy a naive "was rejected" check while
// having already destroyed the file, so every case also proves the occupant's
// bytes survived untouched.
TEST(saveAsRefusesAnOccupiedDestinationAndLeavesItIntact) {
    TemporaryDirectory directory;
    const auto occupied = directory.path() / "occupied.txt";
    const std::string original = "bytes that must survive";
    writeOutOfBand(occupied, original);

    auto runtime = makeRuntime(directory.path());
    ASSERT_TRUE(runtime != nullptr);
    ASSERT_TRUE(run(*runtime, "file.new").accepted());

    const auto result =
        run(*runtime, "file.save_as", std::string{"occupied.txt"});
    ASSERT_FALSE(result.accepted());
    ASSERT_EQ(readOutOfBand(occupied), original);
}

TEST(renameRefusesAnOccupiedDestinationAndLeavesBothFilesIntact) {
    TemporaryDirectory directory;
    const auto source = directory.path() / "source.txt";
    const auto occupied = directory.path() / "occupied.txt";
    const std::string sourceText = "source bytes";
    const std::string occupiedText = "occupant bytes";
    writeOutOfBand(source, sourceText);
    writeOutOfBand(occupied, occupiedText);

    auto runtime = makeRuntime(directory.path());
    ASSERT_TRUE(runtime != nullptr);
    ASSERT_TRUE(run(*runtime, "file.open", std::string{"source.txt"}).accepted());

    const auto result =
        run(*runtime, "file.rename", std::string{"occupied.txt"});
    ASSERT_FALSE(result.accepted());
    // A rename that clobbered would leave one file; a rename that "rolled back"
    // by deleting could leave none. Both files must still be here, unchanged.
    ASSERT_EQ(readOutOfBand(occupied), occupiedText);
    ASSERT_EQ(readOutOfBand(source), sourceText);
}

TEST(newDirectoryRefusesAnOccupiedName) {
    TemporaryDirectory directory;
    const auto occupied = directory.path() / "taken";
    writeOutOfBand(occupied, "i am a file");

    auto runtime = makeRuntime(directory.path());
    ASSERT_TRUE(runtime != nullptr);

    ASSERT_FALSE(run(*runtime, "file.new_directory", std::string{"taken"})
                     .accepted());
    ASSERT_EQ(readOutOfBand(occupied), "i am a file");
}

// The one permitted overwrite: a document saving over the path it already
// lives at. Without this the clash rule would make saving twice impossible.
TEST(saveOverwritesTheDocumentsOwnPathRepeatedly) {
    TemporaryDirectory directory;
    const auto target = directory.path() / "notes.txt";
    writeOutOfBand(target, "first\n");

    auto runtime = makeRuntime(directory.path());
    ASSERT_TRUE(runtime != nullptr);
    ASSERT_TRUE(run(*runtime, "file.open", std::string{"notes.txt"}).accepted());

    ASSERT_TRUE(run(*runtime, "text.insert",
                    ssg::TextInputArguments{"second\n"})
                    .accepted());
    ASSERT_TRUE(run(*runtime, "file.save").accepted());
    ASSERT_TRUE(run(*runtime, "text.insert",
                    ssg::TextInputArguments{"third\n"})
                    .accepted());
    ASSERT_TRUE(run(*runtime, "file.save").accepted());

    const auto contents = readOutOfBand(target);
    ASSERT_TRUE(contents.find("second") != std::string::npos);
    ASSERT_TRUE(contents.find("third") != std::string::npos);
}

// Save-as to a FREE name must still work -- a clash rule that refused
// everything would pass the refusal tests above.
TEST(saveAsToAFreeNameSucceedsAndRetitlesTheTab) {
    TemporaryDirectory directory;
    auto runtime = makeRuntime(directory.path());
    ASSERT_TRUE(runtime != nullptr);

    ASSERT_TRUE(run(*runtime, "file.new").accepted());
    ASSERT_TRUE(run(*runtime, "text.insert", ssg::TextInputArguments{"hello"})
                    .accepted());
    ASSERT_TRUE(
        run(*runtime, "file.save_as", std::string{"fresh.txt"}).accepted());
    ASSERT_TRUE(fs::is_regular_file(directory.path() / "fresh.txt"));

    auto snapshot = runtime->snapshot(ssg::ClientId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    bool titled = false;
    for (const auto& tab : snapshot->sections().tabs.tabs) {
        if (tab.label.find("fresh.txt") != std::string::npos) titled = true;
    }
    ASSERT_TRUE(titled);
}

// Renaming to a free name must move the file and retitle the tab.
TEST(renameToAFreeNameMovesTheFileAndRetitlesTheTab) {
    TemporaryDirectory directory;
    writeOutOfBand(directory.path() / "before.txt", "content\n");

    auto runtime = makeRuntime(directory.path());
    ASSERT_TRUE(runtime != nullptr);
    ASSERT_TRUE(run(*runtime, "file.open", std::string{"before.txt"}).accepted());
    ASSERT_TRUE(
        run(*runtime, "file.rename", std::string{"after.txt"}).accepted());

    ASSERT_FALSE(fs::exists(directory.path() / "before.txt"));
    ASSERT_EQ(readOutOfBand(directory.path() / "after.txt"), "content\n");

    auto snapshot = runtime->snapshot(ssg::ClientId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    bool titled = false;
    for (const auto& tab : snapshot->sections().tabs.tabs) {
        if (tab.label.find("after.txt") != std::string::npos) titled = true;
    }
    ASSERT_TRUE(titled);
}

// Starting with no arguments must leave something editable on screen.
TEST(aRuntimeWithNoDocumentOpensAnEditableNewBuffer) {
    TemporaryDirectory directory;
    auto runtime = makeRuntime(directory.path());
    ASSERT_TRUE(runtime != nullptr);

    ASSERT_TRUE(run(*runtime, "file.new").accepted());

    auto snapshot = runtime->snapshot(ssg::ClientId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    const auto& tabs = snapshot->sections().tabs.tabs;
    ASSERT_EQ(tabs.size(), std::size_t{1});
    ASSERT_EQ(tabs.front().label, std::string{"[new buffer]"});

    // Editable, not merely present.
    ASSERT_TRUE(run(*runtime, "text.insert", ssg::TextInputArguments{"typed"})
                    .accepted());
    ASSERT_TRUE(runtime->activeDocumentText().find("typed") !=
                std::string::npos);
}

// Rolling back a rename must leave the workspace as it was: the file back at
// its old name AND NOTHING at the new one. This is the oracle that a
// placeholder-based clash check fails -- it makes the recovery snapshot record
// the placeholder as the destination's prior state, so a rollback restores an
// empty file where there had been none. Refusal tests cannot see that.
TEST(rollingBackARenameLeavesNothingAtTheNewName) {
    TemporaryDirectory directory;
    const auto recoveryRoot = directory.path() / "recovery";
    fs::create_directories(recoveryRoot);
    writeOutOfBand(directory.path() / "before.txt", "payload\n");

    auto recovery = ssg::RecoveryActions::create(recoveryRoot);
    auto workspace = ssg::Workspace::create(directory.path(), recovery);
    const auto opened = workspace.openFile("before.txt");
    ASSERT_TRUE(opened.accepted());

    const auto renamed = workspace.renameFile(*opened.document, "after.txt");
    ASSERT_TRUE(renamed.accepted());
    ASSERT_TRUE(fs::exists(directory.path() / "after.txt"));
    ASSERT_TRUE(renamed.compensation.has_value());
    if (!renamed.compensation) return;

    ASSERT_TRUE(workspace.restore(*renamed.compensation).accepted());
    ASSERT_EQ(readOutOfBand(directory.path() / "before.txt"), "payload\n");
    ASSERT_FALSE(fs::exists(directory.path() / "after.txt"));
}

}  // namespace

int main() {
    RUN(saveAsRefusesAnOccupiedDestinationAndLeavesItIntact);
    RUN(renameRefusesAnOccupiedDestinationAndLeavesBothFilesIntact);
    RUN(newDirectoryRefusesAnOccupiedName);
    RUN(saveOverwritesTheDocumentsOwnPathRepeatedly);
    RUN(saveAsToAFreeNameSucceedsAndRetitlesTheTab);
    RUN(renameToAFreeNameMovesTheFileAndRetitlesTheTab);
    RUN(aRuntimeWithNoDocumentOpensAnEditableNewBuffer);
    RUN(rollingBackARenameLeavesNothingAtTheNewName);
    std::cout << "Passed: " << passed << " Failed: " << failed << '\n';
    return failed == 0 ? 0 : 1;
}
