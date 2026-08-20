#pragma once

#include <ssg/EditorClient.h>
#include <ssg/Keymap.h>

#include <optional>
#include <string>

namespace ssg {

struct ClientKeyInput {
    KeyStroke stroke;
    std::string committedText;
};

enum class ClientOwnedInputKind : std::uint8_t {
    AppendText,
    DeleteGraphemeBackward,
    DeleteWordBackward,
    SelectNext,
    SelectPrevious,
    Submit,
};

struct ClientOwnedInput {
    ClientOwnedInputKind kind;
    std::string text;
};

enum class ClientInputOutcome : std::uint8_t {
    Unhandled,
    ClientOwned,
    Dispatched,
    Rejected,
};

struct ClientInputResult {
    ClientInputOutcome outcome;
    std::optional<ClientOwnedInput> clientOwned;
    std::optional<CommandResult> command;
};

}  // namespace ssg
