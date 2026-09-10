#include <ssg/Editor.h>
#include <ssg/ScriptHost.h>

#include "grid_test_frame.h"
#include "test_helpers.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

namespace fs = std::filesystem;

fs::path uniqueRoot() {
    const auto root = testRuntimePath("dispatch_root_" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root / "scratch");
    fs::create_directories(root / "recovery");
    return root;
}

std::unique_ptr<ssg::Editor> makeRuntime(fs::path const& root) {
    return std::move(ssg::createEditor(
                         {.cwd = root,
                          .scratchRoot = root / "scratch",
                          .recoveryRoot = root / "recovery",
                          .enableGitDiffWorker = false,
                          .enableFilesystemWatcher = false})
                         .session);
}

TEST(viewActionsRemainExplicitAcrossEditorDispatch) {
    const auto root = uniqueRoot();
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;

    runtime->addCommand("oracle.view", "View", [] {
        return ssg::CommandResult{ssg::CommandError::None, {},
                                  ssg::ViewAction{ssg::ScrollPages{-2}}};
    });
    const auto result = runtime->dispatch("oracle.view");
    ASSERT_TRUE(result.accepted());
    ASSERT_FALSE(result.completed());
    ASSERT_EQ(result.viewAction, std::optional<ssg::ViewAction>{
                                     ssg::ScrollPages{-2}});
    fs::remove_all(root);
}

TEST(keyBoundToUnknownIdWorksAfterRegistrationWithoutRebuildingTheBinding) {
    const auto root = uniqueRoot();
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;

    auto bound = ssg::applyKeymapBind(
        runtime->keymap, {"Mod+KeyG", "oracle.late", "editor"});
    ASSERT_TRUE(bound.accepted());
    if (!bound.accepted()) return;
    runtime->keymap = std::move(bound.keymap);
    ++runtime->keymapGeneration;
    ssg::KeyStroke key{ssg::KeyCode::KeyG, true};
    auto unknown = runtime->input(ssg::ClientKeyInput{key, {}});
    ASSERT_EQ(unknown.outcome, ssg::ClientInputOutcome::Rejected);
    ASSERT_TRUE(unknown.command.has_value());
    if (unknown.command) {
        ASSERT_EQ(unknown.command->error, ssg::CommandError::UnknownCommand);
        ASSERT_TRUE(unknown.command->message.find("oracle.late") !=
                    std::string::npos);
    }

    int calls = 0;
    runtime->addCommand("oracle.late", "Late", [&] {
        ++calls;
        return ssg::CommandResult{};
    });
    auto registered = runtime->input(ssg::ClientKeyInput{key, {}});
    ASSERT_EQ(registered.outcome, ssg::ClientInputOutcome::Dispatched);
    ASSERT_EQ(calls, 1);
    ssg::ScriptHost scripts{*runtime};
    ASSERT_TRUE(scripts.evaluate("ssg.command('oracle.late')").accepted());
    ASSERT_EQ(calls, 2);
    fs::remove_all(root);
}

TEST(nestedDispatchIsRefusedAndDeferredIdsDrainInOrder) {
    const auto root = uniqueRoot();
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;

    ssg::CommandResult nested;
    std::vector<std::string> calls;
    runtime->addCommand("oracle.view_target", "View target", [&] {
        calls.emplace_back("view");
        return ssg::CommandResult{
            ssg::CommandError::None, {},
            ssg::ViewAction{ssg::ScrollPages{-2}}};
    });
    runtime->addCommand("oracle.target", "Target", [&] {
        calls.emplace_back("target");
        return ssg::CommandResult{};
    });
    runtime->addCommand("oracle.outer", "Outer", [&] {
        nested = runtime->dispatch("oracle.target");
        return runtime->deferDispatch("oracle.view_target") &&
                       runtime->deferDispatch("oracle.target")
                   ? ssg::CommandResult{}
                   : ssg::CommandResult{ssg::CommandError::HandlerFailed,
                                        "queue failed", {}};
    });

    const auto result = runtime->dispatch("oracle.outer");
    ASSERT_TRUE(result.accepted());
    ASSERT_EQ(result.viewAction,
              std::optional<ssg::ViewAction>{
                  ssg::ViewAction{ssg::ScrollPages{-2}}});
    ASSERT_EQ(nested.error, ssg::CommandError::HandlerFailed);
    ASSERT_EQ(nested.message, std::string{ssg::kNestedDispatchRefusal});
    ASSERT_EQ(calls, (std::vector<std::string>{"view", "target"}));
    fs::remove_all(root);
}

TEST(editorRefusesRegistryMutationDuringAHandler) {
    const auto root = uniqueRoot();
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;

    bool addRefused = false;
    bool replaceRefused = false;
    runtime->addCommand("oracle.mutates", "Mutates", [&] {
        try {
            runtime->addCommand("oracle.illegal", "Illegal", [] {
                return ssg::CommandResult{};
            });
        } catch (std::logic_error const&) {
            addRefused = true;
        }
        try {
            runtime->replaceCommands({}, {});
        } catch (std::logic_error const&) {
            replaceRefused = true;
        }
        return ssg::CommandResult{};
    });
    ASSERT_TRUE(runtime->dispatch("oracle.mutates").accepted());
    ASSERT_TRUE(addRefused);
    ASSERT_TRUE(replaceRefused);
    ASSERT_TRUE(runtime->commandRegistry().find("oracle.illegal") == nullptr);
    fs::remove_all(root);
}

}  // namespace

SSG_TEST_SUITE(test_command_dispatch) {
    RUN(viewActionsRemainExplicitAcrossEditorDispatch);
    RUN(keyBoundToUnknownIdWorksAfterRegistrationWithoutRebuildingTheBinding);
    RUN(nestedDispatchIsRefusedAndDeferredIdsDrainInOrder);
    RUN(editorRefusesRegistryMutationDuringAHandler);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
