#include <ssg/Editor.h>

namespace ssg {
namespace {

OperationResult lspFeatureCommand(Editor& runtime, std::string_view id) {
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
void registerLspFeatureCommands(Commands& commands, Editor& runtime) {
    auto declare = [&](std::string id, std::string label) {
        auto const name = id;
        commands.add(std::move(id), std::move(label), [&runtime, name] {
            return lspFeatureCommand(runtime, name);
        });
    };
    declare("goto.definition", "Go to Definition");
    declare("goto.reference", "Goto Reference");
    declare("completion.open", "Completion Open");
    declare("completion.next", "Completion Next");
    declare("completion.previous", "Completion Previous");
    declare("completion.accept", "Completion Accept");
    declare("completion.dismiss", "Completion Dismiss");
    declare("hover.show", "Hover Show");
    declare("hover.dismiss", "Hover Dismiss");
}

void bindRuntimeLanguageServices(Commands& commands, Editor& runtime) {
    registerLspFeatureCommands(commands, runtime);
}

} // namespace ssg
