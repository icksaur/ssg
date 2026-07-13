#pragma once

// The command palette / fuzzy finder candidate list.  The library publishes the
// authoritative set of things the palette can act on for the current mode
// (registered commands, workspace files, symbols); a client fuzzy-filters and
// ranks this list locally for responsiveness (see doc/spec-palette.md).  The
// ranked view, query, and selection are client-owned presentation, not part of
// this authoritative list.

#include <ssg/search.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
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

// The typed argument for `palette.execute`: the id of the selected candidate the
// server should execute.  Carrying the id (rather than a client-held selection
// index) lets the server validate membership in the published candidate set and
// keeps the command's argument shape explicit on the wire (see spec-palette.md).
struct PaletteExecuteArguments {
    std::string command_id;

    friend bool operator==(const PaletteExecuteArguments&, const PaletteExecuteArguments&) = default;
};

// A client's locally-ranked palette view, reported for library-owned
// presentation (see doc/spec-palette.md).  `rows` is the bounded visible window
// of ranked candidates; `selected` indexes into it.  `ghost` is the remaining
// characters of the top candidate's label after the query (fish-style
// completion), shown dim in the header.  The library projects this into the
// active pane and header only while the palette prompt is open.
struct PaletteReport {
    std::string query;
    std::string ghost;
    std::vector<PaletteCandidate> rows;
    std::optional<std::uint32_t> selected;

    friend bool operator==(const PaletteReport&, const PaletteReport&) = default;
};

// The authoritative fuzzy ranker for the palette.  Returns indices into
// `candidates`, keeping only entries whose `label` or `id` subsequence-matches
// `query`, ordered by descending score with a stable tiebreak (label ascending,
// then id ascending).  An empty query keeps every candidate in `label`/`id`
// order.  This is the single scoring algorithm every client shares so no two
// rankers diverge (spec P5); the TUI reports a window of this order.
[[nodiscard]] std::vector<std::size_t> palette_rank(
    std::vector<PaletteCandidate> const& candidates, std::string_view query);

// The fish-style ghost completion for `query` given the top-ranked candidate's
// label: the label's remaining characters when the label starts with `query`
// (case-insensitively), else empty.  Presentation-only; never mutates state.
[[nodiscard]] std::string palette_ghost(std::string_view top_label,
                                        std::string_view query);

}  // namespace ssg
