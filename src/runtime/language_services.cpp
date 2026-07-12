#include "editor_runtime_internal.h"

namespace ssg {
namespace {

template <typename T>
T const* payload_as(std::any const& payload) { return std::any_cast<T>(&payload); }

CommandHandlerResult lsp_feature_command(EditorRuntime::Impl& runtime, std::string_view id) {
    if (id == "completion.dismiss") {
        runtime.lsp_features.completion.visible = false;
        return success();
    }
    if (id == "hover.dismiss") {
        runtime.lsp_features.hover.reset();
        return success();
    }
    runtime.lsp_features.status = "LSP is not configured";
    return failure(runtime.lsp_features.status);
}

CommandHandlerResult lsp_workspace_command(EditorRuntime::Impl& runtime, std::any const& payload) {
    auto const* name = payload_as<std::string>(payload);
    if (name == nullptr || name->empty()) return failure("rename.symbol requires a new-name payload");
    runtime.lsp_features.status = "LSP rename is not configured";
    return failure(runtime.lsp_features.status);
}

} // namespace

void bind_runtime_language_services(EditorSessionBuilder& builder, EditorRuntime::Impl& runtime) {
    auto feature_commands = lsp_feature_command_set();
    auto workspace_commands = lsp_workspace_edit_command_set();
    for (auto const& descriptor : feature_commands.descriptors()) {
        builder.bind(std::string{descriptor.id}, [&runtime, descriptor](CommandContext&, std::any const&) {
            return runtime.run_transaction([&] { return lsp_feature_command(runtime, descriptor.id); });
        });
    }
    for (auto const& descriptor : workspace_commands.descriptors()) {
        builder.bind(std::string{descriptor.id}, [&runtime](CommandContext&, std::any const& payload) {
            return runtime.run_transaction([&] { return lsp_workspace_command(runtime, payload); });
        });
    }
}

} // namespace ssg
