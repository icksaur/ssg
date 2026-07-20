#include <ssg/file_commands.h>

#include <stdexcept>

namespace ssg {

FileCommandsCommandSet fileCommandsCommandSet() {
    return {};
}

PromptRequest filePathPrompt(FileCommand command) {
    std::string label;
    switch (command) {
        case FileCommand::OpenDirectory:
            label = "Open directory";
            break;
        case FileCommand::Create:
            label = "New file path";
            break;
        case FileCommand::Open:
            label = "Open file";
            break;
        case FileCommand::SaveAs:
            label = "Save file as";
            break;
        case FileCommand::Rename:
            label = "Rename file";
            break;
        case FileCommand::NewDirectory:
            label = "New directory path";
            break;
        default:
            throw std::invalid_argument(
                "file command does not accept a path prompt");
    }
    return {PromptKind::Path, label, {{"path", label, {}}}, {}, std::nullopt};
}

}  // namespace ssg
