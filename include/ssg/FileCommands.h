#pragma once

#include <ssg/Workspace.h>
#include <ssg/PromptSurface.h>

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

// CONTRACT: This is the single file-command inventory. Registration and
// dispatch policy consume it rather than restating command facts.
struct FileCommandDescriptor {
    std::string_view id;
    FileCommand command;
    bool lua = true;
    // Takes a workspace-relative path, and so must open the path prompt when
    // dispatched without one. Declaring it here is what lets a test enumerate
    // the path-taking commands and prove each one is wired; without the flag
    // there is nothing to enumerate and the check passes vacuously.
    bool pathPrompt = false;
    // Changes the file behind the ACTIVE tab. A live diff tab is a computed
    // view of two revisions and has no such file, so these are refused there.
    // Declared per command rather than decided by a switch, because a switch
    // over this enum does NOT fail to compile when a command is added
    // (verified: adding an enumerator built cleanly), so a switch would let a
    // new command silently miss the rule.
    bool mutatesActiveDocumentFile = false;

};

inline constexpr std::array<FileCommandDescriptor, 12> kFileCommands{{
    {"workspace.open_directory", FileCommand::OpenDirectory, true, true, false},
    {"file.new", FileCommand::Create, true, false, false},
    {"file.open", FileCommand::Open, true, true, false},
    {"file.open_recent", FileCommand::OpenRecent, true, false, false},
    {"file.open_dropped_content", FileCommand::OpenDroppedContent, false, false,
     false},
    {"file.save", FileCommand::Save, true, false, true},
    {"file.save_all", FileCommand::SaveAll, true, false, false},
    {"file.save_as", FileCommand::SaveAs, true, true, true},
    {"file.reload", FileCommand::Reload, true, false, true},
    {"file.rename", FileCommand::Rename, true, true, true},
    {"file.delete", FileCommand::Remove, true, false, true},
    {"file.new_directory", FileCommand::NewDirectory, true, true, false},
}};

[[nodiscard]] PromptRequest fileCommandPathPrompt(FileCommand command);

}  // namespace ssg
