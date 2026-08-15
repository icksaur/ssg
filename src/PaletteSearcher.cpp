#include <ssg/PaletteSearcher.h>

#include <algorithm>
#include <limits>
#include <optional>

namespace ssg {
namespace {

// ASCII-only case fold: A-Z -> a-z, every other byte (including non-ASCII UTF-8
// continuation bytes) folds to itself. Deliberately NOT locale std::tolower, so the
// C++ reference and a client matcher agree byte-for-byte regardless of locale.
char folded(char value) {
    unsigned char byte = static_cast<unsigned char>(value);
    if (byte >= 'A' && byte <= 'Z') return static_cast<char>(byte - 'A' + 'a');
    return static_cast<char>(byte);
}

// Score `query` as a case-folded subsequence of `candidate`, iterating raw UTF-8
// bytes; nullopt when `query` is not a subsequence. Weights come from `params` so
// the score is a pure function of the published parameters.
std::optional<int> fuzzyScore(std::string_view candidate, std::string_view query,
                              MatcherParameters const& params) {
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
        score += params.baseScore;
        if (cursor == 0 || candidate[cursor - 1] == '/' ||
            candidate[cursor - 1] == '_' || candidate[cursor - 1] == '-' ||
            candidate[cursor - 1] == '.') {
            score += params.wordBoundaryBonus;
        }
        if (previous != std::string_view::npos && cursor == previous + 1) {
            score += params.contiguityBonus;
        }
        if (candidate[cursor] == wanted) score += params.exactCaseBonus;
        previous = cursor++;
    }
    score -= static_cast<int>(std::min<std::size_t>(
        candidate.size(), static_cast<std::size_t>(std::max(params.lengthCap, 0))));
    return score;
}

std::vector<std::size_t> rankWith(std::vector<PaletteCandidate> const& candidates,
                                  std::string_view query,
                                  MatcherParameters const& params) {
    struct Ranked {
        std::size_t index;
        int score;
    };
    std::vector<Ranked> ranked;
    ranked.reserve(candidates.size());
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        auto const& candidate = candidates[index];
        auto const labelScore = fuzzyScore(candidate.label, query, params);
        auto const idScore = fuzzyScore(candidate.id, query, params);
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

}  // namespace

std::vector<std::size_t> referenceRank(PaletteViewState const& state,
                                       std::string_view query) {
    return rankWith(state.candidates, query, state.parameters);
}

bool matcherParametersInDomain(const MatcherParameters& params) {
    constexpr int m = kMaxMatcherParameterMagnitude;
    auto ok = [](int value, int lo, int hi) { return value >= lo && value <= hi; };
    return ok(params.baseScore, -m, m) && ok(params.wordBoundaryBonus, -m, m) &&
           ok(params.contiguityBonus, -m, m) && ok(params.exactCaseBonus, -m, m) &&
           ok(params.lengthCap, 0, m);
}

std::vector<std::size_t> PaletteSearcher::rank(
    std::vector<PaletteCandidate> const& candidates, std::string_view query) const {
    return rankWith(candidates, query, MatcherParameters{});
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
