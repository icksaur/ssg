#include <ssg/PaletteSubmit.h>

#include <ssg/PaletteSearcher.h>

namespace ssg {

std::optional<PaletteSubmitCommand> paletteSubmitCommand(
    PickerActivation activation, std::string candidateId) {
    switch (activation.mode) {
    case SearchMode::Command:
    case SearchMode::File:
        return PaletteSubmitCommand{
            CommandName{"picker.submit"},
            PickerSubmitArguments{activation, std::move(candidateId)}};
    case SearchMode::Line:
    case SearchMode::Symbol:
    case SearchMode::Text:
        return std::nullopt;
    }
    return std::nullopt;
}

}  // namespace ssg
