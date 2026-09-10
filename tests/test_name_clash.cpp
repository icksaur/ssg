#include "test_helpers.h"
#include "grid_test_frame.h"

#include <ssg/Editor.h>
#include <ssg/FileCommands.h>
#include <ssg/PromptSurface.h>
#include <ssg/RecoveryManager.h>
#include <ssg/platform_files.h>
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

std::unique_ptr<ssg::Editor> makeRuntime(const fs::path& root) {
    ssg::EditorConfig config{
        root, root / "scratch", root / "recovery"};
    config.enableGitDiffWorker = false;
    config.enableFilesystemWatcher = false;
    auto created = ssg::createEditor(config);
    if (!created.accepted()) return nullptr;
    auto runtime = std::move(created.session);
    return runtime;
}

ssg::CommandResult run(ssg::Editor& runtime, std::string id,
                       std::any payload = {}) {
    return runtime.dispatch({std::move(id),  std::move(payload)});
}

// Forces the archive's copy to fail so the delete's abort path is reachable.
class FailArchiveCopy : public ssg::FileIoFaultInjector {
public:
    ssg::FileIoStatus beforeOperation(std::string_view operation,
                                      const fs::path&) override {
        return operation == "copyFileDurably" ? ssg::FileIoStatus::IoError
                                              : ssg::FileIoStatus::Ok;
    }
};

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

    auto snapshot = ssg::test::projectGridFrame(*runtime);
    ASSERT_TRUE(snapshot.has_value());
    bool titled = false;
    for (const auto& tab : snapshot->tabs.tabs) {
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

    auto snapshot = ssg::test::projectGridFrame(*runtime);
    ASSERT_TRUE(snapshot.has_value());
    bool titled = false;
    for (const auto& tab : snapshot->tabs.tabs) {
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

    auto snapshot = ssg::test::projectGridFrame(*runtime);
    ASSERT_TRUE(snapshot.has_value());
    const auto& tabs = snapshot->tabs.tabs;
    ASSERT_EQ(tabs.size(), std::size_t{1});
    ASSERT_EQ(tabs.front().label, std::string{"[new buffer]"});

    // Editable, not merely present.
    ASSERT_TRUE(run(*runtime, "text.insert", ssg::TextInputArguments{"typed"})
                    .accepted());
    ASSERT_TRUE(ssg::test::activeDocumentText(*runtime).find("typed") !=
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

    auto recovery = ssg::RecoveryManager::create(recoveryRoot);
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

// Deleting must leave the bytes recoverable. This is the invariant the whole
// archive exists for: the command takes no confirmation, so the only thing
// standing between a mistaken keystroke and permanent loss is this copy.
TEST(deletingAFileLeavesTheBytesInTheArchive) {
    TemporaryDirectory directory;
    const std::string payload = "irreplaceable\n";
    writeOutOfBand(directory.path() / "doomed.txt", payload);

    auto runtime = makeRuntime(directory.path());
    ASSERT_TRUE(runtime != nullptr);
    ASSERT_TRUE(run(*runtime, "file.open", std::string{"doomed.txt"}).accepted());
    ASSERT_TRUE(run(*runtime, "file.delete").accepted());

    ASSERT_FALSE(fs::exists(directory.path() / "doomed.txt"));

    const auto archiveRoot = directory.path() / ".ssg" / "archive";
    std::string recovered;
    for (const auto& entry : fs::recursive_directory_iterator(archiveRoot)) {
        if (entry.is_regular_file() &&
            entry.path().filename() == "doomed.txt") {
            recovered = readOutOfBand(entry.path());
        }
    }
    ASSERT_EQ(recovered, payload);
}

// The ordering rule. If the archive cannot be written the delete must FAIL and
// leave the file alone -- a delete never reduces the number of copies below
// one. Fault injection is what makes this reachable; without it the branch
// would only ever run on a full disk.
TEST(aFailedArchiveWriteAbortsTheDeleteAndKeepsTheFile) {
    TemporaryDirectory directory;
    const std::string payload = "must survive a failed archive\n";
    writeOutOfBand(directory.path() / "kept.txt", payload);

    auto runtime = makeRuntime(directory.path());
    ASSERT_TRUE(runtime != nullptr);
    ASSERT_TRUE(run(*runtime, "file.open", std::string{"kept.txt"}).accepted());

    FailArchiveCopy injector;
    auto* previous = ssg::installFileIoFaultInjector(&injector);
    const auto result = run(*runtime, "file.delete");
    (void)ssg::installFileIoFaultInjector(previous);

    ASSERT_FALSE(result.accepted());
    ASSERT_EQ(readOutOfBand(directory.path() / "kept.txt"), payload);
}

// The live-diff rule's classification, named exactly. Adding a FileCommand does
// NOT fail to compile (verified by perturbation: adding an enumerator built
// cleanly), so the set is pinned here instead -- a new command lands with
// mutatesActiveDocumentFile defaulting to false and must be considered.
constexpr std::string_view kActiveFileMutators[] = {
    "file.save", "file.save_as", "file.reload", "file.rename", "file.delete",
};

bool mutatesActiveFile(std::string_view id) {
    for (const auto candidate : kActiveFileMutators) {
        if (candidate == id) return true;
    }
    return false;
}

TEST(theActiveFileMutatorSetIsExactlyTheDeclaredOne) {
    std::size_t flagged = 0;
    for (const auto& descriptor : ssg::kFileCommands) {
        ASSERT_EQ(descriptor.mutatesActiveDocumentFile,
                  mutatesActiveFile(descriptor.id));
        if (descriptor.mutatesActiveDocumentFile) ++flagged;
    }
    ASSERT_EQ(flagged, std::size(kActiveFileMutators));
}

// Every command that mutates the active document's file needs a document to
// act on, and refuses without one. file.new and file.new_directory create
// something new and must NOT be caught by that rule.
TEST(activeFileMutatorsRefuseWithNoDocumentWhileCreatorsDoNot) {
    for (const auto& descriptor : ssg::kFileCommands) {
        if (!descriptor.mutatesActiveDocumentFile) continue;
        TemporaryDirectory directory;
        auto runtime = makeRuntime(directory.path());
        ASSERT_TRUE(runtime != nullptr);
        ASSERT_FALSE(run(*runtime, std::string{descriptor.id}).accepted());
    }

    TemporaryDirectory directory;
    auto runtime = makeRuntime(directory.path());
    ASSERT_TRUE(runtime != nullptr);
    ASSERT_TRUE(run(*runtime, "file.new").accepted());
    ASSERT_TRUE(
        run(*runtime, "file.new_directory", std::string{"made"}).accepted());
}

// A tab whose file has been deleted would offer editing and saving of
// something that no longer exists.
TEST(deletingAFileClosesItsTab) {
    TemporaryDirectory directory;
    writeOutOfBand(directory.path() / "doomed.txt", "bytes\n");
    auto runtime = makeRuntime(directory.path());
    ASSERT_TRUE(runtime != nullptr);
    ASSERT_TRUE(run(*runtime, "file.open", std::string{"doomed.txt"}).accepted());

    auto before = ssg::test::projectGridFrame(*runtime);
    ASSERT_TRUE(before.has_value());
    bool present = false;
    for (const auto& tab : before->tabs.tabs) {
        if (tab.label.find("doomed.txt") != std::string::npos) present = true;
    }
    ASSERT_TRUE(present);

    ASSERT_TRUE(run(*runtime, "file.delete").accepted());

    auto after = ssg::test::projectGridFrame(*runtime);
    ASSERT_TRUE(after.has_value());
    for (const auto& tab : after->tabs.tabs) {
        ASSERT_TRUE(tab.label.find("doomed.txt") == std::string::npos);
    }
}

// The live-diff rule against an ACTUAL live diff tab, not just the descriptor
// classification. A live diff tab is a computed view of two revisions, so
// there is no file to save, rename, reload or delete.
TEST(everyActiveFileMutatorIsRefusedInALiveDiffTab) {
    TemporaryDirectory directory;
    writeOutOfBand(directory.path() / "coexist.txt", "disk\n");
    auto runtime = makeRuntime(directory.path());
    ASSERT_TRUE(runtime != nullptr);
    ASSERT_TRUE(
        run(*runtime, "file.open", std::string{"coexist.txt"}).accepted());

    ASSERT_TRUE(ssg::test::applyGitDiffScan(*runtime,
                        {.revision = std::uint64_t{30},
                         .baselineIdentity = "head-x:index-1",
                         .files = {{.id = ssg::DiffFileId{"coexist-id"},
                                    .path = "coexist.txt",
                                    .baselineContent = std::string{"before\n"},
                                    .workingContent = std::string{"after\n"}}}})
                    .accepted());
    ASSERT_TRUE(run(*runtime, "panel.show_git_status").accepted());
    ASSERT_TRUE(run(*runtime, "tree.select_next").accepted());
    ASSERT_TRUE(run(*runtime, "tree.activate").accepted());

    auto snapshot = ssg::test::projectGridFrame(*runtime);
    ASSERT_TRUE(snapshot.has_value());
    bool liveDiffActive = false;
    for (const auto& tab : snapshot->tabs.tabs) {
        if (tab.kind == ssg::TabKind::LiveDiff &&
            snapshot->tabs.active == tab.id) {
            liveDiffActive = true;
        }
    }
    ASSERT_TRUE(liveDiffActive);

    // Driven from the descriptor set, so a command that gains the flag is
    // covered here without editing this test.
    for (const auto& descriptor : ssg::kFileCommands) {
        if (!descriptor.mutatesActiveDocumentFile) continue;
        ASSERT_FALSE(run(*runtime, std::string{descriptor.id}).accepted());
    }
    // The file is untouched by any of those refusals.
    ASSERT_EQ(readOutOfBand(directory.path() / "coexist.txt"),
              std::string{"disk\n"});
}

}  // namespace

SSG_TEST_SUITE(test_name_clash) {
    RUN(saveAsRefusesAnOccupiedDestinationAndLeavesItIntact);
    RUN(renameRefusesAnOccupiedDestinationAndLeavesBothFilesIntact);
    RUN(newDirectoryRefusesAnOccupiedName);
    RUN(saveOverwritesTheDocumentsOwnPathRepeatedly);
    RUN(saveAsToAFreeNameSucceedsAndRetitlesTheTab);
    RUN(renameToAFreeNameMovesTheFileAndRetitlesTheTab);
    RUN(aRuntimeWithNoDocumentOpensAnEditableNewBuffer);
    RUN(rollingBackARenameLeavesNothingAtTheNewName);
    RUN(deletingAFileLeavesTheBytesInTheArchive);
    RUN(aFailedArchiveWriteAbortsTheDeleteAndKeepsTheFile);
    RUN(theActiveFileMutatorSetIsExactlyTheDeclaredOne);
    RUN(activeFileMutatorsRefuseWithNoDocumentWhileCreatorsDoNot);
    RUN(deletingAFileClosesItsTab);
    RUN(everyActiveFileMutatorIsRefusedInALiveDiffTab);
    std::cout << "Passed: " << passed << " Failed: " << failed << '\n';
    return failed == 0 ? 0 : 1;
}
