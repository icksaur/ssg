#pragma once

#include <ssg/Search.h>
#include <ssg/Viewport.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace ssg {

struct PaletteCandidate {
    std::string id;
    std::string label;
    std::string detail;

    friend bool operator==(const PaletteCandidate&, const PaletteCandidate&) = default;
};

// The fuzzy-match scoring weights and length cap, published with the candidate
// universe so a client scores against the SAME parameters the library uses -- the
// single source of truth for the shared match/order contract. A client hardcodes
// none of these; it reads them from the PaletteViewState it receives. The defaults
// here ARE that source; the wire carries them so a client can never match candidates
// without the parameters that score them.
struct MatcherParameters {
    // Per matched byte.
    int baseScore = 10;
    // Added when a matched byte begins a word (index 0, or preceded by / _ - .).
    int wordBoundaryBonus = 8;
    // Added when a matched byte is adjacent to the previous match.
    int contiguityBonus = 6;
    // Added when the raw (unfolded) byte matches the query byte exactly.
    int exactCaseBonus = 1;
    // Subtracted: min(candidate byte length, lengthCap) -- shorter candidates win.
    int lengthCap = 100;

    friend bool operator==(const MatcherParameters&, const MatcherParameters&) =
        default;
};

// The magnitude every published matcher weight and the length cap must stay within.
// A parameter outside this domain is a rejected frame. Published on the palette wire
// so a client validates against the same bound rather than hardcoding a copy.
inline constexpr std::int64_t kMaxMatcherParameterMagnitude = 1'000'000;

// The maximum byte length of a candidate's id or label the matcher will score. A
// longer candidate is a rejected frame (never truncated -- the match contract is a
// full-byte subsequence). Published on the palette wire alongside the magnitude.
inline constexpr std::int64_t kMaxCandidateBytes = 1'000'000;

// The number of weight terms that may be added for a single matched candidate byte
// (base + word-boundary + contiguity + exact-case). A proof constant, named so the
// exactness bound below is symbols, not a literal.
inline constexpr std::int64_t kMaxWeightsPerScoredByte = 4;

// The largest integer a double represents exactly (one past Number.MAX_SAFE_INTEGER).
// A score whose magnitude stays below this is bit-identical between the C++ int64 score
// and the JavaScript double score.
inline constexpr std::int64_t kMaxExactDoubleInteger = std::int64_t{1} << 53;

// At most kMaxWeightsPerScoredByte weights are added per matched candidate byte, over at
// most kMaxCandidateBytes bytes, and a length penalty of at most
// kMaxMatcherParameterMagnitude is then applied; the total magnitude must stay exactly
// representable as a double. This is the matcher-owned safety proof, independent of any
// wire limit.
static_assert(kMaxWeightsPerScoredByte * kMaxMatcherParameterMagnitude *
                      kMaxCandidateBytes +
                  kMaxMatcherParameterMagnitude <
              kMaxExactDoubleInteger);

// True iff every weight is within +/- kMaxMatcherParameterMagnitude and lengthCap is
// within [0, kMaxMatcherParameterMagnitude].
[[nodiscard]] bool matcherParametersInDomain(const MatcherParameters& params);

struct PaletteViewState {
    PaletteViewState();

    std::optional<SearchMode> activeMode;
    std::string commandOpenCommandId;
    std::string fileOpenCommandId;
    std::vector<PaletteCandidate> commandCandidates;
    std::vector<PaletteCandidate> fileCandidates;
    // The parameters a client must score `candidates` with; travels on the same
    // channel so candidates and their scoring arrive atomically.
    MatcherParameters parameters;

    [[nodiscard]] const std::vector<PaletteCandidate>* candidatesFor(
        SearchMode mode) const noexcept;

    friend bool operator==(const PaletteViewState&, const PaletteViewState&) = default;
};

struct PaletteExecuteArguments {
    std::string commandId;

    friend bool operator==(const PaletteExecuteArguments&, const PaletteExecuteArguments&) = default;
};

struct PickerSubmitArguments {
    SearchMode mode = SearchMode::Command;
    std::string candidateId;

    friend bool operator==(const PickerSubmitArguments&,
                           const PickerSubmitArguments&) = default;
};

struct PaletteReport {
    std::string query;
    std::string ghost;
    std::vector<PaletteCandidate> rows;
    std::optional<std::uint32_t> selected;
    std::uint32_t firstVisible = 0;
    ScrollbarMetrics scrollbar{};

    friend bool operator==(const PaletteReport&, const PaletteReport&) = default;
};

struct PaletteWindowState {
    std::string query;
    std::size_t selected = 0;
    std::uint32_t firstVisible = 0;
    std::uint32_t paneRows = 1;
};

class PaletteSearcher {
public:
    [[nodiscard]] std::vector<std::size_t> rank(
        std::vector<PaletteCandidate> const& candidates,
        std::string_view query) const;

    [[nodiscard]] std::string ghost(std::string_view topLabel,
                                    std::string_view query) const;

    [[nodiscard]] PaletteReport report(
        std::vector<PaletteCandidate> const& candidates,
        PaletteWindowState& window) const;
};

// The shared match-and-order contract, as a pure function of the published state.
// This is the SPECIFICATION a responsive client executes locally against the same
// candidate universe and parameters the library published, so the library reference
// and every client matcher produce identical match sets and orderings. Match: the
// query is a case-folded (ASCII-only) subsequence of a candidate's label OR id, over
// raw UTF-8 bytes. Order: descending score (scored with `state.parameters`), ties
// broken by label then id ascending, stable. Returns candidate indices in order;
// non-matches are dropped. Throws std::invalid_argument if `state.parameters` are out
// of domain or a candidate exceeds kMaxCandidateBytes -- the score-exactness invariant
// cannot hold for such input, so it is refused rather than silently normalized (the
// honest wire path never produces it; decodePalette rejects such a frame first).
[[nodiscard]] std::vector<std::size_t> referenceRank(
    PaletteViewState const& state, SearchMode mode, std::string_view query);

}  // namespace ssg
