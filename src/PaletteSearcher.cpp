#include <ssg/PaletteSearcher.h>

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

std::optional<int> fuzzyScore(std::string_view candidate,
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

std::vector<std::size_t> PaletteSearcher::rank(
    std::vector<PaletteCandidate> const& candidates, std::string_view query) const {
    struct Ranked {
        std::size_t index;
        int score;
    };
    std::vector<Ranked> ranked;
    ranked.reserve(candidates.size());
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        auto const& candidate = candidates[index];
        auto const labelScore = fuzzyScore(candidate.label, query);
        auto const idScore = fuzzyScore(candidate.id, query);
        if (!labelScore && !idScore) continue;
        ranked.push_back(
            {index,
             std::max(labelScore.value_or(std::numeric_limits<int>::min()),
                      idScore.value_or(std::numeric_limits<int>::min()))});
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

std::string PaletteSearcher::ghost(std::string_view topLabel,
                                   std::string_view query) const {
    if (query.empty() || query.size() >= topLabel.size()) return {};
    for (std::size_t index = 0; index < query.size(); ++index) {
        if (folded(topLabel[index]) != folded(query[index])) return {};
    }
    return std::string{topLabel.substr(query.size())};
}

PaletteReport PaletteSearcher::report(
    std::vector<PaletteCandidate> const& candidates, PaletteWindowState& window) const {
    PaletteReport report;
    auto const order = rank(candidates, window.query);
    report.query = window.query;
    if (!order.empty()) {
        report.ghost = ghost(candidates[order.front()].label, window.query);
    }
    bool selectionClamped = false;
    if (window.selected >= order.size()) {
        window.selected = order.empty() ? 0 : order.size() - 1;
        selectionClamped = true;
    }
    std::optional<std::uint32_t> const selected =
        order.empty() ? std::nullopt
                      : std::optional<std::uint32_t>{
                            static_cast<std::uint32_t>(window.selected)};
    auto const scroll = Viewport{}.listScrollView(
        static_cast<std::uint32_t>(order.size()), window.paneRows,
        window.firstVisible, selected,
        /*keep_selection_visible=*/selectionClamped);
    window.firstVisible = scroll.firstVisible;
    report.firstVisible = scroll.firstVisible;
    report.scrollbar = scroll.scrollbar;
    for (std::uint32_t row = 0; row < scroll.visibleCount; ++row) {
        report.rows.push_back(candidates[order[scroll.firstVisible + row]]);
    }
    report.selected = selected;
    return report;
}

}  // namespace ssg
