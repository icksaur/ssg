#include "editor_session_internal.h"

#include <ssg/CommandCatalog.h>
#include <ssg/Selection.h>

#include <algorithm>
#include <cctype>
#include <charconv>

namespace ssg {
namespace {

CommandHandlerResult validatePublishedCommand(EditorSession::Impl& runtime,
                                             CommandContext& context,
                                             std::string const& commandId) {
    auto const palette = runtime.paletteView();
    const auto* candidates = palette.candidatesFor(SearchMode::Command);
    if (candidates == nullptr) {
        return failure("command picker inventory is unavailable");
    }
    bool const published =
        std::any_of(candidates->begin(), candidates->end(),
                    [&](auto const& candidate) { return candidate.id == commandId; });
    if (!published) {
        return failure("command is not in the palette candidate set: " + commandId);
    }
    auto const* declared = runtime.session->catalog()->find(commandId);
    if (declared != nullptr) {
        for (auto const& capability : declared->requiredCapabilities) {
            if (!context.principal().hasCapability(capability)) {
                return failure("principal lacks capability for palette command: " +
                               commandId);
            }
        }
    }
    return success();
}

CommandHandlerResult validatePaletteTarget(EditorSession::Impl& runtime,
                                           CommandContext& context,
                                           std::string const& commandId) {
    bool const paletteOpen = runtime.interaction.prompt().active() &&
                              runtime.interaction.prompt().request() &&
                              runtime.interaction.prompt().request()->kind ==
                                  PromptKind::Palette;
    if (!paletteOpen) return failure("palette.execute requires the palette to be open");
    // Every picker uses a Palette-kind prompt, so prompt kind alone no longer
    // identifies the command palette.  Without this the file picker's
    // candidates -- which are PATHS, not command ids -- would be submittable as
    // commands.
    if (runtime.interaction.openPicker() != PickerKind::Command) {
        return failure("palette.execute requires the command palette to be open");
    }
    return validatePublishedCommand(runtime, context, commandId);
}

CommandHandlerResult searchCommand(EditorSession::Impl& runtime, CommandContext& context, std::string_view id, std::any const& payload) {
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
        runtime.rebuildFileCandidates();
    }
    else if (id == "palette.close") {
        if (auto const* expected = payloadAs<PickerActivation>(payload)) {
            if (!runtime.deferredCommands.empty()) {
                if (!runtime.defer(
                        std::nullopt,
                        ClientCommand{"palette.close", context.revision(),
                                      *expected})) {
                    return failure("could not defer the picker close");
                }
                return success();
            }
            if (runtime.interaction.openPickerActivation() != *expected) {
                return success();
            }
        }
        if (!runtime.interaction.apply(CloseFinder{})) {
            return failure("no palette to close");
        }
    }
    else if (id == "palette.next" || id == "search.results_next") runtime.search.selectNext();
    else if (id == "palette.previous" || id == "search.results_previous") runtime.search.selectPrevious();
    else if (id == "palette.execute") {
        auto const* arguments = payloadAs<PaletteExecuteArguments>(payload);
        if (arguments == nullptr) return failure("palette.execute requires a command id payload");
        auto validation = validatePaletteTarget(runtime, context, arguments->commandId);
        if (!validation.accepted) return validation;
        if (!runtime.defer(std::nullopt,
                           ClientCommand{arguments->commandId, revision, {}})) {
            return failure("could not queue the selected command");
        }
        (void)runtime.interaction.apply(CloseFinder{});
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
    } else if (id == "goto.file" || id == "goto.symbol") {
        auto const* target = payloadAs<NavigationTarget>(payload);
        if (target != nullptr) (void)runtime.navigation.visit(*target, NavigationOrigin::User);
        else return failure(std::string{id} + " requires a navigation target payload");
    } else if (id == "goto.line") {
        // No payload means the command was invoked directly (keybinding or
        // palette): open a one-field prompt that re-dispatches goto.line with the
        // typed line number via the generic prompt.submit path.
        auto const* lineText = payloadAs<std::string>(payload);
        if (lineText == nullptr) {
            auto opened = runtime.interaction.openPrompt(PromptRequest{
                PromptKind::CommandArgument, "go to line",
                {{"line", "line number", ""}}, {}, std::nullopt, "goto.line"});
            if (!opened.accepted()) return failure(opened.error->message);
            return success();
        }
        if (!runtime.activeDocumentId()) return failure("goto.line requires an active document");
        // Trim surrounding whitespace, then require the whole value to be a
        // base-10 integer. The number itself is not range-checked here: it is
        // clamped to [1, lineCount] below, so 0 or a negative goes to the first
        // line and an over-large number goes to the last (backlog: clamp to
        // [1, LINES]).
        std::string_view digits{*lineText};
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
        // One source of truth for line boundaries: the active text. The line
        // count is newlines + 1, and the target line's start is taken from the
        // same scan, so the two can never disagree.
        std::string const& text = runtime.activeText();
        std::size_t lineCount = 1;
        for (char c : text) if (c == '\n') ++lineCount;
        // Clamp the 1-based request to [1, lineCount], then convert to a 0-based
        // line index.
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
        auto position = SelectionNavigator::resolvePosition(text, ByteOffset{start}, 4);
        if (!position) return failure("goto.line could not resolve the target position");
        SelectionCommandArguments arguments;
        arguments.position = *position;
        // Placing and revealing the caret is owned by cursor.set_position; route
        // through it (deferred, since the session lock is non-reentrant) rather
        // than duplicating the reveal/focus/history contract here.
        if (!runtime.defer(std::nullopt,
                           ClientCommand{"cursor.set_position", revision,
                                         std::any{arguments}})) {
            return failure("could not queue cursor.set_position");
        }
    } else {
        return failure("unknown search command");
    }
    return success();
}

CommandHandlerResult treeCommand(EditorSession::Impl& runtime,
                                 CommandContext& context,
                                 std::string_view id,
                                 std::any const& payload) {
    if (id == "tree.select_next") { (void)runtime.tree.selectNext(); return success(); }
    if (id == "tree.select_previous") { (void)runtime.tree.selectPrevious(); return success(); }
    if (id == "tree.activate_node") {
        auto const* arguments = payloadAs<TreeSelectArguments>(payload);
        if (arguments == nullptr) {
            return failure("tree.activate_node requires a node id payload");
        }
        if (!runtime.tree.select(arguments->nodeId)) {
            return failure("tree node is not selectable");
        }

        (void)runtime.interaction.focusPanel();
        return treeCommand(runtime, context, "tree.activate", {});
    }
    if (id == "tree.activate") {
        const auto binding = runtime.tree.activeProviderBinding();
        const auto providerKind =
            binding ? std::optional<TreeProviderKind>{binding->kind}
                    : std::nullopt;
        auto selected = runtime.tree.selectedNode();
        if (!selected) return failure("no tree node is selected");
        if (selected->expandable) { (void)runtime.tree.toggleSelected(); return success(); }
        if (providerKind == TreeProviderKind::Git && selected->workspacePath) {
            const auto diffView = runtime.diff.viewState();
            auto file = std::find_if(
                diffView.files.begin(), diffView.files.end(),
                [&](const DiffFileView& candidate) {
                    return candidate.path.generic_string() == *selected->workspacePath;
                });
            if (file == diffView.files.end()) {
                if (selected->gitStatus &&
                    selected->gitStatus->status == GitTreeStatus::Deleted) {
                    return failure("detailed view is unavailable for deleted file");
                }
            } else {
                return runtime.openOrFocusLiveDiffTab(
                    *file, NavigationClass::User,
                    context.principal().clientId(), context.viewId());
            }
        }
        if (selected->workspacePath) {
            auto result = runtime.workspace.openFile(*selected->workspacePath);
            if (!result.accepted() || !result.document) return failure("failed to open tree file");
            auto opened = runtime.activateDocument(*result.document);
            if (opened.accepted) runtime.interaction.focusEditor();
            return opened;
        }
        return success();
    }
    if (id == "tree.select") {
        auto const* arguments = payloadAs<TreeSelectArguments>(payload);
        if (arguments == nullptr) return failure("tree.select requires a node id payload");
        if (!runtime.tree.select(arguments->nodeId)) return failure("tree node is not selectable");

        // Focus follows the pointer (M8-F): clicking a tree row acts on the panel,
        // so move keyboard focus there. (For a file click the app dispatches
        // tree.activate next, whose file-open focus_editor() then wins.)
        (void)runtime.interaction.focusPanel();
        return success();
    }
    auto const* invocation = payloadAs<TreeCommandInvocation>(payload);
    if (invocation == nullptr) return failure(std::string{id} + " requires a tree invocation payload");
    if (id == "tree.toggle_expanded") {
        auto toggled = runtime.tree.toggleExpanded(invocation->providerId, invocation->nodeId);

        return toggled ? success() : failure("tree node does not exist");
    }
    auto command = runtime.tree.invokeNodeCommand(invocation->providerId, invocation->nodeId, invocation->commandId);
    return command ? success() : failure("tree node command does not exist");
}

CommandHandlerResult diffCommand(EditorSession::Impl& runtime, std::string_view id, std::any const& payload) {
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

CommandHandlerResult followCommand(EditorSession::Impl& runtime, std::string_view id) {
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
// interface.
void registerDiffAndFollowCommands(CommandCatalog& builder,
                                   EditorSession::Impl& runtime) {
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
// Semantic tree actions share one handler. Scroll actions resolve to the
// attached view owner instead of mutating session presentation state.
void registerTreeCommands(CommandCatalog& builder,
                          EditorSession::Impl& runtime) {
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
    builder.add(spec("tree.activate_node", "Open Node")
                    .handler<TreeSelectArguments>(
                       [&runtime](CommandContext& context,
                                  TreeSelectArguments const& arguments) {
                           return runtime.runTransaction([&] {
                               return treeCommand(
                                   runtime, context, "tree.activate_node",
                                   std::any{arguments});
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
    builder.add(CommandSpecBuilder{"tree.scroll"}
                    .owner("tree-providers")
                    .summary("Scroll")
                    .viewAction()
                    .lua()
                    .handler<ScrollLinesArguments>(
                        [](CommandContext&,
                           ScrollLinesArguments const& arguments) {
                            return CommandHandlerResult::requireView(
                                ViewScrollLines{ViewScrollTarget::Tree,
                                                arguments.rows});
                        }));
    builder.add(CommandSpecBuilder{"tree.scroll_to_fraction"}
                    .owner("tree-providers")
                    .summary("Scroll To Fraction")
                    .viewAction()
                    .lua()
                    .handler<ScrollFractionArguments>(
                        [](CommandContext&,
                           ScrollFractionArguments const& arguments) {
                            return CommandHandlerResult::requireView(
                                ViewScrollFraction{
                                    ViewScrollTarget::Tree,
                                    arguments.numerator,
                                    arguments.denominator});
                        }));
}

// The pickers, workspace search, and the go-to jumps.
void registerSearchPaletteCommands(CommandCatalog& builder,
                                   EditorSession::Impl& runtime) {
    auto spec = [](std::string id, std::string summary) {
        return CommandSpecBuilder{std::move(id)}
            .owner("search-palette")
            .summary(std::move(summary))
            .mutates()
            .lua();
    };
    auto bare = [&](std::string id, std::string label, std::string summary) {
        auto name = id;
        auto built = spec(std::move(id), std::move(summary))
                         .handler([&runtime, name](CommandContext& context) {
                             return runtime.runTransaction([&] {
                                 return searchCommand(runtime, context, name,
                                                      {});
                             });
                         });
        if (!label.empty()) built.label(std::move(label));
        builder.add(std::move(built));
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

    // Direct clients close unconditionally with no payload. The internal
    // post-submit close carries the activation it is allowed to dismiss, so a
    // selected command that replaced the picker cannot have its new UI canceled.
    builder.add(
        spec("palette.close", "Close")
            .optionalInProcessHandler<PickerActivation>(
                [&runtime](CommandContext& context,
                           std::optional<PickerActivation> expected) {
                    return runtime.runTransaction([&] {
                        return searchCommand(
                            runtime, context, "palette.close",
                            expected ? std::any{*expected} : std::any{});
                    });
                }));

    // Names the command to run, so it is the one search command a remote client
    // may send an argument for.
    builder.add(spec("palette.execute", "Execute")
                    .handler<PaletteExecuteArguments>(
                        [&runtime](CommandContext& context,
                                   PaletteExecuteArguments const& arguments) {
                            return runtime.runTransaction([&] {
                                return searchCommand(runtime, context,
                                                     "palette.execute",
                                                     std::any{arguments});
                            });
                        }));

    builder.add(CommandSpecBuilder{"picker.submit"}
                    .owner("search-palette")
                    .summary("Submit Picker Candidate")
                    .stateValidatedMutation()
                    .lua()
                    .handler<PickerSubmitArguments>(
                        [&runtime](CommandContext& context,
                                  PickerSubmitArguments const& arguments) {
                           return runtime.runTransaction([&] {
                               auto const palette = runtime.paletteView();
                               auto const* candidates =
                                   palette.candidatesFor(
                                       arguments.activation.mode);
                               if (candidates == nullptr) {
                                   return failure(
                                       "picker mode has no candidate inventory");
                               }
                               auto const published = std::find_if(
                                   candidates->begin(),
                                   candidates->end(),
                                   [&](auto const& candidate) {
                                       return candidate.id ==
                                              arguments.candidateId;
                                   });
                               if (published == candidates->end()) {
                                   return failure(
                                       "candidate is not in the picker inventory");
                               }
                               if (runtime.interaction.openPickerActivation() !=
                                   arguments.activation) {
                                   return failure(
                                       "picker.submit requires a matching open picker");
                               }
                               ClientCommand selected;
                               if (arguments.activation.mode ==
                                   SearchMode::Command) {
                                   auto validation = validatePublishedCommand(
                                       runtime, context, arguments.candidateId);
                                   if (!validation.accepted) return validation;
                                   selected = ClientCommand{
                                       arguments.candidateId,
                                       context.revision(), {}};
                               } else if (arguments.activation.mode ==
                                          SearchMode::File) {
                                   selected = ClientCommand{
                                       "file.open", context.revision(),
                                       arguments.candidateId};
                               } else {
                                   return failure(
                                       "open picker has no submit action");
                               }
                               if (runtime.deferredCommands.contains(
                                       "palette.close")) {
                                   return failure(
                                       "another picker submission is pending");
                               }
                               if (!runtime.defer(std::nullopt,
                                                  std::move(selected))) {
                                   return failure(
                                       "could not queue the selected command");
                               }
                               if (!runtime.defer(
                                       std::nullopt,
                                       ClientCommand{"palette.close",
                                                     context.revision(),
                                                     arguments.activation})) {
                                   return failure(
                                       "could not queue the picker close");
                               }
                               return success();
                           });
                        }));

    // An absent query searches for the current one.
    builder.add(spec("search.workspace", "Workspace")
                    .optionalInProcessHandler<std::string>(
                        [&runtime](CommandContext& context,
                                   std::optional<std::string> const& query) {
                            return runtime.runTransaction([&] {
                                return searchCommand(
                                    runtime, context, "search.workspace",
                                    query ? std::any{*query} : std::any{});
                            });
                        }));

    // Each jumps to a place the client resolved, which is meaningless to
    // another process.
    auto jump = [&](std::string id, std::string label, std::string summary) {
        auto name = id;
        builder.add(spec(std::move(id), std::move(summary))
                        .label(std::move(label))
                        .optionalInProcessHandler<NavigationTarget>(
                            [&runtime, name](
                                CommandContext& context,
                                std::optional<NavigationTarget> const& target) {
                                return runtime.runTransaction([&] {
                                    return searchCommand(
                                        runtime, context, name,
                                        target ? std::any{*target}
                                               : std::any{});
                                });
                            }));
    };
    jump("goto.file", "Go to File", "Go to File");
    jump("goto.symbol", "Go to Symbol", "Go to Symbol");

    // goto.line is not a client-resolved jump: with no argument it opens a
    // line-number prompt, and the prompt round-trip re-dispatches it with the
    // typed string. An absent payload therefore opens the prompt.
    builder.add(spec("goto.line", "Go to Line")
                    .label("Go to Line")
                    .optionalInProcessHandler<std::string>(
                        [&runtime](CommandContext& context,
                                   std::optional<std::string> const& line) {
                            return runtime.runTransaction([&] {
                                return searchCommand(
                                    runtime, context, "goto.line",
                                    line ? std::any{*line} : std::any{});
                            });
                        }));
}

void bindRuntimeNavigation(CommandCatalog& builder, EditorSession::Impl& runtime) {
    registerDiffAndFollowCommands(builder, runtime);
    registerSearchPaletteCommands(builder, runtime);
    registerTreeCommands(builder, runtime);
}

} // namespace ssg
