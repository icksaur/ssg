#include "test_helpers.h"
#include "grid_test_frame.h"

#include <ssg/Editor.h>
#include <ssg/FileCommands.h>
#include <ssg/PromptSurface.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace {

namespace fs = std::filesystem;

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        path_ = testSystemRuntimePath(
            "path_prompt_" +
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
        root, root / "recovery", root / "archive"};
    config.enableGitDiffWorker = false;
    config.enableFilesystemWatcher = false;
    auto created = ssg::createEditor(config);
    if (!created.accepted()) return nullptr;
    auto runtime = std::move(created.session);
    return runtime;
}

ssg::CommandResult run(ssg::Editor& runtime, std::string id) {
    return runtime.dispatch(std::move(id));
}

bool updatePromptValue(ssg::Editor& runtime, std::size_t index, std::string value) {
    auto result = runtime.input(ssg::UpdatePromptValueInput{index, std::move(value)});
    return result.outcome != ssg::ClientInputOutcome::Rejected;
}

bool pathPromptOpen(ssg::Editor& runtime) {
    auto snapshot = ssg::test::projectGridFrame(runtime);
    if (!snapshot) return false;
    return snapshot->prompt.activeKind ==
           ssg::PromptKind::Path;
}

// The path-taking commands, named exactly. A count threshold would let the set
// shrink silently; naming them means removing one is a deliberate edit here.
constexpr std::string_view kPathCommands[] = {
    "workspace.open_directory", "file.open", "file.save_as", "file.rename",
    "file.new_directory",
};

bool isPathCommand(std::string_view id) {
    for (const auto candidate : kPathCommands) {
        if (candidate == id) return true;
    }
    return false;
}

// Every path-taking command must announce itself, and the flag must agree with
// the pathPrompt() accessor. Three independent expressions of the same fact
// catch a descriptor added to one and forgotten in the others.
TEST(pathPromptFlagAgreesWithThePathPromptAccessor) {
    std::size_t flagged = 0;
    for (const auto& descriptor : ssg::kFileCommands) {
        bool accessorAccepts = true;
        try {
            (void)ssg::fileCommandPathPrompt(descriptor.command);
        } catch (...) {
            accessorAccepts = false;
        }
        ASSERT_EQ(descriptor.pathPrompt, accessorAccepts);
        ASSERT_EQ(descriptor.pathPrompt, isPathCommand(descriptor.id));
        if (descriptor.pathPrompt) ++flagged;
    }
    ASSERT_EQ(flagged, std::size(kPathCommands));
}

// Dispatching a path-taking command with no payload must leave a path prompt
// open. Driven from the descriptor table, so a command that gains the flag
// without the wiring fails here rather than in front of a user.
TEST(everyPathCommandWithoutAPayloadOpensAPathPrompt) {
    std::size_t exercised = 0;

    for (const auto& descriptor : ssg::kFileCommands) {
        if (!descriptor.pathPrompt) continue;

        TemporaryDirectory directory;
        const auto seed = directory.path() / "seed.txt";
        std::ofstream{seed} << "seed\n";
        auto runtime = makeRuntime(directory.path());
        ASSERT_TRUE(runtime != nullptr);

        // rename needs a SAVED document; save_as needs any document; open and
        // new_directory need none. Opening a real file satisfies all three, so
        // the loop stays uniform.
    (void)ssg::applyFilePathCompletion(*runtime, ssg::PromptCompletion::FileOpen, "seed.txt");

        ASSERT_TRUE(run(*runtime, std::string{descriptor.id}).accepted());
        ASSERT_TRUE(pathPromptOpen(*runtime));
        ++exercised;
    }

    ASSERT_EQ(exercised, std::size(kPathCommands));
}

// The precondition must be checked BEFORE the prompt opens, so a user is never
// asked to type a name that cannot possibly be used. Without this, save_as with
// nothing open would prompt and then discard the typed name.
TEST(aPathCommandThatCannotRunRefusesInsteadOfPrompting) {
    TemporaryDirectory directory;
    auto runtime = makeRuntime(directory.path());
    ASSERT_TRUE(runtime != nullptr);

    // No document is open, so there is nothing to save or rename.
    ASSERT_FALSE(run(*runtime, "file.save_as").accepted());
    ASSERT_FALSE(pathPromptOpen(*runtime));
    ASSERT_FALSE(run(*runtime, "file.rename").accepted());
    ASSERT_FALSE(pathPromptOpen(*runtime));

    // An unnamed buffer has no file to rename, so rename must still refuse --
    // while save_as, which is how a buffer GETS a name, must now prompt.
    ASSERT_TRUE(run(*runtime, "file.new").accepted());
    ASSERT_FALSE(run(*runtime, "file.rename").accepted());
    ASSERT_FALSE(pathPromptOpen(*runtime));
    ASSERT_TRUE(run(*runtime, "file.save_as").accepted());
    ASSERT_TRUE(pathPromptOpen(*runtime));
}

// The load-bearing behavior: two different path commands prompted in turn must
// each apply their own typed file operation. A submit that used the wrong
// completion would apply the wrong operation, and the differing side effects
// (a directory versus a file) is what detects that.
TEST(submittingAPathPromptRunsTheOperationThatOpenedIt) {
    TemporaryDirectory directory;
    auto runtime = makeRuntime(directory.path());
    ASSERT_TRUE(runtime != nullptr);

    ASSERT_TRUE(run(*runtime, "file.new_directory").accepted());
    ASSERT_TRUE(pathPromptOpen(*runtime));
    ASSERT_TRUE(updatePromptValue(*runtime, 0, "made-by-prompt"));
    ASSERT_TRUE(run(*runtime, "prompt.submit").accepted());
    ASSERT_TRUE(fs::is_directory(directory.path() / "made-by-prompt"));
    ASSERT_FALSE(pathPromptOpen(*runtime));

    // The same submit machinery must now produce a FILE. If submit had
    // hard-coded new_directory, this would create a directory instead.
    ASSERT_TRUE(run(*runtime, "file.new").accepted());
    ASSERT_TRUE(run(*runtime, "file.save_as").accepted());
    ASSERT_TRUE(pathPromptOpen(*runtime));
    ASSERT_TRUE(updatePromptValue(*runtime, 0, "saved-by-prompt.txt"));
    ASSERT_TRUE(run(*runtime, "prompt.submit").accepted());
    ASSERT_TRUE(fs::is_regular_file(directory.path() / "saved-by-prompt.txt"));
}

// Saving a buffer that has never had a name is really a save-as, so it must
// prompt rather than fail. Observed through the effect: submitting a name saves
// the buffer under it.
TEST(savingAnUnnamedBufferPromptsAndThenSaves) {
    TemporaryDirectory directory;
    auto runtime = makeRuntime(directory.path());
    ASSERT_TRUE(runtime != nullptr);

    ASSERT_TRUE(run(*runtime, "file.new").accepted());
    ASSERT_TRUE(run(*runtime, "file.save").accepted());
    ASSERT_TRUE(pathPromptOpen(*runtime));

    ASSERT_TRUE(updatePromptValue(*runtime, 0, "named-at-save.txt"));
    ASSERT_TRUE(run(*runtime, "prompt.submit").accepted());
    ASSERT_TRUE(fs::is_regular_file(directory.path() / "named-at-save.txt"));
}

// Cancelling must not run the command. Without this, a cancel that shared
// submit's path would silently perform whatever the user backed out of.
TEST(cancellingAPathPromptRunsNothing) {
    TemporaryDirectory directory;
    auto runtime = makeRuntime(directory.path());
    ASSERT_TRUE(runtime != nullptr);

    ASSERT_TRUE(run(*runtime, "file.new_directory").accepted());
    ASSERT_TRUE(updatePromptValue(*runtime, 0, "never-created"));
    ASSERT_TRUE(run(*runtime, "prompt.cancel").accepted());

    ASSERT_FALSE(fs::exists(directory.path() / "never-created"));
    ASSERT_FALSE(pathPromptOpen(*runtime));
}

// An empty path must not reach the command: creating "" or saving over the
// workspace root are both nonsense the prompt has to refuse.
TEST(submittingAnEmptyPathDoesNotRunTheCommand) {
    TemporaryDirectory directory;
    auto runtime = makeRuntime(directory.path());
    ASSERT_TRUE(runtime != nullptr);

    ASSERT_TRUE(run(*runtime, "file.new_directory").accepted());
    ASSERT_FALSE(run(*runtime, "prompt.submit").accepted());
}

// Typing must land in a prompt the user is looking at, and only there.
TEST(updatingAValueWithNoPromptOpenIsRejected) {
    TemporaryDirectory directory;
    auto runtime = makeRuntime(directory.path());
    ASSERT_TRUE(runtime != nullptr);

    ASSERT_FALSE(updatePromptValue(*runtime, 0, "stray"));
}

}  // namespace

SSG_TEST_SUITE(test_path_prompt) {
    RUN(pathPromptFlagAgreesWithThePathPromptAccessor);
    RUN(everyPathCommandWithoutAPayloadOpensAPathPrompt);
    RUN(aPathCommandThatCannotRunRefusesInsteadOfPrompting);
    RUN(submittingAPathPromptRunsTheOperationThatOpenedIt);
    RUN(savingAnUnnamedBufferPromptsAndThenSaves);
    RUN(cancellingAPathPromptRunsNothing);
    RUN(submittingAnEmptyPathDoesNotRunTheCommand);
    RUN(updatingAValueWithNoPromptOpenIsRejected);
    std::cout << "Passed: " << passed << " Failed: " << failed << '\n';
    return failed == 0 ? 0 : 1;
}
