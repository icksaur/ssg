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
                            : ProtocolValue::makeNull()}});
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
         {"parameters", parameters}});
    ASSERT_FALSE(decodePalette(value).has_value());
}

TEST(decodeRejectsOutOfRangeMode) {
    ProtocolValue value = ProtocolValue::makeObject(
        {{"mode", ProtocolValue::makeUint(99)},
         {"candidates", ProtocolValue::makeArray({})},
         {"parameters", *encodePalette(PaletteViewState{}).field("parameters")}});
    ASSERT_FALSE(decodePalette(value).has_value());
}

TEST(decodeRejectsNonArrayCandidates) {
    ProtocolValue value = ProtocolValue::makeObject(
        {{"mode", ProtocolValue::makeUint(4)},
         {"candidates", ProtocolValue::makeText("not an array")},
         {"parameters", *encodePalette(PaletteViewState{}).field("parameters")}});
    ASSERT_FALSE(decodePalette(value).has_value());
}

}  // namespace

int main() {
    RUN(encodeDecodeRoundTripsExactly);
    RUN(publishedParametersAreTheLibraryDefaults);
    RUN(decodeRejectsCandidateMissingAField);
    RUN(decodeRejectsParametersMissingAField);
    RUN(decodeRejectsOutOfRangeMode);
    RUN(decodeRejectsNonArrayCandidates);
    return failed;
}
