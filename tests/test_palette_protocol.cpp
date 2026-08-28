// Round-trip + rejection oracle (contract) for the palette-section wire codec, and
// the single-source-of-truth check for the published matcher parameters. The
// knowable answers: encode->decode recovers a PaletteViewState exactly (candidates,
// mode, and matcher parameters); a value the codec OWNS as malformed (a candidate
// missing id/label/detail, a MatcherParameters missing any field, an out-of-range
// mode, a non-array candidates field) decodes to nullopt, never a partial; and the
// parameters a fresh PaletteViewState publishes ARE the library default constants
// (the client reads them off the wire and hardcodes none).

#include "ssg/PaletteProtocol.h"

#include "ssg/PaletteSearcher.h"
#include "test_helpers.h"

#include <optional>
#include <string>

namespace {

using namespace ssg;

PaletteViewState sample() {
    PaletteViewState state;
    state.activeMode = SearchMode::Command;
    state.commandCandidates = {
        {"edit.undo", "Undo", "Ctrl+Z"},
        {"file.save", "Save File", ""},
        {"vue.open", "Open \xC3\xA9\x63lair", "non-ascii detail"},
    };
    state.fileCandidates = {
        {"src/main.cpp", "src/main.cpp", ""},
    };
    return state;
}

ProtocolValue paletteWire(
    ProtocolValue commandCandidates = ProtocolValue::makeArray({}),
    ProtocolValue fileCandidates = ProtocolValue::makeArray({}),
    ProtocolValue parameters =
        *encodePalette(PaletteViewState{}).field("parameters"),
    ProtocolValue activeMode = ProtocolValue::makeNull(),
    ProtocolValue magnitude =
        ProtocolValue::makeInt(kMaxMatcherParameterMagnitude),
    ProtocolValue candidateBytes =
        ProtocolValue::makeInt(kMaxCandidateBytes)) {
    return ProtocolValue::makeObject(
        {{"active_mode", std::move(activeMode)},
         {"command_open_command_id", ProtocolValue::makeText("palette.open")},
         {"command_candidates", std::move(commandCandidates)},
         {"file_open_command_id",
          ProtocolValue::makeText("file_finder.open")},
         {"file_candidates", std::move(fileCandidates)},
         {"parameters", std::move(parameters)},
         {"max_parameter_magnitude", std::move(magnitude)},
         {"max_candidate_bytes", std::move(candidateBytes)}});
}

TEST(encodeDecodeRoundTripsExactly) {
    const auto state = sample();
    const auto decoded = decodePalette(encodePalette(state));
    ASSERT_TRUE(decoded.has_value());
    if (decoded) ASSERT_TRUE(*decoded == state);
}

TEST(activeModeRoundTripsWithoutChangingInventories) {
    PaletteViewState first = sample();
    PaletteViewState second = sample();
    second.activeMode = SearchMode::File;
    const auto a = decodePalette(encodePalette(first));
    const auto b = decodePalette(encodePalette(second));
    ASSERT_TRUE(a.has_value() && b.has_value());
    if (a && b) {
        ASSERT_EQ(a->activeMode, std::optional<SearchMode>{SearchMode::Command});
        ASSERT_EQ(b->activeMode, std::optional<SearchMode>{SearchMode::File});
        ASSERT_EQ(a->commandCandidates, b->commandCandidates);
        ASSERT_EQ(a->fileCandidates, b->fileCandidates);
        ASSERT_FALSE(*a == *b);
    }
}

TEST(closedPickerStillCarriesBothInventories) {
    auto state = sample();
    state.activeMode.reset();
    const auto decoded = decodePalette(encodePalette(state));
    ASSERT_TRUE(decoded.has_value());
    if (decoded) {
        ASSERT_FALSE(decoded->activeMode.has_value());
        ASSERT_FALSE(decoded->commandCandidates.empty());
        ASSERT_FALSE(decoded->fileCandidates.empty());
    }
}

TEST(publishedParametersAreTheLibraryDefaults) {
    // A fresh PaletteViewState carries the library's matcher constants, and they
    // survive the wire -- the single source a client scores with.
    const auto decoded = decodePalette(encodePalette(PaletteViewState{}));
    ASSERT_TRUE(decoded.has_value());
    if (decoded) ASSERT_TRUE(decoded->parameters == MatcherParameters{});
}

TEST(decodeRejectsCandidateMissingAField) {
    ProtocolValue::Array candidates;
    candidates.push_back(ProtocolValue::makeObject(
        {{"id", ProtocolValue::makeText("x")},
         {"label", ProtocolValue::makeText("X")}}));  // no detail
    ASSERT_FALSE(
        decodePalette(
           paletteWire(ProtocolValue::makeArray(std::move(candidates))))
           .has_value());
}

TEST(decodeRejectsParametersMissingAField) {
    ProtocolValue parameters = ProtocolValue::makeObject(
        {{"base_score", ProtocolValue::makeInt(10)},
         {"word_boundary_bonus", ProtocolValue::makeInt(8)},
         {"contiguity_bonus", ProtocolValue::makeInt(6)},
         {"exact_case_bonus", ProtocolValue::makeInt(1)}});  // no length_cap
    ASSERT_FALSE(
        decodePalette(paletteWire(ProtocolValue::makeArray({}),
                                  ProtocolValue::makeArray({}),
                                  std::move(parameters)))
            .has_value());
}

TEST(decodeRejectsOutOfRangeMode) {
    ASSERT_FALSE(
        decodePalette(paletteWire(
                          ProtocolValue::makeArray({}),
                          ProtocolValue::makeArray({}),
                          *encodePalette(PaletteViewState{}).field("parameters"),
                          ProtocolValue::makeUint(99)))
            .has_value());
    ASSERT_FALSE(
        decodePalette(paletteWire(
                          ProtocolValue::makeArray({}),
                          ProtocolValue::makeArray({}),
                          *encodePalette(PaletteViewState{}).field("parameters"),
                          ProtocolValue::makeUint(
                              static_cast<std::uint8_t>(SearchMode::Text))))
            .has_value());
}

TEST(decodeRejectsNonArrayCandidates) {
    ASSERT_FALSE(
        decodePalette(paletteWire(ProtocolValue::makeText("not an array")))
            .has_value());
}

TEST(decodeRejectsOutOfDomainParameter) {
    // A weight beyond the published magnitude domain is refused, not silently
    // accepted -- large weights would overflow the C++ int score and diverge from
    // the JS double score.
    ProtocolValue parameters = ProtocolValue::makeObject(
        {{"base_score",
          ProtocolValue::makeInt(kMaxMatcherParameterMagnitude + 1)},
         {"word_boundary_bonus", ProtocolValue::makeInt(8)},
         {"contiguity_bonus", ProtocolValue::makeInt(6)},
         {"exact_case_bonus", ProtocolValue::makeInt(1)},
         {"length_cap", ProtocolValue::makeInt(100)}});
    ASSERT_FALSE(
        decodePalette(paletteWire(ProtocolValue::makeArray({}),
                                  ProtocolValue::makeArray({}),
                                  std::move(parameters)))
            .has_value());
}

TEST(decodeRejectsParameterOutsideIntRange) {
    // A value beyond 32-bit int is malformed, never silently narrowed.
    ProtocolValue parameters = ProtocolValue::makeObject(
        {{"base_score", ProtocolValue::makeInt(5'000'000'000LL)},
         {"word_boundary_bonus", ProtocolValue::makeInt(8)},
         {"contiguity_bonus", ProtocolValue::makeInt(6)},
         {"exact_case_bonus", ProtocolValue::makeInt(1)},
         {"length_cap", ProtocolValue::makeInt(100)}});
    ASSERT_FALSE(
        decodePalette(paletteWire(ProtocolValue::makeArray({}),
                                  ProtocolValue::makeArray({}),
                                  std::move(parameters)))
            .has_value());
}

TEST(decodeRejectsMissingMagnitude) {
    auto valid = PaletteViewState{};
    ProtocolValue value = ProtocolValue::makeObject(
        {{"active_mode", ProtocolValue::makeNull()},
         {"command_open_command_id", ProtocolValue::makeText("palette.open")},
         {"command_candidates", ProtocolValue::makeArray({})},
         {"file_open_command_id",
          ProtocolValue::makeText("file_finder.open")},
         {"file_candidates", ProtocolValue::makeArray({})},
         {"parameters", *encodePalette(valid).field("parameters")},
         {"max_candidate_bytes", ProtocolValue::makeInt(kMaxCandidateBytes)}});
    ASSERT_FALSE(decodePalette(value).has_value());
}

TEST(decodeRejectsMismatchedMagnitude) {
    // A frame stamped with a domain bound other than the library's own is refused --
    // the C++ authority scores with its compiled constant and must not admit a frame
    // claiming a different bound.
    ASSERT_FALSE(
        decodePalette(paletteWire(
                          ProtocolValue::makeArray({}),
                          ProtocolValue::makeArray({}),
                          *encodePalette(PaletteViewState{}).field("parameters"),
                          ProtocolValue::makeNull(),
                          ProtocolValue::makeInt(
                              kMaxMatcherParameterMagnitude - 1)))
            .has_value());
}

TEST(decodeRejectsOversizedCandidate) {
    // A candidate whose id/label exceeds the published byte bound is refused, never
    // truncated -- the full-byte subsequence contract requires the whole candidate.
    ProtocolValue::Array candidates;
    candidates.push_back(ProtocolValue::makeObject(
        {{"id", ProtocolValue::makeText(
                    std::string(static_cast<std::size_t>(kMaxCandidateBytes) + 1, 'a'))},
         {"label", ProtocolValue::makeText("ok")},
         {"detail", ProtocolValue::makeText("")}}));
    ASSERT_FALSE(
        decodePalette(
            paletteWire(ProtocolValue::makeArray(std::move(candidates))))
            .has_value());
}

}  // namespace

int main() {
    RUN(encodeDecodeRoundTripsExactly);
    RUN(activeModeRoundTripsWithoutChangingInventories);
    RUN(closedPickerStillCarriesBothInventories);
    RUN(publishedParametersAreTheLibraryDefaults);
    RUN(decodeRejectsCandidateMissingAField);
    RUN(decodeRejectsParametersMissingAField);
    RUN(decodeRejectsOutOfRangeMode);
    RUN(decodeRejectsNonArrayCandidates);
    RUN(decodeRejectsOutOfDomainParameter);
    RUN(decodeRejectsParameterOutsideIntRange);
    RUN(decodeRejectsMissingMagnitude);
    RUN(decodeRejectsMismatchedMagnitude);
    RUN(decodeRejectsOversizedCandidate);
    return failed;
}
