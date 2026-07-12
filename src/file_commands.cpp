#include <ssg/file_commands.h>

#include <stdexcept>

namespace ssg {

FileCommandsCommandSet file_commands_command_set() {
    return {};
}

PromptRequest file_path_prompt(FileCommand command) {
    std::string label;
    switch (command) {
        case FileCommand::open_directory:
            label = "Open directory";
            break;
        case FileCommand::create:
            label = "New file path";
            break;
        case FileCommand::open:
            label = "Open file";
            break;
        case FileCommand::save_as:
            label = "Save file as";
            break;
        case FileCommand::rename:
            label = "Rename file";
            break;
        case FileCommand::new_directory:
            label = "New directory path";
            break;
        default:
            throw std::invalid_argument(
                "file command does not accept a path prompt");
    }
    return {PromptKind::path, label, {{"path", label, {}}}, {}, std::nullopt};
}

}  // namespace ssg
