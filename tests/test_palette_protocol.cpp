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
    state.mode = SearchMode::Command;
    state.pickerEpoch = 7;
    state.candidates = {
        {"edit.undo", "Undo", "Ctrl+Z"},
        {"file.save", "Save File", ""},
        {"vue.open", "Open \xC3\xA9\x63lair", "non-ascii detail"},
    };
    return state;
}

TEST(encodeDecodeRoundTripsExactly) {
    const auto state = sample();
    const auto decoded = decodePalette(encodePalette(state));
    ASSERT_TRUE(decoded.has_value());
    if (decoded) ASSERT_TRUE(*decoded == state);
}

TEST(pickerEpochRoundTripsAndDistinguishesReopens) {
    // The reopen identity survives the wire so a client can reset its local query
    // when a same-kind picker reopens (the epoch advances while presence never
    // toggles). Two states differing ONLY in epoch decode as distinct.
    PaletteViewState first = sample();
    first.pickerEpoch = 41;
    PaletteViewState second = sample();
    second.pickerEpoch = 42;
    const auto a = decodePalette(encodePalette(first));
    const auto b = decodePalette(encodePalette(second));
    ASSERT_TRUE(a.has_value() && b.has_value());
    if (a && b) {
        ASSERT_EQ(a->pickerEpoch, 41u);
        ASSERT_EQ(b->pickerEpoch, 42u);
        ASSERT_FALSE(*a == *b);
    }
}

TEST(decodeMissingPickerEpochAsZero) {
    // picker_epoch is additive for version-1 compatibility: old frames without it
    // decode as the initial epoch while new frames still round-trip a real reopen
    // identity.
    ProtocolValue value = ProtocolValue::makeObject(
        {{"mode", ProtocolValue::makeUint(4)},
         {"candidates", ProtocolValue::makeArray({})},
         {"parameters", *encodePalette(PaletteViewState{}).field("parameters")},
         {"max_parameter_magnitude",
          ProtocolValue::makeInt(kMaxMatcherParameterMagnitude)},
         {"max_candidate_bytes", ProtocolValue::makeInt(kMaxCandidateBytes)}});
    const auto decoded = decodePalette(value);
    ASSERT_TRUE(decoded.has_value());
    if (decoded) ASSERT_EQ(decoded->pickerEpoch, std::uint64_t{0});
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
    ProtocolValue value = ProtocolValue::makeObject(
        {{"mode", ProtocolValue::makeUint(4)},
         {"candidates", ProtocolValue::makeArray(std::move(candidates))},
         {"parameters", encodePalette(PaletteViewState{}).field("parameters")
                            ? *encodePalette(PaletteViewState{}).field("parameters")
                            : ProtocolValue::makeNull()},
         {"max_parameter_magnitude", ProtocolValue::makeInt(kMaxMatcherParameterMagnitude)},
         {"max_candidate_bytes", ProtocolValue::makeInt(kMaxCandidateBytes)}});
    ASSERT_FALSE(decodePalette(value).has_value());
}

TEST(decodeRejectsParametersMissingAField) {
    ProtocolValue parameters = ProtocolValue::makeObject(
        {{"base_score", ProtocolValue::makeInt(10)},
         {"word_boundary_bonus", ProtocolValue::makeInt(8)},
         {"contiguity_bonus", ProtocolValue::makeInt(6)},
         {"exact_case_bonus", ProtocolValue::makeInt(1)}});  // no length_cap
    ProtocolValue value = ProtocolValue::makeObject(
        {{"mode", ProtocolValue::makeUint(4)},
         {"candidates", ProtocolValue::makeArray({})},
         {"parameters", parameters},
         {"max_parameter_magnitude", ProtocolValue::makeInt(kMaxMatcherParameterMagnitude)},
         {"max_candidate_bytes", ProtocolValue::makeInt(kMaxCandidateBytes)}});
    ASSERT_FALSE(decodePalette(value).has_value());
}

TEST(decodeRejectsOutOfRangeMode) {
    ProtocolValue value = ProtocolValue::makeObject(
        {{"mode", ProtocolValue::makeUint(99)},
         {"candidates", ProtocolValue::makeArray({})},
         {"parameters", *encodePalette(PaletteViewState{}).field("parameters")},
         {"max_parameter_magnitude", ProtocolValue::makeInt(kMaxMatcherParameterMagnitude)},
         {"max_candidate_bytes", ProtocolValue::makeInt(kMaxCandidateBytes)}});
    ASSERT_FALSE(decodePalette(value).has_value());
}

TEST(decodeRejectsNonArrayCandidates) {
    ProtocolValue value = ProtocolValue::makeObject(
        {{"mode", ProtocolValue::makeUint(4)},
         {"candidates", ProtocolValue::makeText("not an array")},
         {"parameters", *encodePalette(PaletteViewState{}).field("parameters")},
         {"max_parameter_magnitude", ProtocolValue::makeInt(kMaxMatcherParameterMagnitude)},
         {"max_candidate_bytes", ProtocolValue::makeInt(kMaxCandidateBytes)}});
    ASSERT_FALSE(decodePalette(value).has_value());
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
    ProtocolValue value = ProtocolValue::makeObject(
        {{"mode", ProtocolValue::makeUint(4)},
         {"candidates", ProtocolValue::makeArray({})},
         {"parameters", parameters},
         {"max_parameter_magnitude", ProtocolValue::makeInt(kMaxMatcherParameterMagnitude)},
         {"max_candidate_bytes", ProtocolValue::makeInt(kMaxCandidateBytes)}});
    ASSERT_FALSE(decodePalette(value).has_value());
}

TEST(decodeRejectsParameterOutsideIntRange) {
    // A value beyond 32-bit int is malformed, never silently narrowed.
    ProtocolValue parameters = ProtocolValue::makeObject(
        {{"base_score", ProtocolValue::makeInt(5'000'000'000LL)},
         {"word_boundary_bonus", ProtocolValue::makeInt(8)},
         {"contiguity_bonus", ProtocolValue::makeInt(6)},
         {"exact_case_bonus", ProtocolValue::makeInt(1)},
         {"length_cap", ProtocolValue::makeInt(100)}});
    ProtocolValue value = ProtocolValue::makeObject(
        {{"mode", ProtocolValue::makeUint(4)},
         {"candidates", ProtocolValue::makeArray({})},
         {"parameters", parameters},
         {"max_parameter_magnitude", ProtocolValue::makeInt(kMaxMatcherParameterMagnitude)},
         {"max_candidate_bytes", ProtocolValue::makeInt(kMaxCandidateBytes)}});
    ASSERT_FALSE(decodePalette(value).has_value());
}

TEST(decodeRejectsMissingMagnitude) {
    // The published domain bound must be present; a frame without it is malformed.
    ProtocolValue value = ProtocolValue::makeObject(
        {{"mode", ProtocolValue::makeUint(4)},
         {"candidates", ProtocolValue::makeArray({})},
         {"parameters", *encodePalette(PaletteViewState{}).field("parameters")}});
    ASSERT_FALSE(decodePalette(value).has_value());
}

TEST(decodeRejectsMismatchedMagnitude) {
    // A frame stamped with a domain bound other than the library's own is refused --
    // the C++ authority scores with its compiled constant and must not admit a frame
    // claiming a different bound.
    ProtocolValue value = ProtocolValue::makeObject(
        {{"mode", ProtocolValue::makeUint(4)},
         {"candidates", ProtocolValue::makeArray({})},
         {"parameters", *encodePalette(PaletteViewState{}).field("parameters")},
         {"max_parameter_magnitude",
          ProtocolValue::makeInt(kMaxMatcherParameterMagnitude - 1)},
         {"max_candidate_bytes", ProtocolValue::makeInt(kMaxCandidateBytes)}});
    ASSERT_FALSE(decodePalette(value).has_value());
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
    ProtocolValue value = ProtocolValue::makeObject(
        {{"mode", ProtocolValue::makeUint(4)},
         {"candidates", ProtocolValue::makeArray(std::move(candidates))},
         {"parameters", *encodePalette(PaletteViewState{}).field("parameters")},
         {"max_parameter_magnitude",
          ProtocolValue::makeInt(kMaxMatcherParameterMagnitude)},
         {"max_candidate_bytes", ProtocolValue::makeInt(kMaxCandidateBytes)}});
    ASSERT_FALSE(decodePalette(value).has_value());
}

}  // namespace

int main() {
    RUN(encodeDecodeRoundTripsExactly);
    RUN(pickerEpochRoundTripsAndDistinguishesReopens);
    RUN(decodeMissingPickerEpochAsZero);
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
