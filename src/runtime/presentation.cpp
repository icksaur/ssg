#include "editor_session_internal.h"


#include <algorithm>
#include <array>
#include <stdexcept>
#include <variant>

namespace ssg {
namespace {

const UiNode* schemaNode(const UiNode& node, const UiNodeId& id) {
    if (node.id == id) return &node;
    const auto* container = std::get_if<UiContainer>(&node.content);
    if (!container) return nullptr;
    for (const auto& child : container->children) {
        if (const auto* found = schemaNode(child, id)) return found;
    }
    return nullptr;
}

bool effectivelyPresent(const UiNode& node, const UiNodeId& id,
                        const UiPresenceSection& presence,
                        bool ancestorsPresent = true) {
    const auto record = std::find_if(
        presence.nodes.begin(), presence.nodes.end(),
        [&](const UiPresenceRecord& item) { return item.id == node.id; });
    if (record == presence.nodes.end()) return false;
    const bool present = ancestorsPresent && record->present;
    if (node.id == id) return present;
    const auto* container = std::get_if<UiContainer>(&node.content);
    if (!container) return false;
    return std::any_of(
        container->children.begin(), container->children.end(),
        [&](const UiNode& child) {
            return effectivelyPresent(child, id, presence, present);
        });
}

CommandHandlerResult activateUiNode(
    EditorSession::Impl& runtime, CommandContext& context,
    const UiNodeActivationArguments& arguments) {
    const UiFrame frame = runtime.sections().uiFrame;
    if (frame.version().generation != arguments.generation) {
        return failure("UI activation schema is stale");
    }
    const UiNode* node = schemaNode(frame.schema().root, arguments.nodeId);
    if (!node ||
        !effectivelyPresent(frame.schema().root, arguments.nodeId,
                            frame.presence())) {
        return failure("UI activation target is not present");
    }
    const auto state = std::find_if(
        frame.state().nodes.begin(), frame.state().nodes.end(),
        [&](const UiNodeState& item) { return item.id == arguments.nodeId; });
    const auto* leaf = std::get_if<UiLeaf>(&node->content);
    if (state == frame.state().nodes.end() || !state->leaf || !leaf) {
        return failure("UI activation target is not actionable");
    }

    ClientCommand target;
    target.baseRevision = context.revision();
    if (leaf->widget.kind == WidgetKind::TextInput &&
        state->leaf->active.has_value()) {
        target.id = "prompt.focus_control";
        target.payload = PromptFocusArguments{leaf->widget.id};
    } else {
        if ((leaf->widget.kind != WidgetKind::Field &&
             leaf->widget.kind != WidgetKind::Checkbox) ||
            !state->leaf->command || state->leaf->command->empty()) {
            return failure("UI activation target is not actionable");
        }
        const CommandEntry* command =
            runtime.catalog->find(*state->leaf->command);
        if (!command || command->argument.type ||
            command->effect == CommandEffect::Routing) {
            return failure(
                "UI activation target is not a payloadless command");
        }
        target.id = *state->leaf->command;
    }
    if (!runtime.defer(std::nullopt, std::move(target))) {
        return failure("UI activation target could not be queued");
    }
    return success();
}

CommandHandlerResult setWordWrap(EditorSession::Impl& runtime) {
    bool next = !boolSetting(runtime.settings, SettingKey::WordWrap, runtime.wordWrap);
    auto mutation = runtime.settings.set(SettingScope::Workspace, SettingKey::WordWrap, next);
    if (!mutation.accepted()) return failure(mutation.error->message);
    runtime.wordWrap = next;
    return success();
}

CommandHandlerResult setLineNumbers(EditorSession::Impl& runtime) {
    bool next = !boolSetting(runtime.settings, SettingKey::LineNumbers,
                             runtime.lineNumbers);
    auto mutation = runtime.settings.set(SettingScope::Workspace,
                                         SettingKey::LineNumbers, next);
    if (!mutation.accepted()) return failure(mutation.error->message);
    runtime.lineNumbers = next;
    return success();
}

CommandHandlerResult shellCommand(EditorSession::Impl& runtime,
                                  std::string_view id) {
    if (id == "panel.toggle") (void)runtime.interaction.apply(TogglePanel{});
    else if (id == "panel.focus") (void)runtime.interaction.focusPanel();
    else if (id == "panel.show_files") {
        if (!runtime.interaction.apply(ShowPanelProvider{
                builtInPanelTreeProvider(TreeProviderKind::Filesystem)})) {
            return failure("files tree provider is unavailable");
        }
    } else if (id == "panel.show_git_status") {
        if (!runtime.interaction.apply(ShowPanelProvider{
                builtInPanelTreeProvider(TreeProviderKind::Git)})) {
            return failure("git tree provider is unavailable");
        }
    }
    else if (id == "panel.next_provider") {
        const auto active = runtime.tree.activeProviderBinding();
        if (!active) return failure("no active tree provider");
        const TreeProviderBinding target =
            cyclePanelTreeProvider(*active, CycleDirection::Next);
        if (!runtime.interaction.apply(SwitchPanelProvider{target})) {
            return failure("next tree provider is unavailable");
        }
    } else if (id == "panel.previous_provider") {
        const auto active = runtime.tree.activeProviderBinding();
        if (!active) return failure("no active tree provider");
        const TreeProviderBinding target =
            cyclePanelTreeProvider(*active, CycleDirection::Previous);
        if (!runtime.interaction.apply(SwitchPanelProvider{target})) {
            return failure("previous tree provider is unavailable");
        }
    }
    else if (id == "view.toggle_distraction_free")
        runtime.interaction.toggleDistractionFree();
    else return failure("unknown shell command");
    return success();
}

CommandHandlerResult promptStatusCommand(EditorSession::Impl& runtime,
                                         ViewId viewId,
                                         Revision revision,
                                         std::string_view id,
                                         std::any const& payload) {
    if (id == "prompt.submit" || id == "prompt.cancel" ||
        id == "prompt.next" || id == "prompt.previous" ||
        id == "prompt.update_value") {
        auto const& request = runtime.interaction.prompt().request();
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
                    runtime, viewId, revision, command, payload);
            }
            if (id == "prompt.cancel") {
                return executeFindReplaceCommand(
                    runtime, viewId, revision, FindReplaceCommand::FindClose,
                    payload);
            }
            if (id == "prompt.next") {
                return executeFindReplaceCommand(
                    runtime, viewId, revision, FindReplaceCommand::FindNext,
                    payload);
            }
            if (id == "prompt.previous") {
                return executeFindReplaceCommand(
                    runtime, viewId, revision, FindReplaceCommand::FindPrevious,
                    payload);
            }
            return failure("command is not valid for a find/replace prompt");
        }
        if (id == "prompt.submit") {
            auto result = runtime.interaction.submitPrompt();
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
                if (!runtime.defer(
                        std::nullopt,
                        ClientCommand{submission->commandId, revision,
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
                runtime.interaction.updatePromptValue(arguments->index, arguments->value);
            return result.accepted() ? success() : failure(result.error->message);
        }
        if (id == "prompt.cancel") {
            auto result = runtime.interaction.cancelPrompt();
            return result.accepted() ? success() : failure(result.error->message);
        }
        return success();
    }
    if (id == "prompt.focus_control") {
        auto const* arguments = payloadAs<PromptFocusArguments>(payload);
        if (arguments == nullptr) {
            return failure("prompt.focus_control requires a focus payload");
        }
        auto result =
            runtime.interaction.focusPromptControl(arguments->controlId);
        return result.accepted() ? success() : failure(result.error->message);
    }
    if (id == "prompt.focus_next_control") {
        auto result = runtime.interaction.focusNextPromptControl();
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

void syncRuntimeSettings(EditorSession::Impl& runtime) {
    runtime.wordWrap = boolSetting(runtime.settings, SettingKey::WordWrap,
                                     runtime.wordWrap);
    runtime.lineNumbers = boolSetting(runtime.settings, SettingKey::LineNumbers,
                                      runtime.lineNumbers);
}

CommandHandlerResult settingsCommand(EditorSession::Impl& runtime, std::string_view id, std::any const& payload) {
    if (id == "settings.open") {
        auto opened = runtime.interaction.openPrompt(PromptRequest{
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

// Theme, style and keymap: the commands `init.lua` uses to configure the
// editor at startup.
//
// Each carries a whole table -- colours, a style value, a key sequence -- as an
// typed payload, so each is declared with `inProcessHandler`.
//
// Declaring each command with the type it actually consumes also removes the
// id-branching these handlers used to do.  A single `themeCommand(id, payload)`
// had to re-derive from the id which of two payload types it held; here the
// type is stated once, at the command, and the compiler carries it.
void registerAppearanceCommands(CommandCatalog& builder,
                                EditorSession::Impl& runtime) {
    auto declare = [&](std::string_view owner, std::string id,
                       std::string summary) {
        return CommandSpecBuilder{std::move(id)}
            .owner(std::string{owner})
            .summary(std::move(summary))
            .mutates()
            .initScript();
    };

    builder.add(declare("theme-model", "theme.set", "Set Colors")
                    .inProcessHandler<ThemeSetArguments>(
                        [&runtime](CommandContext&,
                                   ThemeSetArguments const& arguments) {
                            return runtime.runTransaction([&] {
                                auto result = runtime.theme.withOverrides(arguments);
                                if (!result.accepted()) {
                                    return failure(result.error->message);
                                }
                                runtime.theme = result.snapshot;
                                return success();
                            });
                        }));
    builder.add(declare("style-model", "style.define", "Define")
                    .inProcessHandler<StyleDefineArguments>(
                        [&runtime](CommandContext&,
                                   StyleDefineArguments const& arguments) {
                            return runtime.runTransaction([&] {
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
                        }));
    builder.add(declare("keymap-model", "keymap.bind", "Bind")
                    .inProcessHandler<KeymapBindArguments>(
                        [&runtime](CommandContext&,
                                   KeymapBindArguments const& arguments) {
                            return runtime.runTransaction([&] {
                                auto result = applyKeymapBind(runtime.keymap,
                                                              arguments);
                                if (!result.accepted()) {
                                    return failure(result.error->message);
                                }
                                runtime.keymap = std::move(result.keymap);
                                ++runtime.keymapGeneration;
                                return success();
                            });
                        }));
    builder.add(declare("keymap-model", "keymap.unbind", "Unbind")
                    .inProcessHandler<KeymapUnbindArguments>(
                        [&runtime](CommandContext&,
                                   KeymapUnbindArguments const& arguments) {
                            return runtime.runTransaction([&] {
                                auto result = applyKeymapUnbind(runtime.keymap,
                                                                arguments);
                                if (!result.accepted()) {
                                    return failure(result.error->message);
                                }
                                runtime.keymap = std::move(result.keymap);
                                ++runtime.keymapGeneration;
                                return success();
                            });
                        }));
}

// Word wrap and the three ways to scroll.
//
// The scroll commands all record a user navigation when they move the view, so
// follow-edits knows the user drove rather than the editor.
void registerViewportCommands(CommandCatalog& builder,
                              EditorSession::Impl& runtime) {
    builder.add(CommandSpecBuilder{"view.toggle_word_wrap"}
                    .owner("viewport-wrap-scrollbar")
                    .label("Toggle Word Wrap")
                    .summary("Toggle Word Wrap")
                    .mutates()
                    .lua()
                    .handler([&runtime](CommandContext&) {
                        return runtime.runTransaction(
                            [&] { return setWordWrap(runtime); });
                    }));

    builder.add(CommandSpecBuilder{"view.toggle_line_numbers"}
                    .owner("viewport-wrap-scrollbar")
                    .label("Toggle Line Numbers")
                    .summary("Toggle Line Numbers")
                    .mutates()
                    .lua()
                    .handler([&runtime](CommandContext&) {
                        return runtime.runTransaction(
                            [&] { return setLineNumbers(runtime); });
                    }));

    auto scroll = [](std::string id, std::string summary) {
        return CommandSpecBuilder{std::move(id)}
            .owner("viewport-wrap-scrollbar")
            .summary(std::move(summary))
            .viewAction()
            .lua();
    };
    builder.add(scroll("view.scroll_lines", "Scroll Lines")
                    .handler<ScrollLinesArguments>(
                        [](CommandContext&,
                           ScrollLinesArguments const& arguments) {
                            return CommandHandlerResult::requireView(
                                ViewScrollLines{ViewScrollTarget::Document,
                                                arguments.rows});
                        }));
    builder.add(scroll("view.scroll_pages", "Scroll Pages")
                    .handler<ScrollPagesArguments>(
                        [](CommandContext&,
                           ScrollPagesArguments const& arguments) {
                            return CommandHandlerResult::requireView(
                                ViewScrollPages{arguments.pages});
                        }));
    builder.add(scroll("view.scroll_to_fraction", "Scroll To Fraction")
                    .handler<ScrollFractionArguments>(
                        [](CommandContext&,
                           ScrollFractionArguments const& arguments) {
                            return CommandHandlerResult::requireView(
                                ViewScrollFraction{
                                    ViewScrollTarget::Document,
                                    arguments.numerator,
                                    arguments.denominator});
                        }));
}

// Reading and writing settings.
//
// settings.import_workspace carries a whole settings document from the prompt
// that collected it, and settings.set/reset carry typed mutations that a remote
// client may send.  The three that take nothing say so.
void registerSettingsCommands(CommandCatalog& builder,
                              EditorSession::Impl& runtime) {
    auto declare = [](std::string id, std::string summary) {
        return CommandSpecBuilder{std::move(id)}
            .owner("settings-model")
            .summary(std::move(summary))
            .mutates()
            .lua();
    };
    auto run = [&runtime](std::string_view id, std::any payload) {
        return runtime.runTransaction(
            [&] { return settingsCommand(runtime, id, payload); });
    };

    builder.add(declare("settings.open", "Open Settings")
                    .label("Open Settings")
                    .handler([run](CommandContext&) {
                        return run("settings.open", {});
                    }));
    builder.add(declare("settings.export_workspace", "Export Workspace")
                    .handler([run](CommandContext&) {
                        return run("settings.export_workspace", {});
                    }));
    builder.add(declare("settings.import_workspace", "Import Workspace")
                    .inProcessHandler<std::string>(
                        [run](CommandContext&, std::string const& document) {
                            return run("settings.import_workspace",
                                       std::any{document});
                        }));
    builder.add(declare("settings.set", "Set")
                    .handler<SettingSetArguments>(
                        [run](CommandContext&,
                              SettingSetArguments const& arguments) {
                            return run("settings.set", std::any{arguments});
                        }));
    builder.add(declare("settings.reset", "Reset")
                    .handler<SettingResetArguments>(
                        [run](CommandContext&,
                              SettingResetArguments const& arguments) {
                            return run("settings.reset", std::any{arguments});
                        }));
    builder.add(declare("settings.reset_scope", "Reset Scope")
                    .handler<SettingResetScopeArguments>(
                        [run](CommandContext&,
                              SettingResetScopeArguments const& arguments) {
                            return run("settings.reset_scope",
                                       std::any{arguments});
                        }));
}

// The prompt line and the status queue.  Only prompt.update_value carries
// anything: the text typed so far.
void registerPromptStatusCommands(CommandCatalog& builder,
                                  EditorSession::Impl& runtime) {
    auto spec = [](std::string id, std::string summary) {
        return CommandSpecBuilder{std::move(id)}
            .owner("prompt-status-surface")
            .summary(std::move(summary))
            .mutates()
            .lua();
    };
    auto bare = [&](std::string id, std::string summary, std::string label) {
        auto name = id;
        auto built = spec(std::move(id), std::move(summary))
                         .handler([&runtime, name](CommandContext& context) {
                             return runtime.runTransaction([&] {
                                 return promptStatusCommand(
                                     runtime, context.viewId(),
                                     context.revision(), name, {});
                             });
                         });
        if (!label.empty()) built.label(std::move(label));
        builder.add(std::move(built));
    };

    bare("prompt.submit", "Submit Prompt", "Submit Prompt");
    bare("prompt.cancel", "Cancel", "");
    bare("prompt.next", "Next", "");
    bare("prompt.previous", "Previous", "");
    bare("prompt.focus_next_control", "Focus Next Field", "");
    bare("status.next", "Next", "");
    bare("status.previous", "Previous", "");
    bare("status.dismiss", "Dismiss", "");

    builder.add(
        CommandSpecBuilder{"ui.activate"}
            .owner("ui-frame")
            .summary("Activate Published UI Node")
            .routes()
            .handler<UiNodeActivationArguments>(
                [&runtime](CommandContext& context,
                           const UiNodeActivationArguments& arguments) {
                    return activateUiNode(runtime, context, arguments);
                }));

    builder.add(spec("prompt.update_value", "Update Value")
                    .handler<PromptValueArguments>(
                        [&runtime](CommandContext& context,
                                   PromptValueArguments const& arguments) {
                            return runtime.runTransaction([&] {
                                return promptStatusCommand(
                                    runtime, context.viewId(),
                                    context.revision(),
                                    "prompt.update_value", std::any{arguments});
                            });
                        }));
    builder.add(spec("prompt.focus_control", "Focus Field")
                    .handler<PromptFocusArguments>(
                        [&runtime](CommandContext& context,
                                   PromptFocusArguments const& arguments) {
                            return runtime.runTransaction([&] {
                                return promptStatusCommand(
                                    runtime, context.viewId(),
                                    context.revision(),
                                    "prompt.focus_control", std::any{arguments});
                            });
                        }));
}

// Panes, the sidebar, and distraction-free mode. None takes an argument.
void registerShellLayoutCommands(CommandCatalog& builder,
                                 EditorSession::Impl& runtime) {
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
        builder.add(CommandSpecBuilder{std::string{command.id}}
                       .owner("shell-layout")
                       .summary(std::string{command.summary})
                       .mutates()
                       .lua()
                       .handler([&runtime, kind = command.kind](
                                    CommandContext& context) {
                           const auto client =
                               context.principal().clientId();
                           switch (kind) {
                               case PaneMutationCommand::Kind::SplitHorizontal:
                                   return runtime.splitPane(
                                       client, SplitAxis::Horizontal);
                               case PaneMutationCommand::Kind::SplitVertical:
                                   return runtime.splitPane(
                                       client, SplitAxis::Vertical);
                               case PaneMutationCommand::Kind::Close:
                                   return runtime.closePane(client);
                               case PaneMutationCommand::Kind::Next:
                                   return runtime.cyclePane(
                                       client, PaneCycleDirection::Next);
                               case PaneMutationCommand::Kind::Previous:
                                   return runtime.cyclePane(
                                       client, PaneCycleDirection::Previous);
                           }
                           return failure("unknown pane mutation");
                       }));
    }

    struct PaneFocusCommand {
        std::string_view id;
        std::string_view summary;
        PaneDirection direction;
    };
    const std::array paneFocusCommands{
        PaneFocusCommand{"pane.focus_left", "Focus Left",
                        PaneDirection::Left},
        PaneFocusCommand{"pane.focus_right", "Focus Right",
                        PaneDirection::Right},
        PaneFocusCommand{"pane.focus_up", "Focus Up", PaneDirection::Up},
        PaneFocusCommand{"pane.focus_down", "Focus Down",
                        PaneDirection::Down},
    };
    for (const auto& command : paneFocusCommands) {
        builder.add(
            CommandSpecBuilder{std::string{command.id}}
                .owner("shell-layout")
                .summary(std::string{command.summary})
                .viewAction()
                .handler([direction = command.direction](CommandContext&) {
                    return CommandHandlerResult::requireView(
                       ResolvePaneFocus{direction});
                }));
    }

    auto declare = [&](std::string id, std::string label, std::string summary) {
        auto name = id;
        auto built =
            CommandSpecBuilder{std::move(id)}
                .owner("shell-layout")
                .summary(std::move(summary))
                .mutates()
                .lua()
                .handler([&runtime, name](CommandContext& context) {
                    return runtime.runTransaction([&] {
                        return shellCommand(runtime, name);
                    });
                });
        if (!label.empty()) built.label(std::move(label));
        builder.add(std::move(built));
    };

    declare("panel.toggle", "Toggle Sidebar", "Toggle Sidebar");
    declare("panel.focus", "Focus Sidebar", "Focus Sidebar");
    declare("panel.show_files", "Show Files Sidebar", "Show Files Sidebar");
    declare("panel.show_git_status", "Show Git Sidebar", "Show Git Sidebar");
    declare("panel.next_provider", "", "Next Provider");
    declare("panel.previous_provider", "", "Previous Provider");
    declare("view.toggle_distraction_free", "", "Toggle Distraction Free");
}

void bindRuntimePresentation(CommandCatalog& builder, EditorSession::Impl& runtime) {
    registerViewportCommands(builder, runtime);
    registerShellLayoutCommands(builder, runtime);
    registerPromptStatusCommands(builder, runtime);
    registerSettingsCommands(builder, runtime);
    registerAppearanceCommands(builder, runtime);
}

} // namespace ssg
