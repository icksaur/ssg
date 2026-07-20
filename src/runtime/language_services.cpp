#include "editor_runtime_internal.h"

namespace ssg {
namespace {

template <typename T>
T const* payloadAs(std::any const& payload) { return std::any_cast<T>(&payload); }

CommandHandlerResult lspFeatureCommand(EditorRuntime::Impl& runtime, std::string_view id) {
    if (id == "completion.dismiss") {
        runtime.lspFeatures.completion.visible = false;
        return success();
    }
    if (id == "hover.dismiss") {
        runtime.lspFeatures.hover.reset();
        return success();
    }
    runtime.lspFeatures.status = "LSP is not configured";
    return failure(runtime.lspFeatures.status);
}

CommandHandlerResult lspWorkspaceCommand(EditorRuntime::Impl& runtime, std::any const& payload) {
    auto const* name = payloadAs<std::string>(payload);
    if (name == nullptr || name->empty()) return failure("rename.symbol requires a new-name payload");
    runtime.lspFeatures.status = "LSP rename is not configured";
    return failure(runtime.lspFeatures.status);
}

} // namespace

void bindRuntimeLanguageServices(EditorSessionBuilder& builder, EditorRuntime::Impl& runtime) {
    auto featureCommands = lspFeatureCommandSet();
    auto workspaceCommands = lspWorkspaceEditCommandSet();
    for (auto const& descriptor : featureCommands.descriptors()) {
        builder.bind(std::string{descriptor.id}, [&runtime, descriptor](CommandContext&, std::any const&) {
            return runtime.runTransaction([&] { return lspFeatureCommand(runtime, descriptor.id); });
        });
    }
    for (auto const& descriptor : workspaceCommands.descriptors()) {
        builder.bind(std::string{descriptor.id}, [&runtime](CommandContext&, std::any const& payload) {
            return runtime.runTransaction([&] { return lspWorkspaceCommand(runtime, payload); });
        });
    }
}

} // namespace ssg
