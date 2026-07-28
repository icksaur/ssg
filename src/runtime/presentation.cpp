#include "editor_runtime_internal.h"


#include <algorithm>
#include <stdexcept>
#include <variant>

namespace ssg {
namespace {

template <typename T>
T const* payloadAs(std::any const& payload) { return std::any_cast<T>(&payload); }

bool syncTreeProviderToPanel(EditorRuntime::Impl& runtime) {
    const auto label = runtime.shell.activePanelProvider();
    if (label == "Files") {
        return runtime.tree.activateProvider(TreeProviderId{"filesystem"});
    } else if (label == "Git") {
        if (!runtime.tree.activateProvider(TreeProviderId{"git"})) {
            runtime.tree.replaceProvider(TreeProviderSnapshot{
                TreeProviderId{"git"}, TreeProviderKind::Git,
                TreeRevision{runtime.nextTreeRevision++}, {}});
            return runtime.tree.activateProvider(TreeProviderId{"git"});
        }
        return true;
    } else if (label == "Symbols") {
        if (!runtime.tree.activateProvider(TreeProviderId{"symbols"})) {
            runtime.tree.replaceProvider(TreeProviderSnapshot{
                TreeProviderId{"symbols"}, TreeProviderKind::Symbols,
                TreeRevision{runtime.nextTreeRevision++}, {}});
            return runtime.tree.activateProvider(TreeProviderId{"symbols"});
        }
        return true;
    }
    return false;
}

bool userNavigationShellCommand(std::string_view id) {
    return id == "pane.next" || id == "pane.previous" ||
           id == "pane.focus_left" || id == "pane.focus_right" ||
           id == "pane.focus_up" || id == "pane.focus_down";
}

CommandHandlerResult setWordWrap(EditorRuntime::Impl& runtime) {
    bool next = !boolSetting(runtime.settings, SettingKey::WordWrap, runtime.wordWrap);
    auto mutation = runtime.settings.set(SettingScope::Workspace, SettingKey::WordWrap, next);
    if (!mutation.accepted()) return failure(mutation.error->message);
    runtime.wordWrap = next;
    return success();
}

CommandHandlerResult scrollLines(EditorRuntime::Impl& runtime, std::any const& payload) {
    auto const* arguments = payloadAs<ScrollLinesArguments>(payload);
    if (arguments == nullptr) return failure("view.scroll_lines requires scroll-lines payload");
    // Clamped at the bottom only; the top clamp is deferred to the viewport,
    // which resolves it through ScrollOffset. Deliberate: bounding here would
    // need the document's total visual row count, which is O(document) when word
    // wrap is on, on a path that fires per wheel notch. The tree and picker DO
    // bound eagerly because their totals (node count, ranked size) are free.
    auto offset = ScrollOffset{runtime.requestedFirstVisualRow};
    offset.shiftUnbounded(arguments->rows);
    runtime.requestedFirstVisualRow = offset.firstVisible();
    runtime.selection.firstVisualRow = runtime.requestedFirstVisualRow;
    return success();
}

CommandHandlerResult scrollPages(EditorRuntime::Impl& runtime, std::any const& payload) {
    auto const* arguments = payloadAs<ScrollPagesArguments>(payload);
    if (arguments == nullptr) return failure("view.scroll_pages requires scroll-pages payload");
    // A page is the real pane height cached from the last snapshot, not a fake 24.
    // Same deferred-top-clamp discipline as scroll_lines above. The multiply is
    // saturated before it can overflow: `pages` is wire-decoded.
    auto const pageRows = static_cast<std::int64_t>(
        std::max<std::uint32_t>(runtime.lastPaneContentRows, 1));
    constexpr auto kLimit = std::numeric_limits<std::int64_t>::max();
    auto const delta = arguments->pages > kLimit / pageRows    ? kLimit
                       : arguments->pages < -kLimit / pageRows ? -kLimit
                                              : arguments->pages * pageRows;
    auto offset = ScrollOffset{runtime.requestedFirstVisualRow};
    offset.shiftUnbounded(delta);
    runtime.requestedFirstVisualRow = offset.firstVisible();
    runtime.selection.firstVisualRow = runtime.requestedFirstVisualRow;
    return success();
}

CommandHandlerResult scrollFraction(EditorRuntime::Impl& runtime, std::any const& payload) {
    auto const* arguments = payloadAs<ScrollFractionArguments>(payload);
    if (arguments == nullptr) return failure("view.scroll_to_fraction requires scroll-fraction payload");
    // Resolve maximum_first_row against the REAL pane cached from the last
    // snapshot, so a scrollbar drag to the bottom reaches the true last line on a
    // terminal that is not 24 rows tall (see doc/spec-scroll.md R6). Route through
    // the same wrap-gated viewport the snapshot uses so the drag maps to the same
    // total the scrollbar thumb was drawn from (M12).
    ViewportDimensions const viewport{
        std::max<std::uint32_t>(runtime.lastPaneContentColumns, 1),
        std::max<std::uint32_t>(runtime.lastPaneContentRows, 1)};
    auto view = runtime.computeEditorViewport(viewport, 0, 0);
    auto offset = ScrollOffset{runtime.requestedFirstVisualRow};
    offset.toFraction(arguments->numerator, arguments->denominator,
                      view.totalVisualRows, viewport.rows);
    runtime.requestedFirstVisualRow = offset.firstVisible();
    runtime.selection.firstVisualRow = runtime.requestedFirstVisualRow;
    return success();
}

CommandHandlerResult shellCommand(EditorRuntime::Impl& runtime, std::string_view id) {
    if (id == "pane.split_horizontal") runtime.shell.splitActive(SplitAxis::Horizontal);
    else if (id == "pane.split_vertical") runtime.shell.splitActive(SplitAxis::Vertical);
    else if (id == "pane.close") (void)runtime.shell.closeActivePane();
    else if (id == "pane.next") runtime.shell.nextPane();
    else if (id == "pane.previous") runtime.shell.previousPane();
    else if (id == "pane.focus_left") (void)runtime.shell.focusPane(PaneDirection::Left, runtime.shellView(ViewportDimensions{80, 24}));
    else if (id == "pane.focus_right") (void)runtime.shell.focusPane(PaneDirection::Right, runtime.shellView(ViewportDimensions{80, 24}));
    else if (id == "pane.focus_up") (void)runtime.shell.focusPane(PaneDirection::Up, runtime.shellView(ViewportDimensions{80, 24}));
    else if (id == "pane.focus_down") (void)runtime.shell.focusPane(PaneDirection::Down, runtime.shellView(ViewportDimensions{80, 24}));
    else if (id == "panel.toggle") runtime.shell.togglePanel();
    else if (id == "panel.focus") (void)runtime.shell.focusPanel();
    else if (id == "panel.show_files") {
        if (!runtime.shell.showPanelProvider("Files")) {
            return failure("files panel provider is unavailable");
        }
        if (!syncTreeProviderToPanel(runtime)) {
            return failure("files tree provider is unavailable");
        }
    } else if (id == "panel.show_git_status") {
        if (!runtime.shell.showPanelProvider("Git")) {
            return failure("git panel provider is unavailable");
        }
        if (!syncTreeProviderToPanel(runtime)) {
            return failure("git tree provider is unavailable");
        }
    }
    else if (id == "panel.next_provider") {
        runtime.shell.nextPanelProvider();
        if (!syncTreeProviderToPanel(runtime)) {
            return failure("next tree provider is unavailable");
        }
    } else if (id == "panel.previous_provider") {
        runtime.shell.previousPanelProvider();
        if (!syncTreeProviderToPanel(runtime)) {
            return failure("previous tree provider is unavailable");
        }
    }
    else if (id == "view.toggle_distraction_free") runtime.shell.toggleDistractionFree();
    else return failure("unknown shell command");
    return success();
}

CommandHandlerResult promptStatusCommand(EditorRuntime::Impl& runtime,
                                         Revision revision,
                                         std::string_view id,
                                         std::any const& payload) {
    if (id == "prompt.submit" || id == "prompt.cancel" ||
        id == "prompt.next" || id == "prompt.previous" ||
        id == "prompt.update_value") {
        auto const& request = runtime.prompt.request();
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
                    runtime, revision, command, payload);
            }
            if (id == "prompt.cancel") {
                return executeFindReplaceCommand(
                    runtime, revision, FindReplaceCommand::FindClose, payload);
            }
            if (id == "prompt.next") {
                return executeFindReplaceCommand(
                    runtime, revision, FindReplaceCommand::FindNext, payload);
            }
            return executeFindReplaceCommand(
                runtime, revision, FindReplaceCommand::FindPrevious, payload);
        }
        if (id == "prompt.submit") {
            auto result = runtime.prompt.submit();
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
                runtime.deferredCommands.push_back(
                    {std::nullopt,
                     ClientCommand{submission->commandId, revision,
                                   submission->values.front()}});
            }
            return success();
        }
        if (id == "prompt.update_value") {
            auto const* arguments = payloadAs<PromptValueArguments>(payload);
            if (arguments == nullptr) {
                return failure("prompt.update_value requires a value payload");
            }
            auto result =
                runtime.prompt.updateValue(arguments->index, arguments->value);
            return result.accepted() ? success() : failure(result.error->message);
        }
        if (id == "prompt.cancel") {
            auto result = runtime.prompt.cancel();
            return result.accepted() ? success() : failure(result.error->message);
        }
        return success();
    }
    if (id == "status.next") runtime.status.next();
    else if (id == "status.previous") runtime.status.previous();
    else if (id == "status.dismiss") runtime.status.dismiss();
    else if (id == "status.invoke_action") {
        auto const* invocation = payloadAs<StatusActionInvocation>(payload);
        if (invocation == nullptr) return failure("status.invoke_action requires an action payload");
        auto result = runtime.status.invokeAction(*invocation);
        return result.accepted() ? success() : failure("status action is unavailable");
    }
    return success();
}

std::string settingMessage(SettingMutation const& mutation) {
    return mutation.error ? mutation.error->message : "setting mutation failed";
}

void syncRuntimeSettings(EditorRuntime::Impl& runtime) {
    runtime.wordWrap = boolSetting(runtime.settings, SettingKey::WordWrap,
                                     runtime.wordWrap);
}

CommandHandlerResult settingsCommand(EditorRuntime::Impl& runtime, std::string_view id, std::any const& payload) {
    if (id == "settings.open") {
        (void)runtime.prompt.open(PromptRequest{
            PromptKind::Settings, "Settings",
            {{"settings.query", "Settings query", ""}}, {}, std::nullopt});
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
// in-process payload with no wire form, so each is declared with
// `inProcessHandler`: typed for the handler, absent from the protocol.
//
// Declaring each command with the type it actually consumes also removes the
// id-branching these handlers used to do.  A single `themeCommand(id, payload)`
// had to re-derive from the id which of two payload types it held; here the
// type is stated once, at the command, and the compiler carries it.
void registerAppearanceCommands(EditorSessionBuilder& builder,
                                EditorRuntime::Impl& runtime) {
    auto declare = [&](std::string_view owner, std::string id,
                       std::string summary) {
        return CommandSpecBuilder{std::move(id)}
            .owner(std::string{owner})
            .summary(std::move(summary))
            .mutates()
            .initScript();
    };

    builder.add(declare("theme-model", "theme.define", "Define")
                    .inProcessHandler<ThemeDefineArguments>(
                        [&runtime](CommandContext&,
                                   ThemeDefineArguments const& arguments) {
                            return runtime.runTransaction([&] {
                                auto result = applyThemeDefine(runtime.theme,
                                                               arguments);
                                if (!result.accepted()) {
                                    return failure(result.error->message);
                                }
                                runtime.theme = result.snapshot;
                                return success();
                            });
                        }));
    builder.add(declare("theme-model", "theme.background", "Background")
                    .inProcessHandler<ThemeBackgroundArguments>(
                        [&runtime](CommandContext&,
                                   ThemeBackgroundArguments const& arguments) {
                            return runtime.runTransaction([&] {
                                auto result =
                                    applyThemeBackground(runtime.theme,
                                                         arguments);
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
                                auto result = applyStyleDefine(runtime.style,
                                                               arguments);
                                if (!result.accepted()) {
                                    return failure(result.error->message);
                                }
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
                                return success();
                            });
                        }));
}

// Word wrap and the three ways to scroll.
//
// The scroll commands all record a user navigation when they move the view, so
// follow-edits knows the user drove rather than the editor.
void registerViewportCommands(EditorSessionBuilder& builder,
                              EditorRuntime::Impl& runtime) {
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

    auto scroll = [](std::string id, std::string summary) {
        return CommandSpecBuilder{std::move(id)}
            .owner("viewport-wrap-scrollbar")
            .summary(std::move(summary))
            .mutates()
            .lua();
    };
    builder.add(scroll("view.scroll_lines", "Scroll Lines")
                    .handler<ScrollLinesArguments>(
                        [&runtime](CommandContext& context,
                                   ScrollLinesArguments const& arguments) {
                            return runtime.runTransaction([&] {
                                auto result = scrollLines(runtime,
                                                          std::any{arguments});
                                if (result.accepted) {
                                    runtime.recordNavigation(
                                        context.principal().clientId(),
                                        NavigationClass::User);
                                }
                                return result;
                            });
                        }));
    builder.add(scroll("view.scroll_pages", "Scroll Pages")
                    .handler<ScrollPagesArguments>(
                        [&runtime](CommandContext& context,
                                   ScrollPagesArguments const& arguments) {
                            return runtime.runTransaction([&] {
                                auto result = scrollPages(runtime,
                                                          std::any{arguments});
                                if (result.accepted) {
                                    runtime.recordNavigation(
                                        context.principal().clientId(),
                                        NavigationClass::User);
                                }
                                return result;
                            });
                        }));
    builder.add(scroll("view.scroll_to_fraction", "Scroll To Fraction")
                    .handler<ScrollFractionArguments>(
                        [&runtime](CommandContext& context,
                                   ScrollFractionArguments const& arguments) {
                            return runtime.runTransaction([&] {
                                auto result =
                                    scrollFraction(runtime, std::any{arguments});
                                if (result.accepted) {
                                    runtime.recordNavigation(
                                        context.principal().clientId(),
                                        NavigationClass::User);
                                }
                                return result;
                            });
                        }));
}

// Reading and writing settings.
//
// settings.import_workspace carries a whole settings document from the prompt
// that collected it, and settings.set/reset carry typed mutations that a remote
// client may send.  The three that take nothing say so.
void registerSettingsCommands(EditorSessionBuilder& builder,
                              EditorRuntime::Impl& runtime) {
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
void registerPromptStatusCommands(EditorSessionBuilder& builder,
                                  EditorRuntime::Impl& runtime) {
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
                                     runtime, context.revision(), name, {});
                             });
                         });
        if (!label.empty()) built.label(std::move(label));
        builder.add(std::move(built));
    };

    bare("prompt.submit", "Submit Prompt", "Submit Prompt");
    bare("prompt.cancel", "Cancel", "");
    bare("prompt.next", "Next", "");
    bare("prompt.previous", "Previous", "");
    bare("status.next", "Next", "");
    bare("status.previous", "Previous", "");
    bare("status.dismiss", "Dismiss", "");

    builder.add(spec("status.invoke_action", "Invoke Action")
                    .optionalInProcessHandler<StatusActionInvocation>(
                        [&runtime](CommandContext& context,
                                   std::optional<StatusActionInvocation> const&
                                       invocation) {
                            return runtime.runTransaction([&] {
                                return promptStatusCommand(
                                    runtime, context.revision(),
                                    "status.invoke_action",
                                    invocation ? std::any{*invocation}
                                               : std::any{});
                            });
                        }));
    builder.add(spec("prompt.update_value", "Update Value")
                    .handler<PromptValueArguments>(
                        [&runtime](CommandContext& context,
                                   PromptValueArguments const& arguments) {
                            return runtime.runTransaction([&] {
                                return promptStatusCommand(
                                    runtime, context.revision(),
                                    "prompt.update_value", std::any{arguments});
                            });
                        }));
}

// Panes, the sidebar, and distraction-free mode.  None takes an argument; each
// acts on the current layout.
void registerShellLayoutCommands(EditorSessionBuilder& builder,
                                 EditorRuntime::Impl& runtime) {
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
                        auto result = shellCommand(runtime, name);
                        // Moving focus between panes is the user navigating,
                        // which pauses follow-edits; splitting or closing one
                        // is not.
                        if (result.accepted &&
                            userNavigationShellCommand(name)) {
                            runtime.recordNavigation(
                                context.principal().clientId(),
                                NavigationClass::User);
                        }
                        return result;
                    });
                });
        if (!label.empty()) built.label(std::move(label));
        builder.add(std::move(built));
    };

    declare("pane.split_horizontal", "", "Split Horizontal");
    declare("pane.split_vertical", "", "Split Vertical");
    declare("pane.close", "", "Close");
    declare("pane.next", "", "Next");
    declare("pane.previous", "", "Previous");
    declare("pane.focus_left", "", "Focus Left");
    declare("pane.focus_right", "", "Focus Right");
    declare("pane.focus_up", "", "Focus Up");
    declare("pane.focus_down", "", "Focus Down");
    declare("panel.toggle", "Toggle Sidebar", "Toggle Sidebar");
    declare("panel.focus", "Focus Sidebar", "Focus Sidebar");
    declare("panel.show_files", "Show Files Sidebar", "Show Files Sidebar");
    declare("panel.show_git_status", "Show Git Sidebar", "Show Git Sidebar");
    declare("panel.next_provider", "", "Next Provider");
    declare("panel.previous_provider", "", "Previous Provider");
    declare("view.toggle_distraction_free", "", "Toggle Distraction Free");
}

void bindRuntimePresentation(EditorSessionBuilder& builder, EditorRuntime::Impl& runtime) {
    registerViewportCommands(builder, runtime);
    registerShellLayoutCommands(builder, runtime);
    registerPromptStatusCommands(builder, runtime);
    registerSettingsCommands(builder, runtime);
    registerAppearanceCommands(builder, runtime);
}

} // namespace ssg
