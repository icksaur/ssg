#include <ssg/Editor.h>


#include <algorithm>
#include <limits>
#include <array>
#include <stdexcept>
#include <variant>

namespace ssg {
namespace {

CommandHandlerResult setWordWrap(Editor& runtime) {
    bool next = !boolSetting(runtime.settings, SettingKey::WordWrap, runtime.wordWrap);
    auto mutation = runtime.settings.set(SettingScope::Workspace, SettingKey::WordWrap, next);
    if (!mutation.accepted()) return failure(mutation.error->message);
    runtime.wordWrap = next;
    return success();
}

CommandHandlerResult setLineNumbers(Editor& runtime) {
    bool next = !boolSetting(runtime.settings, SettingKey::LineNumbers,
                             runtime.lineNumbers);
    auto mutation = runtime.settings.set(SettingScope::Workspace,
                                         SettingKey::LineNumbers, next);
    if (!mutation.accepted()) return failure(mutation.error->message);
    runtime.lineNumbers = next;
    return success();
}

CommandHandlerResult resizePanel(Editor& runtime, bool grow) {
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

CommandHandlerResult shellCommand(Editor& runtime,
                                  std::string_view id) {
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

CommandHandlerResult promptStatusCommand(Editor& runtime,
                                         std::string_view id,
                                         std::any const& payload) {
    if (id == "prompt.submit" || id == "prompt.cancel" ||
        id == "prompt.next" || id == "prompt.previous" ||
        id == "prompt.update_value") {
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
                return executeFindReplaceCommand(
                    runtime, command, payload);
            }
            if (id == "prompt.cancel") {
                return executeFindReplaceCommand(
                    runtime, FindReplaceCommand::FindClose,
                    payload);
            }
            if (id == "prompt.next") {
                return executeFindReplaceCommand(
                    runtime, FindReplaceCommand::FindNext,
                    payload);
            }
            if (id == "prompt.previous") {
                return executeFindReplaceCommand(
                    runtime, FindReplaceCommand::FindPrevious,
                    payload);
            }
            return failure("command is not valid for a find/replace prompt");
        }
        if (id == "prompt.submit") {
            auto result = runtime.screen.prompt().submit();
            if (!result.accepted()) return failure(result.error->message);
            // A prompt that names a command exists to collect that command's
            // argument, so submitting it runs the command. Deferred to the
            // dispatch wrapper because the session lock is non-reentrant.
            auto const& submission = result.submission;
            if (submission && !submission->commandId.empty()) {
                if (submission->values.empty() ||
                    submission->values.front().empty()) {
                    return failure(submission->commandId +
                                   " requires a non-empty value");
                }
                if (!runtime.deferDispatch(
                        ClientCommand{submission->commandId,
                                      submission->values.front()})) {
                    return failure("could not queue " + submission->commandId);
                }
            }
            return success();
        }
        if (id == "prompt.update_value") {
            auto const* arguments = payloadAs<PromptValueArguments>(payload);
            if (arguments == nullptr) {
                return failure("prompt.update_value requires a value payload");
            }
            auto result =
                runtime.screen.prompt().updateValue(arguments->index, arguments->value);
            return result.accepted() ? success() : failure(result.error->message);
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
    if (id == "status.next") runtime.status.next();
    else if (id == "status.previous") runtime.status.previous();
    else if (id == "status.dismiss") runtime.status.dismiss();
    return success();
}

std::string settingMessage(SettingMutation const& mutation) {
    return mutation.error ? mutation.error->message : "setting mutation failed";
}

void syncRuntimeSettings(Editor& runtime) {
    runtime.wordWrap = boolSetting(runtime.settings, SettingKey::WordWrap,
                                     runtime.wordWrap);
    runtime.lineNumbers = boolSetting(runtime.settings, SettingKey::LineNumbers,
                                      runtime.lineNumbers);
}

CommandHandlerResult settingsCommand(Editor& runtime, std::string_view id, std::any const& payload) {
    if (id == "settings.open") {
        auto opened = openGenericPrompt(runtime.screen.prompt(), PromptRequest{
            PromptKind::Settings, "settings",
            {{"settings.query", "settings query", ""}}, {}, std::nullopt});
        if (!opened.accepted()) return failure(opened.error->message);
        return success();
    }
    if (id == "settings.export_workspace") {
        runtime.enqueueStatus(StatusPriority::Information, runtime.settings.exportScope(SettingScope::Workspace));
        return success();
    }
    if (id == "settings.import_workspace") {
        auto const* document = payloadAs<std::string>(payload);
        if (document == nullptr) return failure("settings.import_workspace requires a document payload");
        auto result = runtime.settings.importScope(SettingScope::Workspace, *document);
        syncRuntimeSettings(runtime);
        return result.ok ? success() : failure(result.message);
    }
    if (id == "settings.set") {
        auto const* arguments = payloadAs<SettingSetArguments>(payload);
        if (arguments == nullptr) {
            return failure("settings.set requires a typed settings payload");
        }
        auto mutation = runtime.settings.set(arguments->scope, arguments->key,
                                             arguments->value);
        if (!mutation.accepted()) return failure(settingMessage(mutation));
        syncRuntimeSettings(runtime);
        return success();
    }
    if (id == "settings.reset") {
        auto const* arguments = payloadAs<SettingResetArguments>(payload);
        if (arguments == nullptr) {
            return failure("settings.reset requires a typed settings payload");
        }
        auto mutation = runtime.settings.reset(arguments->scope, arguments->key);
        if (!mutation.accepted()) return failure(settingMessage(mutation));
        syncRuntimeSettings(runtime);
        return success();
    }
    if (id == "settings.reset_scope") {
        auto const* arguments = payloadAs<SettingResetScopeArguments>(payload);
        if (arguments == nullptr) {
            return failure("settings.reset_scope requires a typed settings payload");
        }
        try {
            for (auto const& entry : runtime.settings.viewState().entries) {
                if (!runtime.settings.scopedValue(arguments->scope, entry.key)) continue;
                auto mutation = runtime.settings.reset(arguments->scope, entry.key);
                if (!mutation.accepted()) return failure(settingMessage(mutation));
            }
        } catch (std::invalid_argument const& error) {
            return failure(error.what());
        }
        syncRuntimeSettings(runtime);
        return success();
    }
    return failure(std::string{id} + " requires a typed settings payload");
}




} // namespace

CommandHandlerResult applyUiNodeActivation(Editor& runtime,
                                           UiNodeId const& nodeId) {
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
    const CommandEntry* command =
        runtime.catalog.find(*node->resolved->command);
    if (!command || command->argument.type ||
        command->effect == CommandEffect::Routing) {
        return failure("UI activation target is not a payloadless command");
    }
    auto result = runtime.dispatchLocked(ClientCommand{*node->resolved->command});
    if (!result.accepted()) return failure(result.message);
    if (result.viewAction) return CommandHandlerResult::requireView(std::move(*result.viewAction));
    return success();
}


// editor at startup.
//
// Each carries a whole table -- colours, a style value, a key sequence -- as an
// typed payload, so each is declared with `inProcessHandler`.
//
// Declaring each command with the type it actually consumes also removes the
// id-branching these handlers used to do.  A single `themeCommand(id, payload)`
// had to re-derive from the id which of two payload types it held; here the
// type is stated once, at the command, and the compiler carries it.
void registerAppearanceCommands(CommandCatalog& catalog,
                                Editor& runtime) {
    auto declare = [](std::string_view owner, std::string id,
                      std::string summary) {
        return CommandSpec{
            .id = std::move(id),
            .owner = std::string{owner},
            .summary = std::move(summary),
            .effect = CommandEffect::Mutation,
            .initScript = true,
        };
    };

    {
        auto built = declare("theme-model", "theme.set", "Set Colors");
        built.binding = bindInProcessHandler<ThemeSetArguments>(
            [&runtime](CommandContext&, ThemeSetArguments const& arguments) {
                auto result = runtime.theme.withOverrides(arguments);
                if (!result.accepted()) {
                    return failure(result.error->message);
                }
                runtime.theme = result.snapshot;
                return success();
            });
        catalog.add(std::move(built));
    }
    {
        auto built = declare("style-model", "style.define", "Define");
        built.binding = bindInProcessHandler<StyleDefineArguments>(
            [&runtime](CommandContext&, StyleDefineArguments const& arguments) {
                auto result = runtime.style.withDefine(arguments);
                if (!result.accepted()) {
                    return failure(result.error->message);
                }
                // Migrate the schema over the new dimensions FIRST, then
                // adopt the style, so a failure cannot leave layout and the
                // interaction schema inconsistent.
                runtime.rebuildInteractionSchema(
                    result.style.dimensions,
                    result.style.inputLineSigil);
                runtime.style = std::move(result.style);
                return success();
            });
        catalog.add(std::move(built));
    }
    {
        auto built = declare("keymap-model", "keymap.bind", "Bind");
        built.binding = bindInProcessHandler<KeymapBindArguments>(
            [&runtime](CommandContext&, KeymapBindArguments const& arguments) {
                auto result = applyKeymapBind(runtime.keymap, arguments);
                if (!result.accepted()) {
                    return failure(result.error->message);
                }
                runtime.keymap = std::move(result.keymap);
                ++runtime.keymapGeneration;
                return success();
            });
        catalog.add(std::move(built));
    }
    {
        auto built = declare("keymap-model", "keymap.unbind", "Unbind");
        built.binding = bindInProcessHandler<KeymapUnbindArguments>(
            [&runtime](CommandContext&,
                       KeymapUnbindArguments const& arguments) {
                auto result = applyKeymapUnbind(runtime.keymap, arguments);
                if (!result.accepted()) {
                    return failure(result.error->message);
                }
                runtime.keymap = std::move(result.keymap);
                ++runtime.keymapGeneration;
                return success();
            });
        catalog.add(std::move(built));
    }
}

void registerViewportCommands(CommandCatalog& catalog,
                              Editor& runtime) {
    catalog.add(CommandSpec{
        .id = "view.toggle_word_wrap",
        .owner = "viewport-wrap-scrollbar",
        .label = "Toggle Word Wrap",
        .summary = "Toggle Word Wrap",
        .effect = CommandEffect::Mutation,
        .luaApi = true,
        .binding = bindNoArgumentHandler([&runtime](CommandContext&) {
            return setWordWrap(runtime);
        }),
    });

    catalog.add(CommandSpec{
        .id = "view.toggle_line_numbers",
        .owner = "viewport-wrap-scrollbar",
        .label = "Toggle Line Numbers",
        .summary = "Toggle Line Numbers",
        .effect = CommandEffect::Mutation,
        .luaApi = true,
        .binding = bindNoArgumentHandler([&runtime](CommandContext&) {
            return setLineNumbers(runtime);
        }),
    });

}

// Reading and writing settings.
//
// settings.import_workspace carries a whole settings document from the prompt
// that collected it, and settings.set/reset carry typed mutations that a remote
// client may send.  The three that take nothing say so.
void registerSettingsCommands(CommandCatalog& catalog,
                              Editor& runtime) {
    auto declare = [](std::string id, std::string summary) {
        return CommandSpec{
            .id = std::move(id),
            .owner = "settings-model",
            .summary = std::move(summary),
            .effect = CommandEffect::Mutation,
            .luaApi = true,
        };
    };
    auto run = [&runtime](std::string_view id, std::any payload) {
        return settingsCommand(runtime, id, payload);
    };

    {
        auto built = declare("settings.open", "Open Settings");
        built.label = "Open Settings";
        built.binding = bindNoArgumentHandler(
            [run](CommandContext&) { return run("settings.open", {}); });
        catalog.add(std::move(built));
    }
    {
        auto built = declare("settings.export_workspace", "Export Workspace");
        built.binding = bindNoArgumentHandler([run](CommandContext&) {
            return run("settings.export_workspace", {});
        });
        catalog.add(std::move(built));
    }
    {
        auto built = declare("settings.import_workspace", "Import Workspace");
        built.binding = bindInProcessHandler<std::string>(
            [run](CommandContext&, std::string const& document) {
                return run("settings.import_workspace", std::any{document});
            });
        catalog.add(std::move(built));
    }
    {
        auto built = declare("settings.set", "Set");
        built.binding = bindWireHandler<SettingSetArguments>(
            [run](CommandContext&, SettingSetArguments const& arguments) {
                return run("settings.set", std::any{arguments});
            });
        catalog.add(std::move(built));
    }
    {
        auto built = declare("settings.reset", "Reset");
        built.binding = bindWireHandler<SettingResetArguments>(
            [run](CommandContext&, SettingResetArguments const& arguments) {
                return run("settings.reset", std::any{arguments});
            });
        catalog.add(std::move(built));
    }
    {
        auto built = declare("settings.reset_scope", "Reset Scope");
        built.binding = bindWireHandler<SettingResetScopeArguments>(
            [run](CommandContext&,
                  SettingResetScopeArguments const& arguments) {
                return run("settings.reset_scope", std::any{arguments});
            });
        catalog.add(std::move(built));
    }
}

// The prompt line and the status bar.  Only prompt.update_value carries
// anything: the text typed so far.
void registerPromptStatusCommands(CommandCatalog& catalog,
                                  Editor& runtime) {
    auto spec = [](std::string id, std::string summary) {
        return CommandSpec{
            .id = std::move(id),
            .owner = "prompt-status-surface",
            .summary = std::move(summary),
            .effect = CommandEffect::Mutation,
            .luaApi = true,
        };
    };
    auto bare = [&](std::string id, std::string summary, std::string label) {
        auto name = id;
        auto built = spec(std::move(id), std::move(summary));
        built.binding = bindNoArgumentHandler(
            [&runtime, name](CommandContext&) {
                return promptStatusCommand(runtime, name, {});
            });
        if (!label.empty()) built.label = std::move(label);
        catalog.add(std::move(built));
    };

    bare("prompt.submit", "Submit Prompt", "Submit Prompt");
    bare("prompt.cancel", "Cancel", "");
    bare("prompt.next", "Next", "");
    bare("prompt.previous", "Previous", "");
    bare("prompt.focus_next_control", "Focus Next Field", "");
    bare("status.next", "Next", "");
    bare("status.previous", "Previous", "");
    bare("status.dismiss", "Dismiss", "");

    {
        auto built = spec("prompt.update_value", "Update Value");
        built.binding = bindWireHandler<PromptValueArguments>(
            [&runtime](CommandContext&,
                       PromptValueArguments const& arguments) {
                return promptStatusCommand(
                    runtime, "prompt.update_value", std::any{arguments});
            });
        catalog.add(std::move(built));
    }
}

// Panes, the sidebar, and distraction-free mode. None takes an argument.
void registerShellLayoutCommands(CommandCatalog& catalog,
                                 Editor& runtime) {
    struct PaneMutationCommand {
        std::string_view id;
        std::string_view summary;
        enum class Kind {
            SplitHorizontal,
            SplitVertical,
            Close,
            Next,
            Previous,
        } kind;
    };
    const std::array paneMutations{
        PaneMutationCommand{"pane.split_horizontal", "Split Editor Horizontally",
                           PaneMutationCommand::Kind::SplitHorizontal},
        PaneMutationCommand{"pane.split_vertical", "Split Editor Vertically",
                           PaneMutationCommand::Kind::SplitVertical},
        PaneMutationCommand{"pane.close", "Close Editor Pane",
                           PaneMutationCommand::Kind::Close},
        PaneMutationCommand{"pane.next", "Next Editor Pane",
                           PaneMutationCommand::Kind::Next},
        PaneMutationCommand{"pane.previous", "Previous Editor Pane",
                           PaneMutationCommand::Kind::Previous},
    };
    for (const auto& command : paneMutations) {
        catalog.add(CommandSpec{
            .id = std::string{command.id},
            .owner = "shell-layout",
            .summary = std::string{command.summary},
            .effect = CommandEffect::Mutation,
            .luaApi = true,
            .binding = bindNoArgumentHandler(
                [&runtime, kind = command.kind](CommandContext&) {
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
                }),
        });
    }

    struct PaneFocusCommand {
        std::string_view id;
        std::string_view summary;
        PaneDirection direction;
    };
    const std::array paneFocusCommands{
        PaneFocusCommand{"pane.focus_left", "Focus Editor Pane Left",
                        PaneDirection::Left},
        PaneFocusCommand{"pane.focus_right", "Focus Editor Pane Right",
                        PaneDirection::Right},
        PaneFocusCommand{"pane.focus_up", "Focus Editor Pane Up",
                        PaneDirection::Up},
        PaneFocusCommand{"pane.focus_down", "Focus Editor Pane Down",
                        PaneDirection::Down},
    };
    for (const auto& command : paneFocusCommands) {
        catalog.add(CommandSpec{
            .id = std::string{command.id},
            .owner = "shell-layout",
            .summary = std::string{command.summary},
            .effect = CommandEffect::ViewAction,
            .binding = bindNoArgumentHandler(
                [direction = command.direction](CommandContext&) {
                    return CommandHandlerResult::requireView(
                        ResolvePaneFocus{direction});
                }),
        });
    }

    auto declare = [&](std::string id, std::string label, std::string summary) {
        auto name = id;
        CommandSpec built{
            .id = std::move(id),
            .owner = "shell-layout",
            .summary = std::move(summary),
            .effect = CommandEffect::Mutation,
            .luaApi = true,
            .binding = bindNoArgumentHandler(
                [&runtime, name](CommandContext&) {
                    return shellCommand(runtime, name);
                }),
        };
        if (!label.empty()) built.label = std::move(label);
        catalog.add(std::move(built));
    };

    declare("panel.toggle", "Toggle Sidebar", "Toggle Sidebar");
    declare("panel.focus", "Focus Sidebar", "Focus Sidebar");
    declare("panel.toggle_focus", "Toggle Sidebar Focus",
            "Toggle Sidebar Focus");
    declare("panel.shrink", "Shrink Sidebar", "Shrink Sidebar");
    declare("panel.grow", "Grow Sidebar", "Grow Sidebar");
    declare("panel.show_files", "Show Files Sidebar", "Show Files Sidebar");
    declare("panel.show_git_status", "Show Git Sidebar", "Show Git Sidebar");
    declare("panel.show_search", "Show Search Sidebar", "Show Search Sidebar");
    declare("panel.next_provider", "", "Next Sidebar View");
    declare("panel.previous_provider", "", "Previous Sidebar View");
    declare("view.toggle_distraction_free", "", "Toggle Distraction Free");
}

void bindRuntimePresentation(CommandCatalog& catalog, Editor& runtime) {
    registerViewportCommands(catalog, runtime);
    registerShellLayoutCommands(catalog, runtime);
    registerPromptStatusCommands(catalog, runtime);
    registerSettingsCommands(catalog, runtime);
    registerAppearanceCommands(catalog, runtime);
}

void registerAllCommands(CommandCatalog& catalog, Editor& runtime) {
    bindRuntimeEditing(catalog, runtime);
    bindRuntimeFiles(catalog, runtime);
    bindRuntimePresentation(catalog, runtime);
    bindRuntimeNavigation(catalog, runtime);
    bindRuntimeLanguageServices(catalog, runtime);
    bindRuntimeHelp(catalog, runtime);
}

}  // namespace ssg
