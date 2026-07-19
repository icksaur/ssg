#include <ssg/palette.h>

#include <algorithm>
#include <cctype>
#include <limits>
#include <optional>

namespace ssg {
namespace {

char folded(char value) {
    return static_cast<char>(
        std::tolower(static_cast<unsigned char>(value)));
}

// Subsequence fuzzy score, identical in shape to the search controller's
// scorer: contiguous runs and word-boundary hits are rewarded, exact-case hits
// nudged, and longer candidates lightly penalized.  std::nullopt means the
// query is not a subsequence of the candidate.
std::optional<int> fuzzy_score(std::string_view candidate,
                               std::string_view query) {
    if (query.empty()) return 0;
    int score = 0;
    std::size_t cursor = 0;
    std::size_t previous = std::string_view::npos;
    for (const char wanted : query) {
        const char needle = folded(wanted);
        while (cursor < candidate.size() && folded(candidate[cursor]) != needle) {
            ++cursor;
        }
        if (cursor == candidate.size()) return std::nullopt;
        score += 10;
        if (cursor == 0 || candidate[cursor - 1] == '/' ||
            candidate[cursor - 1] == '_' || candidate[cursor - 1] == '-' ||
            candidate[cursor - 1] == '.') {
            score += 8;
        }
        if (previous != std::string_view::npos && cursor == previous + 1) {
            score += 6;
        }
        if (candidate[cursor] == wanted) ++score;
        previous = cursor++;
    }
    score -= static_cast<int>(
        std::min<std::size_t>(candidate.size(), std::size_t{100}));
    return score;
}

}  // namespace

std::vector<std::size_t> palette_rank(
    std::vector<PaletteCandidate> const& candidates, std::string_view query) {
    struct Ranked {
        std::size_t index;
        int score;
    };
    std::vector<Ranked> ranked;
    ranked.reserve(candidates.size());
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        auto const& candidate = candidates[index];
        auto const label_score = fuzzy_score(candidate.label, query);
        auto const id_score = fuzzy_score(candidate.id, query);
        if (!label_score && !id_score) continue;
        ranked.push_back(
            {index,
             std::max(label_score.value_or(std::numeric_limits<int>::min()),
                      id_score.value_or(std::numeric_limits<int>::min()))});
    }
    std::ranges::stable_sort(ranked, [&](Ranked const& left, Ranked const& right) {
        if (left.score != right.score) return left.score > right.score;
        auto const& l = candidates[left.index];
        auto const& r = candidates[right.index];
        if (l.label != r.label) return l.label < r.label;
        return l.id < r.id;
    });
    std::vector<std::size_t> order;
    order.reserve(ranked.size());
    for (auto const& entry : ranked) order.push_back(entry.index);
    return order;
}

std::string palette_ghost(std::string_view top_label, std::string_view query) {
    if (query.empty() || query.size() >= top_label.size()) return {};
    for (std::size_t index = 0; index < query.size(); ++index) {
        if (folded(top_label[index]) != folded(query[index])) return {};
    }
    return std::string{top_label.substr(query.size())};
}

PaletteReport derive_palette_report(
    std::vector<PaletteCandidate> const& candidates, PaletteWindowState& window) {
    PaletteReport report;
    auto const order = palette_rank(candidates, window.query);
    report.query = window.query;
    if (!order.empty()) {
        report.ghost =
            palette_ghost(candidates[order.front()].label, window.query);
    }
    // Clamp the selection into the (possibly shrunken) ranked set; only when the
    // clamp actually moves it do we re-center the window on it, so a free wheel
    // scroll otherwise persists (see doc/spec-scroll.md, spec-m8.md M8-P).
    bool selection_clamped = false;
    if (window.selected >= order.size()) {
        window.selected = order.empty() ? 0 : order.size() - 1;
        selection_clamped = true;
    }
    std::optional<std::uint32_t> const selected =
        order.empty() ? std::nullopt
                      : std::optional<std::uint32_t>{
                            static_cast<std::uint32_t>(window.selected)};
    auto const scroll = compute_list_scroll_view(
        static_cast<std::uint32_t>(order.size()), window.pane_rows,
        window.first_visible, selected,
        /*keep_selection_visible=*/selection_clamped);
    window.first_visible = scroll.first_visible;
    report.first_visible = scroll.first_visible;
    report.scrollbar = scroll.scrollbar;
    for (std::uint32_t row = 0; row < scroll.visible_count; ++row) {
        report.rows.push_back(candidates[order[scroll.first_visible + row]]);
    }
    report.selected = selected;
    return report;
}

}  // namespace ssg
