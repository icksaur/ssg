#pragma once

#include "ssg/prompt.h"
#include "ssg/workspace.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace ssg {

enum class FileCommand : std::uint8_t {
    open_directory,
    create,
    open,
    open_recent,
    open_dropped_content,
    save,
    save_all,
    save_as,
    reload,
    rename,
    remove,
    new_directory,
};

struct FileCommandDescriptor {
    std::string_view id;
    FileCommand command;
    bool lua = true;
    std::optional<std::string_view> required_capability;

    friend bool operator==(const FileCommandDescriptor&,
                           const FileCommandDescriptor&) = default;
};

class FileCommandsCommandSet {
public:
    [[nodiscard]] const std::array<FileCommandDescriptor, 12>& descriptors()
        const noexcept {
        return descriptors_;
    }

private:
    const std::array<FileCommandDescriptor, 12> descriptors_{{
        {"workspace.open_directory", FileCommand::open_directory},
        {"file.new", FileCommand::create},
        {"file.open", FileCommand::open},
        {"file.open_recent", FileCommand::open_recent},
        {"file.open_dropped_content", FileCommand::open_dropped_content, false,
         std::string_view{"local_file_drop"}},
        {"file.save", FileCommand::save},
        {"file.save_all", FileCommand::save_all},
        {"file.save_as", FileCommand::save_as},
        {"file.reload", FileCommand::reload},
        {"file.rename", FileCommand::rename},
        {"file.delete", FileCommand::remove},
        {"file.new_directory", FileCommand::new_directory},
    }};
};

[[nodiscard]] FileCommandsCommandSet file_commands_command_set();
[[nodiscard]] PromptRequest file_path_prompt(FileCommand command);

}  // namespace ssg
