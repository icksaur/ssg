#include "editor_runtime_internal.h"

#include <algorithm>

namespace ssg {
namespace {

template <typename T>
T const* payload_as(std::any const& payload) { return std::any_cast<T>(&payload); }

PromptRequest palette_prompt_request() {
    return PromptRequest{PromptKind::palette, "Command Palette",
                         {{"query", "Command palette query", ""}}, {}, std::nullopt};
}

// Validates that the palette is open and that `command_id` is a member of the
// currently published palette candidate set (the command mode's candidates,
// which `palette_view()` publishes from `descriptors()`) and that the invoking
// principal holds its required capabilities.  On success the target id is
// stashed for the EditorRuntime dispatch wrapper to execute through the registry
// (the session mutex is non-reentrant, so the handler cannot re-enter dispatch).
// This keeps execution server-owned and rejects any id the palette never offered
// (see doc/spec-palette.md P1).
CommandHandlerResult validate_palette_target(EditorRuntime::Impl& runtime,
                                             CommandContext& context,
                                             std::string const& command_id) {
    bool const palette_open = runtime.prompt.active() && runtime.prompt.request() &&
                              runtime.prompt.request()->kind == PromptKind::palette;
    if (!palette_open) return failure("palette.execute requires the palette to be open");
    auto const candidates = runtime.descriptors();
    bool const published =
        std::any_of(candidates.begin(), candidates.end(),
                    [&](auto const& candidate) { return candidate.id == command_id; });
    if (!published) {
        return failure("command is not in the palette candidate set: " + command_id);
    }
    auto const descriptors = p0_command_descriptors();
    auto const found = std::find_if(
        descriptors.begin(), descriptors.end(),
        [&](CommandDescriptor const& descriptor) { return descriptor.id == command_id; });
    if (found != descriptors.end()) {
        for (auto const& capability : found->required_capabilities) {
            if (!context.principal().has_capability(capability)) {
                return failure("principal lacks capability for palette command: " +
                               command_id);
            }
        }
    }
    return success();
}

CommandHandlerResult search_command(EditorRuntime::Impl& runtime, CommandContext& context, std::string_view id, std::any const& payload) {
    Revision const revision = context.revision();
    if (id == "palette.open") { (void)runtime.prompt.open(palette_prompt_request()); }
    else if (id == "palette.close") { (void)runtime.prompt.cancel(); }
    else if (id == "palette.next" || id == "search.results_next") runtime.search.select_next();
    else if (id == "palette.previous" || id == "search.results_previous") runtime.search.select_previous();
    else if (id == "palette.execute") {
        auto const* arguments = payload_as<PaletteExecuteArguments>(payload);
        if (arguments == nullptr) return failure("palette.execute requires a command id payload");
        auto validation = validate_palette_target(runtime, context, arguments->command_id);
        if (!validation.accepted) return validation;
        runtime.pending_palette_target = arguments->command_id;
        (void)runtime.prompt.cancel();
    } else if (id == "search.workspace") {
        std::string query;
        if (auto const* text = payload_as<std::string>(payload)) query = *text;
        auto request = runtime.search.begin_workspace_search(std::move(query), revision);
        auto batch = runtime.search.evaluate(request);
        (void)runtime.search.publish(batch, revision);
    } else if (id == "goto.back") {
        (void)runtime.navigation.back();
    } else if (id == "goto.forward") {
        (void)runtime.navigation.forward();
    } else if (id == "goto.file" || id == "goto.line" || id == "goto.symbol") {
        auto const* target = payload_as<NavigationTarget>(payload);
        if (target != nullptr) (void)runtime.navigation.visit(*target, NavigationOrigin::user);
        else return failure(std::string{id} + " requires a navigation target payload");
    } else {
        return failure("unknown search command");
    }
    return success();
}

CommandHandlerResult tree_command(EditorRuntime::Impl& runtime, std::string_view id, std::any const& payload) {
    if (id == "tree.select_next") { (void)runtime.tree.select_next(); return success(); }
    if (id == "tree.select_previous") { (void)runtime.tree.select_previous(); return success(); }
    if (id == "tree.activate") {
        auto selected = runtime.tree.selected_node();
        if (!selected) return failure("no tree node is selected");
        if (selected->expandable) { (void)runtime.tree.toggle_selected(); return success(); }
        if (selected->workspace_path) {
            auto result = runtime.workspace.open_file(*selected->workspace_path);
            if (!result.accepted() || !result.document) return failure("failed to open tree file");
            auto opened = runtime.activate_document(*result.document);
            if (opened.accepted) runtime.shell.focus_editor();
            return opened;
        }
        return success();
    }
    auto const* invocation = payload_as<TreeCommandInvocation>(payload);
    if (invocation == nullptr) return failure(std::string{id} + " requires a tree invocation payload");
    if (id == "tree.toggle_expanded") {
        return runtime.tree.toggle_expanded(invocation->provider_id, invocation->node_id) ? success() : failure("tree node does not exist");
    }
    auto command = runtime.tree.invoke_node_command(invocation->provider_id, invocation->node_id, invocation->command_id);
    return command ? success() : failure("tree node command does not exist");
}

CommandHandlerResult diff_command(EditorRuntime::Impl& runtime, std::string_view id, std::any const& payload) {
    auto const* file_id = payload_as<DiffFileId>(payload);
    if (file_id == nullptr) return failure(std::string{id} + " requires a diff file ID payload");
    auto file = runtime.diff.file(*file_id);
    if (!file) return failure("diff file does not exist");
    if (id == "diff.next_hunk") (void)next_diff_hunk(file->get(), std::nullopt);
    else if (id == "diff.previous_hunk") (void)previous_diff_hunk(file->get(), std::nullopt);
    else if (id == "diff.open_file") (void)diff_open_file(file->get());
    return success();
}

CommandHandlerResult follow_command(EditorRuntime::Impl& runtime, std::string_view id) {
    auto result = id == "follow_edits.pause" ? runtime.follow.pause()
                                              : runtime.follow.resume(runtime.diff.view_state());
    return result.accepted() ? success() : failure("follow edits command failed");
}

} // namespace

void bind_runtime_navigation(EditorSessionBuilder& builder, EditorRuntime::Impl& runtime) {
    auto search_commands = search_command_set();
    auto tree_commands = tree_command_set();
    auto diff_commands = diff_command_set();
    auto follow_commands = follow_edits_command_set();
    for (auto const& descriptor : search_commands.descriptors()) {
        builder.bind(std::string{descriptor.id}, [&runtime, descriptor](CommandContext& context, std::any const& payload) {
            return runtime.run_transaction([&] { return search_command(runtime, context, descriptor.id, payload); });
        });
    }
    for (auto const& descriptor : tree_commands.descriptors()) {
        builder.bind(std::string{descriptor.id}, [&runtime, descriptor](CommandContext&, std::any const& payload) {
            return runtime.run_transaction([&] { return tree_command(runtime, descriptor.id, payload); });
        });
    }
    for (auto const& descriptor : diff_commands.descriptors()) {
        builder.bind(std::string{descriptor.id}, [&runtime, descriptor](CommandContext&, std::any const& payload) {
            return runtime.run_transaction([&] { return diff_command(runtime, descriptor.id, payload); });
        });
    }
    for (auto const& descriptor : follow_commands.descriptors()) {
        builder.bind(std::string{descriptor.id}, [&runtime, descriptor](CommandContext&, std::any const&) {
            return runtime.run_transaction([&] { return follow_command(runtime, descriptor.id); });
        });
    }
}

} // namespace ssg
