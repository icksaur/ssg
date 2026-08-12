#include <ssg/PaletteSubmit.h>

#include <ssg/PaletteSearcher.h>

namespace ssg {

std::optional<PaletteSubmitCommand> paletteSubmitCommand(
    SearchMode mode, std::string candidateId) {
    switch (mode) {
    case SearchMode::Command:
        return PaletteSubmitCommand{CommandName{"palette.execute"},
                                    PaletteExecuteArguments{std::move(candidateId)}};
    case SearchMode::File:
        // The runtime closes the file picker on a successful file.open, so no
        // client needs to close it; a rejected open leaves the query intact.
        return PaletteSubmitCommand{CommandName{"file.open"},
                                    std::move(candidateId)};
    case SearchMode::Line:
    case SearchMode::Symbol:
    case SearchMode::Text:
        return std::nullopt;
    }
    return std::nullopt;
}

}  // namespace ssg
