#pragma once

#include "ssg/PromptSurface.h"
#include "ssg/Workspace.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ssg {

// Ingress-only payload for `file.open_dropped_content`: this command has no
// existing bundled argument type because `Workspace::open_dropped_content`
// takes its bytes and label as separate parameters. The protocol codec needs
// one std::any-held type to bind the command to a wire converter.
struct DroppedContentArguments {
    std::vector<std::uint8_t> bytes;
    std::string suggestedLabel;

    bool operator==(const DroppedContentArguments&) const = default;
};

enum class FileCommand : std::uint8_t {
    OpenDirectory,
    Create,
    Open,
    OpenRecent,
    OpenDroppedContent,
    Save,
    SaveAll,
    SaveAs,
    Reload,
    Rename,
    Remove,
    NewDirectory,
};

struct FileCommandDescriptor {
    std::string_view id;
    FileCommand command;
    bool lua = true;
    std::optional<std::string_view> requiredCapability;

    friend bool operator==(const FileCommandDescriptor&,
                           const FileCommandDescriptor&) = default;
};

class FileCommandsCommandSet {
public:
    [[nodiscard]] const std::array<FileCommandDescriptor, 12>& descriptors()
        const noexcept {
        return descriptors_;
    }

    // The path-entry prompt for a command that takes a filesystem path
    // (throws for a command that accepts no path prompt).
    [[nodiscard]] PromptRequest pathPrompt(FileCommand command) const;

private:
    const std::array<FileCommandDescriptor, 12> descriptors_{{
        {"workspace.open_directory", FileCommand::OpenDirectory},
        {"file.new", FileCommand::Create},
        {"file.open", FileCommand::Open},
        {"file.open_recent", FileCommand::OpenRecent},
        {"file.open_dropped_content", FileCommand::OpenDroppedContent, false,
         std::string_view{"local_file_drop"}},
        {"file.save", FileCommand::Save},
        {"file.save_all", FileCommand::SaveAll},
        {"file.save_as", FileCommand::SaveAs},
        {"file.reload", FileCommand::Reload},
        {"file.rename", FileCommand::Rename},
        {"file.delete", FileCommand::Remove},
        {"file.new_directory", FileCommand::NewDirectory},
    }};
};

[[nodiscard]] FileCommandsCommandSet fileCommandsCommandSet();

}  // namespace ssg
