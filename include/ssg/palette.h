#pragma once

// The command palette / fuzzy finder candidate list.  The library publishes the
// authoritative set of things the palette can act on for the current mode
// (registered commands, workspace files, symbols); a client fuzzy-filters and
// ranks this list locally for responsiveness (see doc/spec-palette.md).  The
// ranked view, query, and selection are client-owned presentation, not part of
// this authoritative list.

#include <ssg/search.h>

#include <cstdint>
#include <optional>
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

// A client's locally-ranked palette view, reported for library-owned
// presentation (see doc/spec-palette.md).  `rows` is the bounded visible window
// of ranked candidates; `selected` indexes into it.  The library projects this
// into the active pane only while the palette prompt is open.
struct PaletteReport {
    std::string query;
    std::vector<PaletteCandidate> rows;
    std::optional<std::uint32_t> selected;

    friend bool operator==(const PaletteReport&, const PaletteReport&) = default;
};

}  // namespace ssg
