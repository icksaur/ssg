#pragma once

#include <ssg/CommandHandle.h>
#include <ssg/Search.h>

#include <any>
#include <optional>
#include <string>

namespace ssg {

// The command a palette/finder submit resolves to for a given picker mode and
// selected candidate id. Submitting the selected row means different things per
// picker -- a command id for the command palette, a path for the file finder --
// so this decision lives once, called by every client, rather than duplicated in
// each client's submit handler.
struct PaletteSubmitCommand {
    CommandName command;
    std::any payload;
};

// nullopt for a mode with no submit action (Line/Symbol/Text have no picker
// submit today), so a caller neither dispatches nor guesses.
[[nodiscard]] std::optional<PaletteSubmitCommand> paletteSubmitCommand(
    SearchMode mode, std::string candidateId);

}  // namespace ssg
