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
        // FileCommand::Create is deliberately absent: file.new takes a display
        // LABEL, not a path, and creates an unnamed buffer immediately. Naming
        // happens later, at save time, through file.save_as.
        default:
            throw std::invalid_argument(
                "file command does not accept a path prompt");
    }
    return {PromptKind::Path, label, {{"path", label, {}}}, {}, std::nullopt,
            std::move(id)};
}

}  // namespace ssg
