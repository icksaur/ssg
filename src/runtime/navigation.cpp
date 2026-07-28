#include "editor_runtime_internal.h"

#include <ssg/CommandCatalog.h>

#include <algorithm>

namespace ssg {
namespace {

template <typename T>
T const* payloadAs(std::any const& payload) { return std::any_cast<T>(&payload); }

std::optional<TreeProviderId> panelProviderTreeId(std::string_view label) {
    if (label == "Files") return TreeProviderId{"filesystem"};
    if (label == "Git") return TreeProviderId{"git"};
    if (label == "Symbols") return TreeProviderId{"symbols"};
    return std::nullopt;
}

// Validates that the palette is open and that `command_id` is a member of the
// currently published palette candidate set (the command mode's candidates,
// which `palette_view()` publishes from `descriptors()`) and that the invoking
// principal holds its required capabilities.  On success the target id is
// stashed for the EditorRuntime dispatch wrapper to execute through the registry
// (the session mutex is non-reentrant, so the handler cannot re-enter dispatch).
// This keeps execution server-owned and rejects any id the palette never offered
// (see doc/spec-palette.md P1).
CommandHandlerResult validatePaletteTarget(EditorRuntime::Impl& runtime,
                                             CommandContext& context,
                                             std::string const& commandId) {
    bool const paletteOpen = runtime.prompt.active() && runtime.prompt.request() &&
                              runtime.prompt.request()->kind == PromptKind::Palette;
    if (!paletteOpen) return failure("palette.execute requires the palette to be open");
    // Every picker uses a Palette-kind prompt, so prompt kind alone no longer
    // identifies the command palette.  Without this the file picker's
    // candidates -- which are PATHS, not command ids -- would be submittable as
    // commands.
    if (runtime.openPicker != PickerKind::Command) {
        return failure("palette.execute requires the command palette to be open");
    }
    auto const candidates = runtime.descriptors();
    bool const published =
        std::any_of(candidates.begin(), candidates.end(),
                    [&](auto const& candidate) { return candidate.id == commandId; });
    if (!published) {
        return failure("command is not in the palette candidate set: " + commandId);
    }
    auto const* declared = runtime.session->catalog()->find(commandId);
    if (declared != nullptr) {
        for (auto const& capability : declared->requiredCapabilities) {
            if (!context.principal().hasCapability(CapabilityId{std::string{capability}})) {
                return failure("principal lacks capability for palette command: " +
                               commandId);
            }
        }
    }
    return success();
}

CommandHandlerResult searchCommand(EditorRuntime::Impl& runtime, CommandContext& context, std::string_view id, std::any const& payload) {
    Revision const revision = context.revision();
    if (id == "palette.open") {
        if (!runtime.openPickerPrompt(PickerKind::Command)) {
            return failure("could not open the command picker");
        }
    }
    else if (id == "file_finder.open") {
        if (!runtime.openPickerPrompt(PickerKind::File)) {
            return failure("could not open the file picker");
        }
    }
    else if (id == "file_finder.toggle_gitignore") {
        bool const next = !boolSetting(
            runtime.settings, SettingKey::FileFinderRespectGitignore, true);
        auto mutation = runtime.settings.set(
            SettingScope::Workspace, SettingKey::FileFinderRespectGitignore, next);
        if (!mutation.accepted()) return failure(mutation.error->message);
        // Toggling with the picker already open must re-walk, or the setting
        // appears to do nothing until the picker is reopened.
        if (runtime.openPicker == PickerKind::File) runtime.rebuildFileCandidates();
    }
    else if (id == "palette.close") { (void)runtime.prompt.cancel(); }
    else if (id == "palette.next" || id == "search.results_next") runtime.search.selectNext();
    else if (id == "palette.previous" || id == "search.results_previous") runtime.search.selectPrevious();
    else if (id == "palette.execute") {
        auto const* arguments = payloadAs<PaletteExecuteArguments>(payload);
        if (arguments == nullptr) return failure("palette.execute requires a command id payload");
        auto validation = validatePaletteTarget(runtime, context, arguments->commandId);
        if (!validation.accepted) return validation;
        runtime.pendingPaletteTarget = arguments->commandId;
        (void)runtime.prompt.cancel();
    } else if (id == "search.workspace") {
        std::string query;
        if (auto const* text = payloadAs<std::string>(payload)) query = *text;
        auto request = runtime.search.beginWorkspaceSearch(std::move(query), revision);
        auto batch = runtime.search.evaluate(request);
        (void)runtime.search.publish(batch, revision);
    } else if (id == "goto.back") {
        (void)runtime.navigation.back();
    } else if (id == "goto.forward") {
        (void)runtime.navigation.forward();
    } else if (id == "goto.file" || id == "goto.line" || id == "goto.symbol") {
        auto const* target = payloadAs<NavigationTarget>(payload);
        if (target != nullptr) (void)runtime.navigation.visit(*target, NavigationOrigin::User);
        else return failure(std::string{id} + " requires a navigation target payload");
    } else {
        return failure("unknown search command");
    }
    return success();
}

CommandHandlerResult treeCommand(EditorRuntime::Impl& runtime,
                                 CommandContext& context,
                                 std::string_view id,
                                 std::any const& payload) {
    if (auto providerId = panelProviderTreeId(runtime.shell.activePanelProvider())) {
        (void)runtime.tree.activateProvider(*providerId);
    }
    if (id == "tree.select_next") { (void)runtime.tree.selectNext(); runtime.revealTreeSelection(); return success(); }
    if (id == "tree.select_previous") { (void)runtime.tree.selectPrevious(); runtime.revealTreeSelection(); return success(); }
    if (id == "tree.activate") {
        auto treeView = runtime.tree.viewState();
        auto providerKind = treeView.providers.empty()
                                ? std::optional<TreeProviderKind>{}
                                : std::optional<TreeProviderKind>{
                                      treeView.providers.front().kind};
        auto selected = runtime.tree.selectedNode();
        if (!selected) return failure("no tree node is selected");
        if (selected->expandable) { (void)runtime.tree.toggleSelected(); runtime.revealTreeSelection(); return success(); }
        if (providerKind == TreeProviderKind::Git && selected->workspacePath) {
            const auto diffView = runtime.diff.viewState();
            auto file = std::find_if(
                diffView.files.begin(), diffView.files.end(),
                [&](const DiffFileView& candidate) {
                    return candidate.path.generic_string() == *selected->workspacePath;
                });
            if (file == diffView.files.end()) {
                return failure("failed to resolve git status item");
            }
            return runtime.openOrFocusLiveDiffTab(
                *file, NavigationClass::User,
                context.principal().clientId());
        }
        if (selected->workspacePath) {
            auto result = runtime.workspace.openFile(*selected->workspacePath);
            if (!result.accepted() || !result.document) return failure("failed to open tree file");
            auto opened = runtime.activateDocument(*result.document);
            if (opened.accepted) runtime.shell.focusEditor();
            return opened;
        }
        return success();
    }
    if (id == "tree.select") {
        auto const* arguments = payloadAs<TreeSelectArguments>(payload);
        if (arguments == nullptr) return failure("tree.select requires a node id payload");
        if (!runtime.tree.select(arguments->nodeId)) return failure("tree node is not selectable");
        runtime.revealTreeSelection();
        // Focus follows the pointer (M8-F): clicking a tree row acts on the panel,
        // so move keyboard focus there. (For a file click the app dispatches
        // tree.activate next, whose file-open focus_editor() then wins.)
        (void)runtime.shell.focusPanel();
        return success();
    }
    if (id == "tree.scroll") {
        auto const* arguments = payloadAs<ScrollLinesArguments>(payload);
        if (arguments == nullptr) return failure("tree.scroll requires a scroll-lines payload");
        runtime.scrollTree(arguments->rows);
        return success();
    }
    if (id == "tree.scroll_to_fraction") {
        auto const* arguments = payloadAs<ScrollFractionArguments>(payload);
        if (arguments == nullptr) return failure("tree.scroll_to_fraction requires a scroll-fraction payload");
        runtime.scrollTreeToFraction(arguments->numerator, arguments->denominator);
        return success();
    }
    auto const* invocation = payloadAs<TreeCommandInvocation>(payload);
    if (invocation == nullptr) return failure(std::string{id} + " requires a tree invocation payload");
    if (id == "tree.toggle_expanded") {
        auto toggled = runtime.tree.toggleExpanded(invocation->providerId, invocation->nodeId);
        if (toggled) runtime.revealTreeSelection();
        return toggled ? success() : failure("tree node does not exist");
    }
    auto command = runtime.tree.invokeNodeCommand(invocation->providerId, invocation->nodeId, invocation->commandId);
    return command ? success() : failure("tree node command does not exist");
}

CommandHandlerResult diffCommand(EditorRuntime::Impl& runtime, std::string_view id, std::any const& payload) {
    auto const* fileId = payloadAs<DiffFileId>(payload);
    if (fileId == nullptr) return failure(std::string{id} + " requires a diff file ID payload");
    auto file = runtime.diff.file(*fileId);
    if (!file) return failure("diff file does not exist");
    std::optional<std::size_t> hunk;
    const auto currentLine = runtime.activeDocument()
                                 ? std::optional<std::size_t>{
                                       runtime.selection.selections.primary()
                                           .active.line.value()}
                                 : std::nullopt;
    if (id == "diff.next_hunk") {
        hunk = nextDiffHunk(file->get(), currentLine);
    } else if (id == "diff.previous_hunk") {
        hunk = previousDiffHunk(file->get(), currentLine);
    } else if (!file->get().hunks.empty()) {
        hunk = 0;
    }
    if (!hunk) return failure("diff file has no hunks");
    const auto opened = diffOpenFile(file->get());
    const FollowTarget target{file->get().id, opened.path, opened.deleted,
                              file->get().hunks[*hunk].targetStart,
                              runtime.diff.viewState().revision};
    if (!runtime.revealDiffTarget(target, NavigationClass::Programmatic)) {
        return failure("diff target could not be revealed");
    }
    return success();
}

CommandHandlerResult followCommand(EditorRuntime::Impl& runtime, std::string_view id) {
    const auto modeBefore = runtime.follow.viewState().mode;
    FollowEditsResult result;
    if (id == "follow_edits.pause") {
        result = runtime.follow.pause();
    } else if (id == "follow_edits.resume") {
        result = runtime.follow.resume(runtime.diff.viewState());
    } else if (id == "follow_edits.toggle") {
        result = runtime.follow.toggle(runtime.diff.viewState());
    } else {
        return failure("unknown follow edits command");
    }
    if (!result.accepted()) return failure("follow edits command failed");
    const bool resumed = id == "follow_edits.resume" ||
                         (id == "follow_edits.toggle" &&
                          modeBefore == FollowMode::Paused);
    if (resumed) {
        const auto target = runtime.follow.viewState().activeTarget;
        if (target) {
            if (!runtime.openOrRevealFollowTargetProgrammatic(*target)) {
                return failure("follow target could not be revealed");
            }
        }
    }
    return success();
}

} // namespace

// Moving through a diff, and the follow-edits toggle.
//
// The diff commands take a live document id, which is meaningless to a remote
// client, so they are in-process only: typed for the handler, absent from the
// protocol.
void registerDiffAndFollowCommands(EditorSessionBuilder& builder,
                                   EditorRuntime::Impl& runtime) {
    auto diff = [&](std::string id, std::string summary) {
        auto const name = id;
        builder.add(CommandSpecBuilder{std::move(id)}
                        .owner("diff-model")
                        .summary(std::move(summary))
                        .mutates()
                        .lua()
                        .inProcessHandler<DiffFileId>(
                            [&runtime, name](CommandContext&,
                                             DiffFileId const& file) {
                                return runtime.runTransaction([&] {
                                    return diffCommand(runtime, name,
                                                       std::any{file});
                                });
                            }));
    };
    diff("diff.next_hunk", "Next Hunk");
    diff("diff.previous_hunk", "Previous Hunk");
    diff("diff.open_file", "Open File");

    auto follow = [&](std::string id, std::string summary) {
        auto const name = id;
        builder.add(CommandSpecBuilder{std::move(id)}
                        .owner("follow-edits")
                        .summary(std::move(summary))
                        .mutates()
                        .lua()
                        .handler([&runtime, name](CommandContext&) {
                            return runtime.runTransaction([&] {
                                return followCommand(runtime, name);
                            });
                        }));
    };
    follow("follow_edits.resume", "Resume");
    follow("follow_edits.pause", "Pause");
    follow("follow_edits.toggle", "Toggle");
}

// Moving around and acting on whichever tree the panel shows.
//
// All eight share one handler, which branches on the id, so each declaration
// only has to say what the command is called and what it carries.
void registerTreeCommands(EditorSessionBuilder& builder,
                          EditorRuntime::Impl& runtime) {
    auto spec = [](std::string id, std::string summary) {
        return CommandSpecBuilder{std::move(id)}
            .owner("tree-providers")
            .summary(std::move(summary))
            .mutates()
            .lua();
    };
    // The id is captured by value so the lambda owns it: a string_view into
    // the caller's temporary would dangle by the time the command runs.
    auto bare = [&](std::string id, std::string summary) {
        auto name = id;
        builder.add(spec(std::move(id), std::move(summary))
                        .handler([&runtime, name](CommandContext& context) {
                            return runtime.runTransaction([&] {
                                return treeCommand(runtime, context, name, {});
                            });
                        }));
    };

    bare("tree.toggle_expanded", "Toggle Expanded");
    bare("tree.select_next", "Select Next");
    bare("tree.select_previous", "Select Previous");

    builder.add(spec("tree.activate", "Open Selected")
                    .label("Open Selected")
                    .handler([&runtime](CommandContext& context) {
                        return runtime.runTransaction([&] {
                            return treeCommand(runtime, context,
                                               "tree.activate", {});
                        });
                    }));
    builder.add(spec("tree.invoke_node_command", "Invoke Node Command")
                    .optionalInProcessHandler<TreeCommandInvocation>(
                        [&runtime](CommandContext& context,
                                   std::optional<TreeCommandInvocation> const&
                                       invocation) {
                            return runtime.runTransaction([&] {
                                return treeCommand(
                                    runtime, context,
                                    "tree.invoke_node_command",
                                    invocation ? std::any{*invocation}
                                               : std::any{});
                            });
                        }));
    builder.add(spec("tree.select", "Select")
                    .handler<TreeSelectArguments>(
                        [&runtime](CommandContext& context,
                                   TreeSelectArguments const& arguments) {
                            return runtime.runTransaction([&] {
                                return treeCommand(runtime, context,
                                                   "tree.select",
                                                   std::any{arguments});
                            });
                        }));
    builder.add(spec("tree.scroll", "Scroll")
                    .handler<ScrollLinesArguments>(
                        [&runtime](CommandContext& context,
                                   ScrollLinesArguments const& arguments) {
                            return runtime.runTransaction([&] {
                                return treeCommand(runtime, context,
                                                   "tree.scroll",
                                                   std::any{arguments});
                            });
                        }));
    builder.add(spec("tree.scroll_to_fraction", "Scroll To Fraction")
                    .handler<ScrollFractionArguments>(
                        [&runtime](CommandContext& context,
                                   ScrollFractionArguments const& arguments) {
                            return runtime.runTransaction([&] {
                                return treeCommand(runtime, context,
                                                   "tree.scroll_to_fraction",
                                                   std::any{arguments});
                            });
                        }));
}

void bindRuntimeNavigation(EditorSessionBuilder& builder, EditorRuntime::Impl& runtime) {
    registerDiffAndFollowCommands(builder, runtime);
    auto searchCommands = searchCommandSet();
    for (auto const& descriptor : searchCommands.descriptors()) {
        builder.bind(std::string{descriptor.id}, [&runtime, descriptor](CommandContext& context, std::any const& payload) {
            return runtime.runTransaction([&] { return searchCommand(runtime, context, descriptor.id, payload); });
        });
    }
    registerTreeCommands(builder, runtime);
}

} // namespace ssg
