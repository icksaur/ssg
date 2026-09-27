#include <ssg/Editor.h>

#include "editor_test_support.h"
#include "grid_test_frame.h"
#include "test_helpers.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>


namespace {

namespace fs = std::filesystem;

fs::path uniqueRoot() {
    const auto root = testRuntimePath("dispatch_root");
    fs::remove_all(root);
    fs::create_directories(root / "recovery");
    return root;
}

std::unique_ptr<ssg::Editor> makeRuntime(fs::path const& root) {
    return std::move(ssg::createEditor(
                         {.cwd = root,
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

    ASSERT_TRUE(ssg::applyKeymapBind(
                    *runtime, {"Mod+KeyG", "oracle.late", "editor"})
                    .accepted);
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
    fs::remove_all(root);
}

TEST(keymapMutationsAdoptRoutingAndGenerationTogether) {
    const auto root = uniqueRoot();
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;

    int firstCalls = 0;
    int secondCalls = 0;
    runtime->addCommand("oracle.first", "First", [&] {
        ++firstCalls;
        return ssg::CommandResult{};
    });
    runtime->addCommand("oracle.second", "Second", [&] {
        ++secondCalls;
        return ssg::CommandResult{};
    });
    const ssg::KeyStroke key{ssg::KeyCode::KeyG, true};
    const auto initialGeneration = ssg::test::keymapGeneration(*runtime);

    ASSERT_TRUE(ssg::applyKeymapBind(
                    *runtime, {"Mod+KeyG", "oracle.first", "editor"})
                    .accepted);
    ASSERT_EQ(ssg::test::keymapGeneration(*runtime),
              initialGeneration + 1);
    ASSERT_EQ(runtime->input(ssg::ClientKeyInput{key, {}}).outcome,
              ssg::ClientInputOutcome::Dispatched);
    ASSERT_EQ(firstCalls, 1);

    ASSERT_TRUE(ssg::applyKeymapBind(
                    *runtime, {"Mod+KeyG", "oracle.second", "editor"})
                    .accepted);
    ASSERT_EQ(ssg::test::keymapGeneration(*runtime),
              initialGeneration + 2);
    ASSERT_EQ(runtime->input(ssg::ClientKeyInput{key, {}}).outcome,
              ssg::ClientInputOutcome::Dispatched);
    ASSERT_EQ(firstCalls, 1);
    ASSERT_EQ(secondCalls, 1);

    ASSERT_TRUE(ssg::applyKeymapUnbind(
                    *runtime, {"Mod+KeyG", "editor"})
                    .accepted);
    ASSERT_EQ(ssg::test::keymapGeneration(*runtime),
              initialGeneration + 3);
    ASSERT_EQ(runtime->input(ssg::ClientKeyInput{key, {}}).outcome,
              ssg::ClientInputOutcome::Unhandled);
    ASSERT_EQ(secondCalls, 1);

    ASSERT_TRUE(ssg::applyKeymapUnbind(
                    *runtime, {"Mod+KeyQ", "editor"})
                    .accepted);
    ASSERT_EQ(ssg::test::keymapGeneration(*runtime),
              initialGeneration + 4);

    ASSERT_TRUE(ssg::applyKeymapBind(
                    *runtime, {"Mod+KeyG", "oracle.second", "editor"})
                    .accepted);
    ASSERT_EQ(ssg::test::keymapGeneration(*runtime),
              initialGeneration + 5);

    ASSERT_FALSE(ssg::applyKeymapBind(
                     *runtime, {"NotAKey", "oracle.first", "editor"})
                     .accepted);
    ASSERT_FALSE(ssg::applyKeymapUnbind(
                     *runtime, {"NotAKey", "editor"})
                     .accepted);
    ASSERT_EQ(ssg::test::keymapGeneration(*runtime),
              initialGeneration + 5);
    ASSERT_EQ(runtime->input(ssg::ClientKeyInput{key, {}}).outcome,
              ssg::ClientInputOutcome::Dispatched);
    ASSERT_EQ(secondCalls, 2);

    runtime->resetKeymapToDefault();
    ASSERT_EQ(ssg::test::keymapGeneration(*runtime),
              initialGeneration + 6);
    ASSERT_EQ(runtime->input(ssg::ClientKeyInput{key, {}}).outcome,
              ssg::ClientInputOutcome::Unhandled);
    ASSERT_EQ(secondCalls, 2);

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

TEST(editReconciliationHonorsAcceptanceAndNotifiesFollowOnce) {
    const auto root = uniqueRoot();
    std::ofstream{root / "edit.txt"} << "original";
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    ASSERT_TRUE(ssg::test::openFile(*runtime, "edit.txt").accepted());

    const auto insert = [&] {
        return ssg::test::requireCommand(
            runtime->input(ssg::ClientKeyInput{{}, "x"}));
    };
    ASSERT_TRUE(runtime->follow.resume(runtime->diff.viewState()).accepted());
    const auto before = runtime->follow.viewState().generation;
    ASSERT_TRUE(insert().accepted());
    ASSERT_EQ(runtime->follow.viewState().mode, ssg::FollowMode::Paused);
    ASSERT_EQ(runtime->follow.viewState().generation, before + 1);

    ASSERT_TRUE(runtime->follow.resume(runtime->diff.viewState()).accepted());
    const auto beforeCommand = runtime->follow.viewState().generation;
    ASSERT_TRUE(runtime->dispatch("find.open").accepted());
    ASSERT_TRUE(runtime->dispatch("text.newline").accepted());
    ASSERT_EQ(runtime->follow.viewState().generation, beforeCommand + 1);
    ASSERT_FALSE(runtime->findView().open);

    ASSERT_TRUE(runtime->follow.resume(runtime->diff.viewState()).accepted());
    const auto beforeRejected = runtime->follow.viewState().generation;
    ASSERT_TRUE(runtime->dispatch("find.open").accepted());
    runtime->addCommand("oracle.rejected_edit", "Rejected edit", [&] {
        auto edited = ssg::applyEditorTextInput(
            *runtime, ssg::TextInputCommand::Insert, {"!"});
        ASSERT_TRUE(edited.accepted);
        return ssg::CommandResult{ssg::CommandError::HandlerFailed,
                                  "rejected after edit"};
    });
    ASSERT_FALSE(runtime->dispatch("oracle.rejected_edit").accepted());
    ASSERT_EQ(runtime->follow.viewState().generation, beforeRejected);
    ASSERT_FALSE(runtime->findView().open);

    runtime->addCommand("oracle.edit_twice", "Edit twice", [&] {
        ASSERT_TRUE(ssg::applyEditorTextInput(
            *runtime, ssg::TextInputCommand::Insert, {"a"}).accepted);
        ASSERT_TRUE(ssg::applyEditorTextInput(
            *runtime, ssg::TextInputCommand::Insert, {"b"}).accepted);
        return ssg::CommandResult{};
    });
    ASSERT_TRUE(runtime->dispatch("oracle.edit_twice").accepted());
    ASSERT_EQ(runtime->follow.viewState().generation, beforeRejected + 1);

    ASSERT_TRUE(runtime->dispatch("help.open").accepted());
    ASSERT_TRUE(runtime->follow.resume(runtime->diff.viewState()).accepted());
    const auto beforeReadOnly = runtime->follow.viewState().generation;
    ASSERT_FALSE(runtime->applyTextInput(
        ssg::TextInputCommand::Insert, {"rejected"}).accepted);
    ASSERT_EQ(runtime->follow.viewState().generation, beforeReadOnly);
    fs::remove_all(root);
}

TEST(fileActivationAndRejectedCommandsReconcileFind) {
    const auto root = uniqueRoot();
    std::ofstream{root / "edit.txt"} << "original";
    std::ofstream{root / "other.txt"} << "other";
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    ASSERT_TRUE(ssg::test::openFile(*runtime, "edit.txt").accepted());

    ASSERT_TRUE(runtime->dispatch("find.open").accepted());
    ASSERT_TRUE(runtime->findView().open);
    ASSERT_FALSE(runtime->dispatch("oracle.missing").accepted());
    ASSERT_TRUE(runtime->findView().open);
    ASSERT_TRUE(runtime->dispatch("file.new").accepted());
    ASSERT_FALSE(runtime->findView().open);

    ASSERT_TRUE(runtime->dispatch("find.open").accepted());
    ASSERT_TRUE(runtime->findView().open);
    ASSERT_TRUE(ssg::test::openFile(*runtime, "other.txt").accepted());
    ASSERT_FALSE(runtime->findView().open);

    ASSERT_TRUE(runtime->dispatch("find.open").accepted());
    const auto firstTab = runtime->tabs.viewState().tabs.front().id;
    auto rejected = runtime->input(ssg::TabPointerInput{ssg::TabId{99999}});
    ASSERT_EQ(rejected.outcome, ssg::ClientInputOutcome::Rejected);
    ASSERT_TRUE(runtime->findView().open);
    auto activated = runtime->input(ssg::TabPointerInput{firstTab});
    ASSERT_EQ(activated.outcome, ssg::ClientInputOutcome::Dispatched);
    ASSERT_FALSE(runtime->findView().open);
    fs::remove_all(root);
}

TEST(deferredFailureStillNotifiesAnAcceptedEditOnce) {
    const auto root = uniqueRoot();
    std::ofstream{root / "edit.txt"} << "original";
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    ASSERT_TRUE(ssg::test::openFile(*runtime, "edit.txt").accepted());

    runtime->addCommand("oracle.edit", "Edit", [&] {
        return ssg::applyEditorTextInput(
            *runtime, ssg::TextInputCommand::Insert, {"x"});
    });
    runtime->addCommand("oracle.fail", "Fail", [] {
        return ssg::CommandResult{ssg::CommandError::HandlerFailed, "failed"};
    });
    runtime->addCommand("oracle.queue", "Queue", [&] {
        ASSERT_TRUE(runtime->deferDispatch("oracle.edit"));
        ASSERT_TRUE(runtime->deferDispatch("oracle.fail"));
        return ssg::CommandResult{};
    });
    ASSERT_TRUE(runtime->follow.resume(runtime->diff.viewState()).accepted());
    const auto generation = runtime->follow.viewState().generation;
    ASSERT_FALSE(runtime->dispatch("oracle.queue").accepted());
    ASSERT_EQ(runtime->follow.viewState().generation, generation + 1);
    fs::remove_all(root);
}

TEST(deferredEditDetectsDocumentOpenedByPreviousCommand) {
    const auto root = uniqueRoot();
    std::ofstream{root / "first.txt"} << "first";
    std::ofstream{root / "second.txt"} << "second";
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    ASSERT_TRUE(ssg::test::openFile(*runtime, "first.txt").accepted());

    runtime->addCommand("oracle.edit_opened", "Edit opened file", [&] {
        return ssg::applyEditorTextInput(
            *runtime, ssg::TextInputCommand::Insert, {"!"});
    });
    runtime->addCommand("oracle.open_then_edit", "Open then edit", [&] {
        auto opened = runtime->workspace.openFile("second.txt");
        if (!opened.accepted() || !opened.document) {
            return ssg::CommandResult{ssg::CommandError::HandlerFailed,
                                      "failed to open second.txt"};
        }
        auto activated = runtime->activateDocument(*opened.document);
        if (!activated.accepted) {
            return ssg::CommandResult{ssg::CommandError::HandlerFailed,
                                      activated.message};
        }
        ASSERT_TRUE(runtime->deferDispatch("oracle.edit_opened"));
        return ssg::CommandResult{};
    });

    ASSERT_TRUE(runtime->follow.resume(runtime->diff.viewState()).accepted());
    const auto before = runtime->follow.viewState().generation;
    ASSERT_TRUE(runtime->dispatch("oracle.open_then_edit").accepted());
    ASSERT_EQ(runtime->activeDocument()->snapshot().text, std::string{"!second"});
    ASSERT_EQ(runtime->follow.viewState().mode, ssg::FollowMode::Paused);
    ASSERT_EQ(runtime->follow.viewState().generation, before + 1);
    fs::remove_all(root);
}

TEST(deferredFindNextCannotUseMatchesFromPreviousRevision) {
    const auto root = uniqueRoot();
    std::ofstream{root / "edit.txt"} << "cat cat";
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    ASSERT_TRUE(ssg::test::openFile(*runtime, "edit.txt").accepted());
    ASSERT_TRUE(runtime->dispatch("find.open").accepted());
    ASSERT_TRUE(runtime->updateFindQuery(ssg::test::promptText("cat")).accepted());
    ASSERT_EQ(runtime->findView().matches.size(), 2U);

    runtime->addCommand("oracle.edit", "Edit", [&] {
        return ssg::applyEditorTextInput(
            *runtime, ssg::TextInputCommand::Insert, {"!"});
    });
    runtime->addCommand("oracle.check_find", "Check find", [&] {
        ASSERT_FALSE(runtime->findView().open);
        return ssg::CommandResult{};
    });
    runtime->addCommand("oracle.edit_then_find", "Edit then find", [&] {
        ASSERT_TRUE(runtime->deferDispatch("oracle.edit"));
        ASSERT_TRUE(runtime->deferDispatch("oracle.check_find"));
        ASSERT_TRUE(runtime->deferDispatch("find.next"));
        return ssg::CommandResult{};
    });
    ASSERT_TRUE(runtime->dispatch("oracle.edit_then_find").accepted());
    ASSERT_FALSE(runtime->findView().open);
    fs::remove_all(root);
}

TEST(deferredEditNotifiesBeforeFollowResumeWithoutDuplicateNotification) {
    const auto root = uniqueRoot();
    std::ofstream{root / "edit.txt"} << "original";
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    ASSERT_TRUE(ssg::test::openFile(*runtime, "edit.txt").accepted());
    runtime->addCommand("oracle.edit_twice", "Edit twice", [&] {
        ASSERT_TRUE(ssg::applyEditorTextInput(
            *runtime, ssg::TextInputCommand::Insert, {"a"}).accepted);
        ASSERT_TRUE(ssg::applyEditorTextInput(
            *runtime, ssg::TextInputCommand::Insert, {"b"}).accepted);
        return ssg::CommandResult{};
    });
    runtime->addCommand("oracle.edit_then_resume", "Edit then resume", [&] {
        ASSERT_TRUE(runtime->deferDispatch("oracle.edit_twice"));
        ASSERT_TRUE(runtime->deferDispatch("follow_edits.resume"));
        return ssg::CommandResult{};
    });
    ASSERT_TRUE(runtime->follow.resume(runtime->diff.viewState()).accepted());
    const auto before = runtime->follow.viewState().generation;
    ASSERT_TRUE(runtime->dispatch("oracle.edit_then_resume").accepted());
    ASSERT_EQ(runtime->follow.viewState().mode, ssg::FollowMode::Following);
    ASSERT_EQ(runtime->follow.viewState().generation, before + 2);
    fs::remove_all(root);
}

TEST(pickerSubmitReconcilesItsCommandAndCloseAsOneOperation) {
    const auto root = uniqueRoot();
    std::ofstream{root / "edit.txt"} << "original";
    auto runtime = makeRuntime(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    ASSERT_TRUE(ssg::test::openFile(*runtime, "edit.txt").accepted());
    runtime->addCommand("oracle.picker_edit", "Picker edit", [&] {
        return ssg::applyEditorTextInput(
            *runtime, ssg::TextInputCommand::Insert, {"x"});
    });
    ASSERT_TRUE(runtime->dispatch("palette.open").accepted());
    const auto activation = runtime->screen.openPickerActivation();
    ASSERT_TRUE(activation.has_value());
    if (!activation) return;
    ASSERT_TRUE(runtime->follow.resume(runtime->diff.viewState()).accepted());
    const auto before = runtime->follow.viewState().generation;
    const auto rejected = runtime->input(
        ssg::PickerPointerInput{*activation, "oracle.not_published"});
    ASSERT_EQ(rejected.outcome, ssg::ClientInputOutcome::Rejected);
    ASSERT_EQ(runtime->follow.viewState().generation, before);
    const auto submitted = runtime->input(
        ssg::PickerPointerInput{*activation, "oracle.picker_edit"});
    ASSERT_EQ(submitted.outcome, ssg::ClientInputOutcome::Dispatched);
    ASSERT_EQ(runtime->follow.viewState().generation, before + 1);
    ASSERT_FALSE(runtime->screen.openPickerActivation().has_value());
    fs::remove_all(root);
}

}  // namespace

SSG_TEST_SUITE(test_command_dispatch) {
    RUN(viewActionsRemainExplicitAcrossEditorDispatch);
    RUN(keyBoundToUnknownIdWorksAfterRegistrationWithoutRebuildingTheBinding);
    RUN(keymapMutationsAdoptRoutingAndGenerationTogether);
    RUN(nestedDispatchIsRefusedAndDeferredIdsDrainInOrder);
    RUN(editorRefusesRegistryMutationDuringAHandler);
    RUN(editReconciliationHonorsAcceptanceAndNotifiesFollowOnce);
    RUN(fileActivationAndRejectedCommandsReconcileFind);
    RUN(deferredFailureStillNotifiesAnAcceptedEditOnce);
    RUN(deferredEditDetectsDocumentOpenedByPreviousCommand);
    RUN(deferredFindNextCannotUseMatchesFromPreviousRevision);
    RUN(deferredEditNotifiesBeforeFollowResumeWithoutDuplicateNotification);
    RUN(pickerSubmitReconcilesItsCommandAndCloseAsOneOperation);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
