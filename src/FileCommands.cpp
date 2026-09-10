#include <ssg/FileCommands.h>

#include <stdexcept>

namespace ssg {

PromptRequest fileCommandPathPrompt(FileCommand command) {
    std::string label;
    PromptCompletion completion;
    switch (command) {
        case FileCommand::OpenDirectory:
            label = "open directory";
            completion = PromptCompletion::WorkspaceOpenDirectory;
            break;
        case FileCommand::Open:
            label = "open file";
            completion = PromptCompletion::FileOpen;
            break;
        case FileCommand::SaveAs:
            label = "save file as";
            completion = PromptCompletion::FileSaveAs;
            break;
        case FileCommand::Rename:
            label = "rename file";
            completion = PromptCompletion::FileRename;
            break;
        case FileCommand::NewDirectory:
            label = "new directory path";
            completion = PromptCompletion::FileNewDirectory;
            break;
        // FileCommand::Create is deliberately absent: file.new creates a buffer
        // immediately, with no payload for an unnamed one or a workspace-relative
        // path to claim a name. It never prompts.
        default:
            throw std::invalid_argument(
                "file command does not accept a path prompt");
    }
    return {PromptKind::Path, label, {{"path", label, {}}}, {}, std::nullopt,
            completion};
}

}  // namespace ssg
