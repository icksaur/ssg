#include "editor_runtime_internal.h"

namespace ssg {
namespace {

template <typename T>
T const* payload_as(std::any const& payload) { return std::any_cast<T>(&payload); }

CommandHandlerResult search_command(EditorRuntime::Impl& runtime, Revision revision, std::string_view id, std::any const& payload) {
    if (id == "palette.open") runtime.search.open_palette(revision);
    else if (id == "palette.close") runtime.search.close_palette(revision);
    else if (id == "palette.next" || id == "search.results_next") runtime.search.select_next();
    else if (id == "palette.previous" || id == "search.results_previous") runtime.search.select_previous();
    else if (id == "palette.execute") {
        auto result = runtime.search.execute_palette();
        if (!result.accepted) return failure(result.message);
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
            return runtime.run_transaction([&] { return search_command(runtime, context.revision(), descriptor.id, payload); });
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
