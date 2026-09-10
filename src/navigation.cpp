#include <ssg/Editor.h>

#include <ssg/CommandCatalog.h>
#include <ssg/GraphemeLayout.h>
#include <ssg/Selection.h>
#include <ssg/platform_files.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <optional>
#include <string>

namespace ssg {

[[nodiscard]] CommandHandlerResult applyGotoLine(Editor& runtime, std::string_view lineText);

namespace {

// The authoritative text a navigation target is validated against: the buffer
// when the file is already open, so an unsaved edit is not validated against a
// stale disk copy, and the file otherwise.
std::optional<std::string> navigationTargetText(Editor& runtime,
                                                std::string const& path) {
    for (auto const document : runtime.workspace.documents()) {
        auto const state = runtime.workspace.state(document);
        if (!state || state->key.kind() != JournalDocumentKeyKind::Saved) continue;
        if (state->key.savedPath() != path) continue;
        auto const* opened = runtime.workspace.tryDocument(document);
        if (opened == nullptr) return std::nullopt;
        return opened->snapshot().text;
    }
    auto read = readFile(runtime.workspace.root() / path);
    if (!read.ok()) return std::nullopt;
    return std::string{read.bytes.begin(), read.bytes.end()};
}

// The largest grapheme-cluster start at or before `columnOffset`, so a target
// column landing inside a multi-byte cluster resolves to the cluster's start.
std::size_t graphemeBoundaryAtOrBefore(std::string_view lineText,
                                       std::size_t columnOffset) {
    if (columnOffset == 0 || columnOffset >= lineText.size()) return columnOffset;
    auto const run = computeCellRun(lineText);
    std::size_t boundary = 0;
    for (auto const& span : run.spans) {
        if (span.byteOffset > columnOffset) break;
        boundary = span.byteOffset;
    }
    return boundary;
}

std::optional<std::size_t> navigationByteOffset(std::string_view text,
                                                NavigationTarget const& target) {
    if (target.column == 0) return std::nullopt;
    std::size_t lineStart = 0;
    for (std::size_t line = 0; line < target.line.value(); ++line) {
        auto const newline = text.find('\n', lineStart);
        if (newline == std::string_view::npos) return std::nullopt;
        lineStart = newline + 1;
    }
    auto lineEnd = text.find('\n', lineStart);
    if (lineEnd == std::string_view::npos) lineEnd = text.size();
    auto const lineText = text.substr(lineStart, lineEnd - lineStart);
    std::size_t const columnOffset = target.column - 1;
    if (columnOffset > lineText.size()) return std::nullopt;
    return lineStart + graphemeBoundaryAtOrBefore(lineText, columnOffset);
}

CommandHandlerResult placePrimaryCaret(Editor& runtime, FileDocumentId document,
                                       DocumentPosition position) {
    return applyEditorSelections(
        runtime,
        ApplySelections{
            document,
            SelectionSet{std::vector<Selection>{Selection{position, position}}},
            true});
}

// Validates the whole transition before it mutates anything: a target that does
// not resolve leaves the active document, the cursor, the open-document set and
// the navigation history exactly as they were. Recording history is the
// caller's job, and must happen only after this succeeds.
CommandHandlerResult applyNavigationTransition(
    Editor& runtime, NavigationTransition const& transition) {
    if (!transition.target) return failure("navigation has no target");
    auto const& target = *transition.target;
    auto const text = navigationTargetText(runtime, target.path);
    if (!text) return failure("navigation target is unreadable: " + target.path);
    auto const offset = navigationByteOffset(*text, target);
    if (!offset) {
        return failure("navigation target is outside the file: " + target.path);
    }
    auto const position = resolveSelectionPosition(*text, ByteOffset{*offset}, 4);
    if (!position) return failure("navigation target could not be resolved");

    auto opened = runtime.workspace.openFile(target.path);
    if (!opened.accepted() || !opened.document) {
        return failure("navigation target could not be opened: " + target.path);
    }
    runtime.ensureDocumentRuntimeState(*opened.document);
    auto activated = runtime.activateDocument(*opened.document);
    if (!activated.accepted) return activated;
    runtime.screen.focusEditor();

    if (transition.pauseFollowEdits &&
        runtime.follow.viewState().mode == FollowMode::Following) {
        (void)runtime.follow.pause();
    }
    auto placed = placePrimaryCaret(runtime, *opened.document, *position);
    if (!placed.accepted) {
        return placed;
    }
    if (transition.revealPrimaryCaret) {
        return CommandHandlerResult::requireView(ViewAction{RevealSelection{}});
    }
    return success();
}

CommandHandlerResult deferNavigationViewAction(
    Editor& runtime, CommandHandlerResult result) {
    if (!result.accepted || !result.viewAction) return result;
    if (!std::holds_alternative<RevealSelection>(*result.viewAction)) {
        return failure("navigation produced an unsupported view action");
    }
    if (!runtime.deferDispatch(ClientCommand{"view.reveal_caret", {}})) {
        return failure("could not queue view.reveal_caret");
    }
    return success();
}

CommandHandlerResult searchCommand(Editor& runtime, std::string_view id) {
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
        runtime.rebuildFileCandidates();
    }
    else if (id == "palette.close") {
        if (!runtime.screen.closeFinder()) {
            return failure("no palette to close");
        }
    }
    else if (id == "palette.next" || id == "search.results_next") runtime.search.selectNext();
    else if (id == "palette.previous" || id == "search.results_previous") runtime.search.selectPrevious();
    else if (id == "search.workspace") {
        const auto sourceGeneration = ++runtime.workspaceSearchGeneration;
        runtime.startWorkspaceSearch(std::string{}, sourceGeneration);
    } else if (id == "goto.back" || id == "goto.forward") {
        bool const backward = id == "goto.back";
        auto const transition = backward ? runtime.navigation.peekBack()
                                         : runtime.navigation.peekForward();
        if (!transition.target) return success();
        auto applied = applyNavigationTransition(runtime, transition);
        if (!applied.accepted) return applied;
        auto deferred = deferNavigationViewAction(runtime, std::move(applied));
        if (!deferred.accepted) return deferred;
        // The history cursor moves only once the destination is really open.
        if (backward) (void)runtime.navigation.back();
        else (void)runtime.navigation.forward();
        return deferred;
    } else if (id == "goto.line") {
        auto opened = openGenericPrompt(runtime.screen.prompt(), PromptRequest{
            PromptKind::CommandArgument, "go to line",
            {{"line", "line number", ""}}, {}, std::nullopt,
            PromptCompletion::GotoLine});
        if (!opened.accepted()) return failure(opened.error->message);
        return success();
    } else {
        return failure("unknown search command");
    }
    return success();
}

CommandHandlerResult treeCommand(Editor& runtime,
                                 std::string_view id) {
    if (id == "tree.select_next") { (void)runtime.tree.selectNext(); return success(); }
    if (id == "tree.select_previous") { (void)runtime.tree.selectPrevious(); return success(); }
    if (id == "tree.activate") {
        const auto binding = runtime.tree.activeProviderBinding();
        const auto providerKind =
            binding ? std::optional<TreeProviderKind>{binding->kind}
                    : std::nullopt;
        auto selected = runtime.tree.selectedNode();
        if (!selected) return failure("no tree node is selected");
        if (selected->expandable) {
            if (!binding) {
                return failure("tree node is not expandable");
            }
            return runtime.toggleTreeExpanded(binding->id, selected->id);
        }
        if (providerKind == TreeProviderKind::Git && selected->workspacePath) {
            const auto diffView = runtime.diff.viewState();
            auto file = std::find_if(
                diffView.files.begin(), diffView.files.end(),
                [&](const DiffFileView& candidate) {
                    return candidate.path.generic_string() == *selected->workspacePath;
                });
            if (file == diffView.files.end()) {
                if (selected->gitStatus &&
                    selected->gitStatus->status == DiffFileStatus::Deleted) {
                    return failure("detailed view is unavailable for deleted file");
                }
            } else {
                return runtime.openOrFocusLiveDiffTab(*file,
                                                       NavigationClass::User);
            }
        }
        if (providerKind == TreeProviderKind::Search) {
            if (!selected->workspacePath || !selected->sourceLine ||
                !selected->sourceColumn) {
                return failure("search result has no navigation target");
            }
            NavigationTarget target{
                .path = *selected->workspacePath,
                .line = LineIndex{*selected->sourceLine},
                .column = static_cast<std::size_t>(*selected->sourceColumn)};
            return deferNavigationViewAction(
                runtime, navigateTo(runtime, std::move(target)));
        }
        if (selected->workspacePath) {
            auto result = runtime.workspace.openFile(*selected->workspacePath);
            if (!result.accepted() || !result.document) return failure("failed to open tree file");
            auto opened = runtime.activateDocument(*result.document);
            if (opened.accepted) runtime.screen.focusEditor();
            return opened;
        }
        return success();
    }
    if (id == "tree.toggle_expanded") {
        const auto binding = runtime.tree.activeProviderBinding();
        const auto selected = runtime.tree.selectedNode();
        if (!binding || !selected) return failure("no tree node is selected");
        return runtime.toggleTreeExpanded(binding->id, selected->id);
    }
    return failure("unknown tree command");
}

CommandHandlerResult followCommand(Editor& runtime, std::string_view id) {
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

CommandHandlerResult navigateTo(Editor& runtime, NavigationTarget target,
                                NavigationOrigin origin) {
    auto applied = applyNavigationTransition(
        runtime, navigationTransition(target, origin));
    if (!applied.accepted) return applied;
    (void)runtime.navigation.visit(std::move(target), origin);
    return applied;
}

CommandHandlerResult selectTreeNode(Editor& runtime, TreeNodeId nodeId) {
    if (!runtime.tree.select(nodeId)) {
        return failure("tree node is not selectable");
    }
    (void)runtime.screen.focusPanel();
    return success();
}

CommandHandlerResult invokeTreeNodeCommand(
    Editor& runtime, TreeCommandInvocation invocation) {
    auto command = runtime.tree.invokeNodeCommand(
        invocation.providerId, invocation.nodeId, invocation.commandId);
    return command ? success() : failure("tree node command does not exist");
}

CommandHandlerResult navigateDiff(Editor& runtime, DiffFileId fileId,
                                  DiffNavigation navigation) {
    auto file = runtime.diff.file(fileId);
    if (!file) return failure("diff file does not exist");
    std::optional<std::size_t> hunk;
    const auto currentLine = runtime.activeDocument()
                                 ? std::optional<std::size_t>{
                                       runtime.selection.selections.primary()
                                           .active.line.value()}
                                 : std::nullopt;
    if (navigation == DiffNavigation::NextHunk) {
        hunk = nextDiffHunk(file->get(), currentLine);
    } else if (navigation == DiffNavigation::PreviousHunk) {
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

CommandHandlerResult applyGotoLine(Editor& runtime, std::string_view lineText) {
    if (!runtime.activeDocumentId()) return failure("goto.line requires an active document");
    std::string_view digits{lineText};
    while (!digits.empty() && std::isspace(static_cast<unsigned char>(digits.front())))
        digits.remove_prefix(1);
    while (!digits.empty() && std::isspace(static_cast<unsigned char>(digits.back())))
        digits.remove_suffix(1);
    long long requested = 0;
    auto const* first = digits.data();
    auto const* last = first + digits.size();
    auto const [stop, ec] = std::from_chars(first, last, requested);
    if (ec != std::errc{} || stop != last)
        return failure("goto.line expects a line number");
    std::string const& text = runtime.activeText();
    std::size_t lineCount = 1;
    for (char c : text) if (c == '\n') ++lineCount;
    std::size_t target = requested < 1
                             ? 0
                             : (static_cast<unsigned long long>(requested) > lineCount
                                    ? lineCount - 1
                                    : static_cast<std::size_t>(requested - 1));
    std::size_t start = 0;
    if (target > 0) {
        std::size_t seen = 0;
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '\n' && ++seen == target) { start = i + 1; break; }
        }
    }
    auto position = resolveSelectionPosition(text, ByteOffset{start}, 4);
    if (!position) return failure("goto.line could not resolve the target position");
    auto const active = runtime.activeDocumentId();
    if (!active) return failure("goto.line requires an active document");
    return applyEditorSelections(
        runtime,
        ApplySelections{
            *active,
            SelectionSet{std::vector<Selection>{Selection{*position, *position}}},
            true});
}

CommandHandlerResult activateTreeNode(Editor& runtime, TreeNodeId nodeId) {
    if (!runtime.tree.select(nodeId)) {
        return failure("tree node is not selectable");
    }
    (void)runtime.screen.focusPanel();

    auto selected = runtime.tree.selectedNode();
    if (!selected) return failure("no tree node is selected");

    const auto binding = runtime.tree.activeProviderBinding();
    const auto providerKind =
        binding ? std::optional<TreeProviderKind>{binding->kind} : std::nullopt;

    if (selected->expandable) {
        if (!binding) return failure("tree node is not expandable");
        return runtime.toggleTreeExpanded(binding->id, selected->id);
    }
    if (providerKind == TreeProviderKind::Git && selected->workspacePath) {
        const auto diffView = runtime.diff.viewState();
        auto file = std::find_if(
            diffView.files.begin(), diffView.files.end(),
            [&](const DiffFileView& candidate) {
                return candidate.path.generic_string() == *selected->workspacePath;
            });
        if (file != diffView.files.end()) {
            return runtime.openOrFocusLiveDiffTab(*file, NavigationClass::User);
        }
        if (selected->gitStatus &&
            selected->gitStatus->status == DiffFileStatus::Deleted) {
            return failure("detailed view is unavailable for deleted file");
        }
    }
    if (providerKind == TreeProviderKind::Search) {
        if (!selected->workspacePath || !selected->sourceLine ||
            !selected->sourceColumn) {
            return failure("search result has no navigation target");
        }
        NavigationTarget target{
            .path = *selected->workspacePath,
            .line = LineIndex{*selected->sourceLine},
            .column = static_cast<std::size_t>(*selected->sourceColumn)};
        return navigateTo(runtime, std::move(target));
    }
    if (selected->workspacePath) {
        auto result = runtime.workspace.openFile(*selected->workspacePath);
        if (!result.accepted() || !result.document) {
            return failure("failed to open tree file");
        }
        auto opened = runtime.activateDocument(*result.document);
        if (opened.accepted) runtime.screen.focusEditor();
        return opened;
    }
    return success();
}


//
// The diff commands take a live document id, which is meaningless to a remote
// client, so they are in-process only: typed for the handler, absent from the
// interface.
void registerDiffAndFollowCommands(CommandCatalog& catalog,
                                   Editor& runtime) {
    auto follow = [&](std::string id, std::string summary) {
        auto const name = id;
        catalog.add(CommandSpec{
            .id = std::move(id),
            .owner = "follow-edits",
            .summary = std::move(summary),
            .effect = CommandEffect::Mutation,
            .luaApi = true,
            .binding = bindNoArgumentHandler([&runtime, name](CommandContext&) {
                return followCommand(runtime, name);
            }),
        });
    };
    follow("follow_edits.resume", "Resume");
    follow("follow_edits.pause", "Pause");
    follow("follow_edits.toggle", "Toggle");
}

// Moving around and acting on whichever tree the panel shows.
void registerTreeCommands(CommandCatalog& catalog,
                          Editor& runtime) {
    auto spec = [](std::string id, std::string summary) {
        return CommandSpec{
            .id = std::move(id),
            .owner = "tree-providers",
            .summary = std::move(summary),
            .effect = CommandEffect::Mutation,
            .luaApi = true,
        };
    };
    // The id is captured by value so the lambda owns it: a string_view into
    // the caller's temporary would dangle by the time the command runs.
    auto bare = [&](std::string id, std::string summary) {
        auto name = id;
        auto built = spec(std::move(id), std::move(summary));
        built.binding = bindNoArgumentHandler(
            [&runtime, name](CommandContext&) {
                return treeCommand(runtime, name);
            });
        catalog.add(std::move(built));
    };

    bare("tree.toggle_expanded", "Toggle Expanded");
    bare("tree.select_next", "Select Next");
    bare("tree.select_previous", "Select Previous");

    {
        auto built = spec("tree.activate", "Open Selected");
        built.label = "Open Selected";
        built.binding = bindNoArgumentHandler(
            [&runtime](CommandContext&) {
                return treeCommand(runtime, "tree.activate");
            });
        catalog.add(std::move(built));
    }
}

// The pickers, workspace search, and the go-to jumps.
void registerSearchPaletteCommands(CommandCatalog& catalog,
                                   Editor& runtime) {
    auto spec = [](std::string id, std::string summary) {
        return CommandSpec{
            .id = std::move(id),
            .owner = "search-palette",
            .summary = std::move(summary),
            .effect = CommandEffect::Mutation,
            .luaApi = true,
        };
    };
    auto bare = [&](std::string id, std::string label, std::string summary) {
        auto name = id;
        auto built = spec(std::move(id), std::move(summary));
        built.binding = bindNoArgumentHandler(
            [&runtime, name](CommandContext&) {
                return searchCommand(runtime, name);
            });
        if (!label.empty()) built.label = std::move(label);
        catalog.add(std::move(built));
    };

    bare("palette.open", "Command Palette", "Command Palette");
    bare("file_finder.open", "", "Open");
    bare("file_finder.toggle_gitignore", "", "Toggle Gitignore");
    bare("palette.next", "", "Next");
    bare("palette.previous", "", "Previous");
    bare("goto.back", "", "Back");
    bare("goto.forward", "", "Forward");
    bare("search.results_next", "", "Results Next");
    bare("search.results_previous", "", "Results Previous");

    {
        auto built = spec("palette.close", "Close");
        built.binding = bindNoArgumentHandler(
            [&runtime](CommandContext&) {
                return searchCommand(runtime, "palette.close");
            });
        catalog.add(std::move(built));
    }

    {
        auto built = spec("search.workspace", "Workspace");
        built.binding = bindNoArgumentHandler(
            [&runtime](CommandContext&) {
                return searchCommand(runtime, "search.workspace");
            });
        catalog.add(std::move(built));
    }

    {
        auto built = spec("goto.line", "Go to Line");
        built.label = "Go to Line";
        built.binding = bindNoArgumentHandler(
            [&runtime](CommandContext&) {
                return searchCommand(runtime, "goto.line");
            });
        catalog.add(std::move(built));
    }
}

void bindRuntimeNavigation(CommandCatalog& catalog, Editor& runtime) {
    registerDiffAndFollowCommands(catalog, runtime);
    registerSearchPaletteCommands(catalog, runtime);
    registerTreeCommands(catalog, runtime);
}

} // namespace ssg
