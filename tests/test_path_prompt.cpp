#include "test_helpers.h"

#include <ssg/EditorRuntime.h>
#include <ssg/FileCommands.h>
#include <ssg/PromptSurface.h>

#include <chrono>
#include <filesystem>
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
        path_ = fs::temp_directory_path() /
                ("ssg-path-prompt-" +
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

// The prompt as the client sees it. commandId is deliberately NOT here -- it is
// runtime-internal attribution -- so these tests observe which command a prompt
// belongs to through what submitting it DOES, which is the stronger oracle.
bool pathPromptOpen(ssg::EditorRuntime& runtime) {
    auto snapshot = runtime.snapshot(ssg::ClientId{1}, {80, 24});
    if (!snapshot) return false;
    const auto& prompt = snapshot->sections().promptStatus.prompt;
    return prompt.has_value() && prompt->kind == ssg::PromptKind::Path;
}

// Every path-taking command must announce itself, and the flag must agree with
// the pathPrompt() accessor. Two independent expressions of the same fact catch
// a descriptor added to one and forgotten in the other.
TEST(pathPromptFlagAgreesWithThePathPromptAccessor) {
    const auto commands = ssg::fileCommandsCommandSet();
    std::size_t flagged = 0;
    for (const auto& descriptor : commands.descriptors()) {
        bool accessorAccepts = true;
        try {
            (void)commands.pathPrompt(descriptor.command);
        } catch (...) {
            accessorAccepts = false;
        }
        ASSERT_EQ(descriptor.pathPrompt, accessorAccepts);
        if (descriptor.pathPrompt) ++flagged;
    }
    // A guard over an empty set proves nothing.
    ASSERT_TRUE(flagged >= 3);
}

// Dispatching a path-taking command with no payload must leave a path prompt
// open. Driven from the descriptor table, so a command that gains the flag
// without the wiring fails here rather than in front of a user.
TEST(everyPathCommandWithoutAPayloadOpensAPathPrompt) {
    const auto commands = ssg::fileCommandsCommandSet();
    std::size_t exercised = 0;

    for (const auto& descriptor : commands.descriptors()) {
        if (!descriptor.pathPrompt) continue;

        TemporaryDirectory directory;
        auto runtime = makeRuntime(directory.path());
        ASSERT_TRUE(runtime != nullptr);

        // save_as and rename need a document to act on; open and new_directory
        // do not. Creating one unconditionally keeps the loop uniform.
        (void)run(*runtime, "file.new");

        ASSERT_TRUE(run(*runtime, std::string{descriptor.id}).accepted());
        ASSERT_TRUE(pathPromptOpen(*runtime));
        ++exercised;
    }

    ASSERT_TRUE(exercised >= 3);
}

// The load-bearing behavior: two different path commands prompted in turn must
// each re-dispatch THEMSELVES. A submit that ignored the attribution would send
// both to whichever command it hard-coded, and the differing side effects
// (a directory versus a file) is what detects that.
TEST(submittingAPathPromptRedispatchesTheCommandThatOpenedIt) {
    TemporaryDirectory directory;
    auto runtime = makeRuntime(directory.path());
    ASSERT_TRUE(runtime != nullptr);

    ASSERT_TRUE(run(*runtime, "file.new_directory").accepted());
    ASSERT_TRUE(pathPromptOpen(*runtime));
    ASSERT_TRUE(run(*runtime, "prompt.update_value",
                    ssg::PromptValueArguments{0, "made-by-prompt"})
                    .accepted());
    ASSERT_TRUE(run(*runtime, "prompt.submit").accepted());
    ASSERT_TRUE(fs::is_directory(directory.path() / "made-by-prompt"));
    ASSERT_FALSE(pathPromptOpen(*runtime));

    // The same submit machinery must now produce a FILE. If submit had
    // hard-coded new_directory, this would create a directory instead.
    ASSERT_TRUE(run(*runtime, "file.new").accepted());
    ASSERT_TRUE(run(*runtime, "file.save_as").accepted());
    ASSERT_TRUE(pathPromptOpen(*runtime));
    ASSERT_TRUE(run(*runtime, "prompt.update_value",
                    ssg::PromptValueArguments{0, "saved-by-prompt.txt"})
                    .accepted());
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

    ASSERT_TRUE(run(*runtime, "prompt.update_value",
                    ssg::PromptValueArguments{0, "named-at-save.txt"})
                    .accepted());
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
    ASSERT_TRUE(run(*runtime, "prompt.update_value",
                    ssg::PromptValueArguments{0, "never-created"})
                    .accepted());
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

    ASSERT_FALSE(run(*runtime, "prompt.update_value",
                     ssg::PromptValueArguments{0, "stray"})
                     .accepted());
}

}  // namespace

int main() {
    RUN(pathPromptFlagAgreesWithThePathPromptAccessor);
    RUN(everyPathCommandWithoutAPayloadOpensAPathPrompt);
    RUN(submittingAPathPromptRedispatchesTheCommandThatOpenedIt);
    RUN(savingAnUnnamedBufferPromptsAndThenSaves);
    RUN(cancellingAPathPromptRunsNothing);
    RUN(submittingAnEmptyPathDoesNotRunTheCommand);
    RUN(updatingAValueWithNoPromptOpenIsRejected);
    std::cout << "Passed: " << passed << " Failed: " << failed << '\n';
    return failed == 0 ? 0 : 1;
}
