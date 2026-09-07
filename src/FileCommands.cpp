#include <ssg/FileCommands.h>

#include <stdexcept>

namespace ssg {

PromptRequest fileCommandPathPrompt(FileCommand command) {
    std::string label;
    std::string id;
    switch (command) {
        case FileCommand::OpenDirectory:
            label = "open directory";
            id = "workspace.open_directory";
            break;
        case FileCommand::Open:
            label = "open file";
            id = "file.open";
            break;
        case FileCommand::SaveAs:
            label = "save file as";
            id = "file.save_as";
            break;
        case FileCommand::Rename:
            label = "rename file";
            id = "file.rename";
            break;
        case FileCommand::NewDirectory:
            label = "new directory path";
            id = "file.new_directory";
            break;
        // FileCommand::Create is deliberately absent: file.new creates a buffer
        // immediately, with no payload for an unnamed one or a workspace-relative
        // path to claim a name. It never prompts.
        default:
            throw std::invalid_argument(
                "file command does not accept a path prompt");
    }
    return {PromptKind::Path, label, {{"path", label, {}}}, {}, std::nullopt,
            std::move(id)};
}

}  // namespace ssg
