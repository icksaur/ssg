#pragma once

#include <ssg/CommandHandle.h>
#include <ssg/Picker.h>
#include <ssg/Search.h>

#include <any>
#include <optional>
#include <string>

namespace ssg {

// The generic picker command and typed payload for a selected candidate. Every
// client routes through this seam so validation, action dispatch, and the
// authoritative close transition cannot diverge by transport.
struct PaletteSubmitCommand {
    CommandName command;
    std::any payload;
};

// nullopt for a mode with no submit action (Line/Symbol/Text have no picker
// submit today), so a caller neither dispatches nor guesses.
[[nodiscard]] std::optional<PaletteSubmitCommand> paletteSubmitCommand(
    PickerActivation activation, std::string candidateId);

}  // namespace ssg
