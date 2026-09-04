#include <ssg/EditorSessionImpl.h>

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

} // namespace

// Go-to, completion and hover.  None takes an argument: each acts on wherever
// the cursor already is.
void registerLspFeatureCommands(CommandCatalog& catalog,
                                EditorSession::Impl& runtime) {
    auto declare = [&](std::string id, std::string label, std::string summary) {
        auto const name = id;
        CommandSpec spec{
            .id = std::move(id),
            .owner = "lsp-language-features",
            .summary = std::move(summary),
            .effect = CommandEffect::Mutation,
            .luaApi = true,
            .binding = bindNoArgumentHandler([&runtime, name](CommandContext&) {
                return lspFeatureCommand(runtime, name);
            }),
        };
        if (!label.empty()) spec.label = std::move(label);
        catalog.add(std::move(spec));
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

void bindRuntimeLanguageServices(CommandCatalog& catalog, EditorSession::Impl& runtime) {
    registerLspFeatureCommands(catalog, runtime);
}

} // namespace ssg
