#include <ssg/PaletteSearcher.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>

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
// bytes over the WHOLE candidate (a full-byte subsequence, never truncated); nullopt
// when `query` is not a subsequence. `params` is already clamped into the domain and
// `candidate` is already within kMaxCandidateBytes (rankWith enforces both), so the
// int64 score cannot overflow and is bit-identical to the JS double score -- the
// header's static_assert proves that from those two bounds.
std::optional<std::int64_t> fuzzyScore(std::string_view candidate,
                                       std::string_view query,
                                       MatcherParameters const& params) {
    if (query.empty()) return 0;
    std::int64_t score = 0;
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
    score -= static_cast<std::int64_t>(std::min<std::size_t>(
        candidate.size(), static_cast<std::size_t>(std::max(params.lengthCap, 0))));
    return score;
}

std::vector<std::size_t> rankWith(std::vector<PaletteCandidate> const& candidates,
                                  std::string_view query,
                                  MatcherParameters const& params) {
    // Reject loudly at the matcher boundary, matching the web client: malformed input
    // (out-of-domain weights, or a candidate over kMaxCandidateBytes) would breach the
    // score-exactness invariant, so it is refused rather than silently normalized into a
    // plausible ranking. The honest wire path never reaches here -- decode already
    // rejected such a frame -- so this is a programming-error signal for an in-process
    // caller, not an expected runtime outcome.
    if (!matcherParametersInDomain(params)) {
        throw std::invalid_argument(
            "PaletteSearcher: matcher parameters out of domain");
    }
    const std::size_t maxBytes = static_cast<std::size_t>(kMaxCandidateBytes);
    for (auto const& candidate : candidates) {
        if (candidate.label.size() > maxBytes || candidate.id.size() > maxBytes) {
            throw std::invalid_argument(
                "PaletteSearcher: candidate exceeds the score-exact byte bound");
        }
    }
    struct Ranked {
        std::size_t index;
        std::int64_t score;
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
             std::max(labelScore.value_or(std::numeric_limits<std::int64_t>::min()),
                      idScore.value_or(std::numeric_limits<std::int64_t>::min()))});
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
    constexpr std::int64_t m = kMaxMatcherParameterMagnitude;
    auto ok = [](std::int64_t value, std::int64_t lo, std::int64_t hi) {
        return value >= lo && value <= hi;
    };
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
