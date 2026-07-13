#pragma once

// The command palette / fuzzy finder candidate list.  The library publishes the
// authoritative set of things the palette can act on for the current mode
// (registered commands, workspace files, symbols); a client fuzzy-filters and
// ranks this list locally for responsiveness (see doc/spec-palette.md).  The
// ranked view, query, and selection are client-owned presentation, not part of
// this authoritative list.

#include <ssg/search.h>

#include <string>
#include <vector>

namespace ssg {

// One actionable palette entry.  `id` is the command id (or navigation target)
// dispatched on execution; `label` is the display/fuzzy-match text; `detail` is
// optional secondary text (e.g. a bound key sequence or a file's directory).
struct PaletteCandidate {
    std::string id;
    std::string label;
    std::string detail;

    friend bool operator==(const PaletteCandidate&, const PaletteCandidate&) = default;
};

struct PaletteViewState {
    SearchMode mode = SearchMode::command;
    std::vector<PaletteCandidate> candidates;

    friend bool operator==(const PaletteViewState&, const PaletteViewState&) = default;
};

}  // namespace ssg
