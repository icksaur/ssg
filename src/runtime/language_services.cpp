#include "editor_session_internal.h"

namespace ssg {
namespace {

CommandHandlerResult lspFeatureCommand(EditorSession::Impl& runtime, std::string_view id) {
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

CommandHandlerResult lspWorkspaceCommand(EditorSession::Impl& runtime, std::any const& payload) {
    auto const* name = payloadAs<std::string>(payload);
    if (name == nullptr || name->empty()) return failure("rename.symbol requires a new-name payload");
    runtime.lspFeatures.status = "LSP rename is not configured";
    return failure(runtime.lspFeatures.status);
}

} // namespace

// Renaming a symbol across the workspace.  The new name arrives in-process from
// the prompt that collected it.
void registerLspWorkspaceEditCommands(CommandCatalog& builder,
                                      EditorSession::Impl& runtime) {
    builder.add(CommandSpecBuilder{"rename.symbol"}
                    .owner("lsp-workspace-edits")
                    .summary("Symbol")
                    .mutates()
                    .lua()
                    .inProcessHandler<std::string>(
                        [&runtime](CommandContext&, std::string const& name) {
                            return runtime.runTransaction([&] {
                                return lspWorkspaceCommand(runtime,
                                                           std::any{name});
                            });
                        }));
}

// Go-to, completion and hover.  None takes an argument: each acts on wherever
// the cursor already is.
void registerLspFeatureCommands(CommandCatalog& builder,
                                EditorSession::Impl& runtime) {
    auto declare = [&](std::string id, std::string label, std::string summary) {
        auto const name = id;
        auto spec = CommandSpecBuilder{std::move(id)}
                        .owner("lsp-language-features")
                        .summary(std::move(summary))
                        .mutates()
                        .lua()
                        .handler([&runtime, name](CommandContext&) {
                            return runtime.runTransaction([&] {
                                return lspFeatureCommand(runtime, name);
                            });
                        });
        if (!label.empty()) spec.label(std::move(label));
        builder.add(std::move(spec));
    };
    declare("goto.definition", "Go to Definition", "Go to Definition");
    declare("goto.reference", "", "Reference");
    declare("completion.open", "", "Open");
    declare("completion.next", "", "Next");
    declare("completion.previous", "", "Previous");
    declare("completion.accept", "", "Accept");
    declare("completion.dismiss", "", "Dismiss");
    declare("hover.show", "", "Show");
    declare("hover.dismiss", "", "Dismiss");
}

void bindRuntimeLanguageServices(CommandCatalog& builder, EditorSession::Impl& runtime) {
    registerLspWorkspaceEditCommands(builder, runtime);
    registerLspFeatureCommands(builder, runtime);
}

} // namespace ssg
