#include <ssg/Editor.h>


#include <algorithm>
#include <limits>
#include <array>
#include <stdexcept>
#include <variant>

namespace ssg {
namespace {

OperationResult setWordWrap(Editor& runtime) {
    bool next = !boolSetting(runtime.settings, SettingKey::WordWrap, runtime.wordWrap);
    auto mutation = runtime.settings.set(SettingScope::Workspace, SettingKey::WordWrap, next);
    if (!mutation.accepted()) return failure(mutation.error->message);
    runtime.wordWrap = next;
    return success();
}

OperationResult setLineNumbers(Editor& runtime) {
    bool next = !boolSetting(runtime.settings, SettingKey::LineNumbers,
                             runtime.lineNumbers);
    auto mutation = runtime.settings.set(SettingScope::Workspace,
                                         SettingKey::LineNumbers, next);
    if (!mutation.accepted()) return failure(mutation.error->message);
    runtime.lineNumbers = next;
    return success();
}

OperationResult resizePanel(Editor& runtime, bool grow) {
    if (!runtime.screen.showPanel()) {
        return failure("sidebar is unavailable");
    }
    const int minimumWidth = std::max(
        runtime.style.dimensions.panelMinimumWidth, kPanelMinimumWidth);
    const int current = std::max(runtime.style.dimensions.panelTargetWidth,
                                 minimumWidth);
    const int next =
        grow ? (current == std::numeric_limits<int>::max() ? current
                                                           : current + 1)
             : std::max(current - 1, minimumWidth);
    if (next == runtime.style.dimensions.panelTargetWidth) return success();
    runtime.style.dimensions.panelTargetWidth = next;
    runtime.rebuildInteractionSchema(runtime.style.dimensions,
                                     runtime.style.inputLineSigil);
    return success();
}

OperationResult shellCommand(Editor& runtime, std::string_view id) {
    if (id == "panel.toggle") (void)runtime.screen.togglePanel();
    else if (id == "panel.focus") (void)runtime.screen.focusPanel();
    else if (id == "panel.toggle_focus") {
        if (runtime.screen.effectiveFocus() == FocusTarget::Panel) {
            runtime.screen.focusEditor();
        } else if (!runtime.screen.focusPanel()) {
            (void)runtime.screen.togglePanel();
            if (!runtime.screen.focusPanel()) {
                return failure("sidebar is unavailable");
            }
        }
    }
    else if (id == "panel.shrink") return resizePanel(runtime, false);
    else if (id == "panel.grow") return resizePanel(runtime, true);
    else if (id == "panel.show_files") {
        if (!runtime.screen.showPanelProvider(TreeProviderKind::Filesystem)) {
            return failure("Files sidebar is unavailable");
        }
    } else if (id == "panel.show_git_status") {
        if (!runtime.screen.showPanelProvider(TreeProviderKind::Git)) {
            return failure("Git sidebar is unavailable");
        }
    } else if (id == "panel.show_search") {
        if (!runtime.screen.showPanelProvider(TreeProviderKind::Search)) {
            return failure("Search sidebar is unavailable");
        }
    }
    else if (id == "panel.next_provider") {
        if (!runtime.screen.switchPanelProvider(CycleDirection::Next)) {
            return failure("next sidebar view is unavailable");
        }
    } else if (id == "panel.previous_provider") {
        if (!runtime.screen.switchPanelProvider(CycleDirection::Previous)) {
            return failure("previous sidebar view is unavailable");
        }
    }
    else if (id == "view.toggle_distraction_free")
        runtime.screen.toggleDistractionFree();
    else return failure("unknown shell command");
    return success();
}

OperationResult promptCommand(Editor& runtime, std::string_view id) {
    if (id == "prompt.submit" || id == "prompt.cancel" ||
        id == "prompt.next" || id == "prompt.previous") {
        auto const& request = runtime.screen.prompt().request();
        if (!request) {
            return failure("no active prompt");
        }
        if (request->kind == PromptKind::Find ||
            request->kind == PromptKind::Replace) {
            if (id == "prompt.submit") {
                auto command = request->kind == PromptKind::Replace
                                   ? FindReplaceCommand::ReplaceCurrent
                                   : FindReplaceCommand::FindNext;
                return executeFindReplaceCommand(runtime, command);
            }
            if (id == "prompt.cancel") {
                return executeFindReplaceCommand(
                    runtime, FindReplaceCommand::FindClose);
            }
            if (id == "prompt.next") {
                return executeFindReplaceCommand(
                    runtime, FindReplaceCommand::FindNext);
            }
            if (id == "prompt.previous") {
                return executeFindReplaceCommand(
                    runtime, FindReplaceCommand::FindPrevious);
            }
            return failure("command is not valid for a find/replace prompt");
        }
        if (id == "prompt.submit") {
            auto result = runtime.screen.prompt().submit();
            if (!result.accepted()) return failure(result.error->message);
            auto const& submission = result.submission;
            if (submission && submission->completion != PromptCompletion::None) {
                if (submission->values.empty() ||
                    submission->values.front().empty()) {
                    switch (submission->completion) {
                    case PromptCompletion::WorkspaceOpenDirectory:
                        return failure(
                            "workspace.open_directory requires a non-empty value");
                    case PromptCompletion::FileOpen:
                        return failure("file.open requires a non-empty value");
                    case PromptCompletion::FileSaveAs:
                        return failure("file.save_as requires a non-empty value");
                    case PromptCompletion::FileRename:
                        return failure("file.rename requires a non-empty value");
                    case PromptCompletion::FileNewDirectory:
                        return failure(
                            "file.new_directory requires a non-empty value");
                    case PromptCompletion::GotoLine:
                        return failure("goto.line requires a non-empty value");
                    case PromptCompletion::None:
                        break;
                    }
                }
                if (submission->completion == PromptCompletion::GotoLine) {
                    return applyGotoLine(runtime, submission->values.front());
                }
                return applyFilePathCompletion(runtime, submission->completion,
                                               submission->values.front());
            }
            return success();
        }
        if (id == "prompt.cancel") {
            auto result = runtime.screen.prompt().cancel();
            return result.accepted() ? success() : failure(result.error->message);
        }
        return success();
    }
    if (id == "prompt.focus_next_control") {
        auto result = runtime.screen.prompt().focusNextInput();
        return result.accepted() ? success() : failure(result.error->message);
    }
    return success();
}

OperationResult settingsCommand(Editor& runtime, std::string_view id) {
    if (id == "settings.open") {
        auto opened = openGenericPrompt(runtime.screen.prompt(), PromptRequest{
            PromptKind::Settings, "settings",
            {{"settings.query", "settings query", ""}}, {}, std::nullopt});
        if (!opened.accepted()) return failure(opened.error->message);
        return success();
    }
    if (id == "settings.export_workspace") {
        runtime.showStatus(runtime.settings.exportScope(SettingScope::Workspace));
        return success();
    }
    return failure("unknown settings command");
}




} // namespace

OperationResult applyUiNodeActivation(Editor& runtime, UiNodeId const& nodeId) {
    const UiSchema tree = runtime.projectedUiTree();
    const UiNode* node = findUiNode(tree, nodeId);
    if (!node || !isUiNodeVisible(tree, nodeId)) {
        return failure("UI activation target is not present");
    }
    const auto* leaf = std::get_if<UiLeaf>(&node->content);
    if (!node->resolved || !leaf) {
        return failure("UI activation target is not actionable");
    }

    if (leaf->widget.kind == WidgetKind::TextInput &&
        node->resolved->active.has_value()) {
        auto result = runtime.screen.prompt().focusInput(leaf->widget.id);
        return result.accepted() ? success() : failure(result.error->message);
    }
    if ((leaf->widget.kind != WidgetKind::Field &&
         leaf->widget.kind != WidgetKind::Checkbox) ||
        !node->resolved->command || node->resolved->command->empty()) {
        return failure("UI activation target is not actionable");
    }
    if (!runtime.commands.find(*node->resolved->command)) {
        return failure("UI activation target command is not registered");
    }
    auto result = runtime.dispatchLocked(*node->resolved->command);
    if (!result.accepted()) return failure(result.message);
    return {true, {}, std::move(result.viewAction)};
}


OperationResult applyThemeSet(Editor& runtime, ThemeSetArguments arguments) {
    auto result = runtime.theme.withOverrides(arguments);
    if (!result.accepted()) return failure(result.error->message);
    runtime.theme = result.snapshot;
    return success();
}

OperationResult applyStyleDefine(Editor& runtime, StyleDefineArguments arguments) {
    auto result = runtime.style.withDefine(arguments);
    if (!result.accepted()) return failure(result.error->message);
    runtime.rebuildInteractionSchema(result.style.dimensions,
                                    result.style.inputLineSigil);
    runtime.style = std::move(result.style);
    return success();
}

OperationResult applyKeymapBind(Editor& runtime, KeymapBindArguments arguments) {
    auto result = applyKeymapBind(runtime.keymap, arguments);
    if (!result.accepted()) return failure(result.error->message);
    runtime.keymap = std::move(result.keymap);
    ++runtime.keymapGeneration;
    return success();
}

OperationResult applyKeymapUnbind(Editor& runtime,
                                 KeymapUnbindArguments arguments) {
    auto result = applyKeymapUnbind(runtime.keymap, arguments);
    if (!result.accepted()) return failure(result.error->message);
    runtime.keymap = std::move(result.keymap);
    ++runtime.keymapGeneration;
    return success();
}

void registerViewportCommands(Commands& commands, Editor& runtime) {
    commands.add("view.toggle_word_wrap", "Toggle Word Wrap", [&runtime] {
            return setWordWrap(runtime);
    });
    commands.add("view.toggle_line_numbers", "Toggle Line Numbers", [&runtime] {
            return setLineNumbers(runtime);
    });
}

// Reading and writing settings. The no-argument commands open the settings
// prompt and export the workspace scope; typed mutations are applied directly
// through Editor members and do not go through the command catalog.
void registerSettingsCommands(Commands& commands, Editor& runtime) {
    auto run = [&runtime](std::string_view id) {
        return settingsCommand(runtime, id);
    };
    commands.add("settings.open", "Open Settings",
                 [run] { return run("settings.open"); });
    commands.add("settings.export_workspace", "Settings Export Workspace",
                 [run] { return run("settings.export_workspace"); });
}

void registerPromptCommands(Commands& commands, Editor& runtime) {
    auto declare = [&](std::string id, std::string label) {
        auto name = id;
        commands.add(std::move(id), std::move(label), [&runtime, name] {
            return promptCommand(runtime, name);
        });
    };
    declare("prompt.submit", "Submit Prompt");
    declare("prompt.cancel", "Prompt Cancel");
    declare("prompt.next", "Prompt Next");
    declare("prompt.previous", "Prompt Previous");
    declare("prompt.focus_next_control", "Prompt Focus Next Control");
}

// Panes, the sidebar, and distraction-free mode. None takes an argument.
void registerShellLayoutCommands(Commands& commands, Editor& runtime) {
    struct PaneMutationCommand {
        std::string_view id;
        std::string_view label;
        enum class Kind {
            SplitHorizontal,
            SplitVertical,
            Close,
            Next,
            Previous,
        } kind;
    };
    const std::array paneMutations{
        PaneMutationCommand{"pane.split_horizontal", "Split Horizontal",
                           PaneMutationCommand::Kind::SplitHorizontal},
        PaneMutationCommand{"pane.split_vertical", "Split Vertical",
                           PaneMutationCommand::Kind::SplitVertical},
        PaneMutationCommand{"pane.close", "Close",
                           PaneMutationCommand::Kind::Close},
        PaneMutationCommand{"pane.next", "Next",
                           PaneMutationCommand::Kind::Next},
        PaneMutationCommand{"pane.previous", "Previous",
                           PaneMutationCommand::Kind::Previous},
    };
    for (const auto& command : paneMutations) {
        commands.add(std::string{command.id}, "Pane " + std::string{command.label},
                     [&runtime, kind = command.kind] {
                    switch (kind) {
                        case PaneMutationCommand::Kind::SplitHorizontal:
                            return runtime.splitPane(SplitAxis::Horizontal);
                        case PaneMutationCommand::Kind::SplitVertical:
                            return runtime.splitPane(SplitAxis::Vertical);
                        case PaneMutationCommand::Kind::Close:
                            return runtime.closePane();
                        case PaneMutationCommand::Kind::Next:
                            return runtime.cyclePane(CycleDirection::Next);
                        case PaneMutationCommand::Kind::Previous:
                            return runtime.cyclePane(
                                CycleDirection::Previous);
                    }
                    return failure("unknown editor pane mutation");
        });
    }

    struct PaneFocusCommand {
        std::string_view id;
        std::string_view label;
        PaneDirection direction;
    };
    const std::array paneFocusCommands{
        PaneFocusCommand{"pane.focus_left", "Focus Left",
                        PaneDirection::Left},
        PaneFocusCommand{"pane.focus_right", "Focus Right",
                        PaneDirection::Right},
        PaneFocusCommand{"pane.focus_up", "Focus Up",
                        PaneDirection::Up},
        PaneFocusCommand{"pane.focus_down", "Focus Down",
                        PaneDirection::Down},
    };
    for (const auto& command : paneFocusCommands) {
        commands.add(std::string{command.id}, "Pane " + std::string{command.label},
                     [direction = command.direction] {
                    return OperationResult{
                        true, {}, ViewAction{ResolvePaneFocus{direction}}};
        });
    }

    auto declare = [&](std::string id, std::string label) {
        auto name = id;
        commands.add(std::move(id), std::move(label), [&runtime, name] {
            return shellCommand(runtime, name);
        });
    };

    declare("panel.toggle", "Toggle Sidebar");
    declare("panel.focus", "Focus Sidebar");
    declare("panel.toggle_focus", "Toggle Sidebar Focus");
    declare("panel.shrink", "Shrink Sidebar");
    declare("panel.grow", "Grow Sidebar");
    declare("panel.show_files", "Show Files Sidebar");
    declare("panel.show_git_status", "Show Git Sidebar");
    declare("panel.show_search", "Show Search Sidebar");
    declare("panel.next_provider", "Panel Next Provider");
    declare("panel.previous_provider", "Panel Previous Provider");
    declare("view.toggle_distraction_free", "View Toggle Distraction Free");
}

void bindRuntimePresentation(Commands& commands, Editor& runtime) {
    registerViewportCommands(commands, runtime);
    registerShellLayoutCommands(commands, runtime);
    registerPromptCommands(commands, runtime);
    registerSettingsCommands(commands, runtime);
}

void registerAllCommands(Commands& commands, Editor& runtime) {
    bindRuntimeEditing(commands, runtime);
    bindRuntimeFiles(commands, runtime);
    bindRuntimePresentation(commands, runtime);
    bindRuntimeNavigation(commands, runtime);
    bindRuntimeLanguageServices(commands, runtime);
    bindRuntimeHelp(commands, runtime);
}

}  // namespace ssg
