#include "editor_runtime_internal.h"

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
    auto const descriptors = p0CommandDescriptors();
    auto const found = std::find_if(
        descriptors.begin(), descriptors.end(),
        [&](CommandDescriptor const& descriptor) { return descriptor.id == commandId; });
    if (found != descriptors.end()) {
        for (auto const& capability : found->requiredCapabilities) {
            if (!context.principal().hasCapability(capability)) {
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

void bindRuntimeNavigation(EditorSessionBuilder& builder, EditorRuntime::Impl& runtime) {
    auto searchCommands = searchCommandSet();
    auto treeCommands = treeCommandSet();
    auto diffCommands = diffCommandSet();
    auto followCommands = followEditsCommandSet();
    for (auto const& descriptor : searchCommands.descriptors()) {
        builder.bind(std::string{descriptor.id}, [&runtime, descriptor](CommandContext& context, std::any const& payload) {
            return runtime.runTransaction([&] { return searchCommand(runtime, context, descriptor.id, payload); });
        });
    }
    for (auto const& descriptor : treeCommands.descriptors()) {
        builder.bind(std::string{descriptor.id}, [&runtime, descriptor](CommandContext& context, std::any const& payload) {
            return runtime.runTransaction([&] { return treeCommand(runtime, context, descriptor.id, payload); });
        });
    }
    for (auto const& descriptor : diffCommands.descriptors()) {
        builder.bind(std::string{descriptor.id}, [&runtime, descriptor](CommandContext&, std::any const& payload) {
            return runtime.runTransaction([&] { return diffCommand(runtime, descriptor.id, payload); });
        });
    }
    for (auto const& descriptor : followCommands.descriptors()) {
        builder.bind(std::string{descriptor.id}, [&runtime, descriptor](CommandContext&, std::any const&) {
            return runtime.runTransaction([&] { return followCommand(runtime, descriptor.id); });
        });
    }
}

} // namespace ssg
