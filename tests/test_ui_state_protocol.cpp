// Round-trip + rejection oracle (contract) for the dynamic-node-state wire codec.
// The knowable answers: encode→decode recovers a section exactly, and a value the
// codec OWNS as malformed (no generation, a duplicate node id, a wrong leaf field
// type) decodes to nullopt, never a partial.

#include "ssg/UiStateProtocol.h"

#include "ssg/UiNodeState.h"
#include "test_helpers.h"

#include <optional>
#include <string>
#include <vector>

namespace {

using namespace ssg;

UiStateSection sample() {
    UiStateSection section;
    section.generation = Generation{9};
    section.nodes.push_back(UiNodeState{UiNodeId{"root"}, true, std::nullopt});
    section.nodes.push_back(UiNodeState{
        UiNodeId{"f"}, true,
        UiLeafState{"~/proj", "Current path",
                    std::optional<std::string>{"panel.show_files"},
                    std::nullopt}});
    section.nodes.push_back(UiNodeState{
        UiNodeId{"c"}, true,
        UiLeafState{"case", "case", std::nullopt, std::optional<bool>{true}}});
    section.nodes.push_back(UiNodeState{UiNodeId{"sp"}, true, std::nullopt});
    return section;
}

TEST(encodeDecodeRoundTripsExactly) {
    const auto section = sample();
    const auto decoded = decodeUiState(encodeUiState(section));
    ASSERT_TRUE(decoded.has_value());
    if (decoded) ASSERT_TRUE(*decoded == section);
}

// An empty section (generation only, no nodes) round-trips.
TEST(emptySectionRoundTrips) {
    UiStateSection empty;
    empty.generation = Generation{3};
    const auto decoded = decodeUiState(encodeUiState(empty));
    ASSERT_TRUE(decoded.has_value());
    if (decoded) ASSERT_TRUE(*decoded == empty);
}

// A section with no generation is malformed.
TEST(missingGenerationDecodesToNullopt) {
    const ProtocolValue value =
        ProtocolValue::makeObject({{"nodes", ProtocolValue::makeArray({})}});
    ASSERT_FALSE(decodeUiState(value).has_value());
}

// A duplicate node id is malformed (the section must be one record per node).
TEST(duplicateNodeIdDecodesToNullopt) {
    const auto node = [] {
        return ProtocolValue::makeObject(
            {{"id", ProtocolValue::makeText("dup")},
             {"present", ProtocolValue::makeBool(true)},
             {"leaf", ProtocolValue::makeNull()}});
    };
    const ProtocolValue value = ProtocolValue::makeObject(
        {{"generation", ProtocolValue::makeUint(1)},
         {"nodes", ProtocolValue::makeArray({node(), node()})}});
    ASSERT_FALSE(decodeUiState(value).has_value());
}

// A leaf whose `checked` is not a boolean, or `command` not a string, is malformed.
TEST(malformedLeafFieldTypeDecodesToNullopt) {
    const auto sectionWithLeaf = [](ProtocolValue leaf) {
        return ProtocolValue::makeObject(
            {{"generation", ProtocolValue::makeUint(1)},
             {"nodes", ProtocolValue::makeArray({ProtocolValue::makeObject(
                 {{"id", ProtocolValue::makeText("n")},
                  {"present", ProtocolValue::makeBool(true)},
                  {"leaf", std::move(leaf)}})})}});
    };
    // checked is a string, not a bool.
    ASSERT_FALSE(decodeUiState(sectionWithLeaf(ProtocolValue::makeObject(
                                   {{"value", ProtocolValue::makeText("v")},
                                    {"label", ProtocolValue::makeText("l")},
                                    {"command", ProtocolValue::makeNull()},
                                    {"checked", ProtocolValue::makeText("yes")}})))
                     .has_value());
    // command is a bool, not a string.
    ASSERT_FALSE(decodeUiState(sectionWithLeaf(ProtocolValue::makeObject(
                                   {{"value", ProtocolValue::makeText("v")},
                                    {"label", ProtocolValue::makeText("l")},
                                    {"command", ProtocolValue::makeBool(true)},
                                    {"checked", ProtocolValue::makeNull()}})))
                     .has_value());
    // value is missing.
    ASSERT_FALSE(decodeUiState(sectionWithLeaf(ProtocolValue::makeObject(
                                   {{"label", ProtocolValue::makeText("l")}})))
                     .has_value());
}

}  // namespace

int main() {
    RUN(encodeDecodeRoundTripsExactly);
    RUN(emptySectionRoundTrips);
    RUN(missingGenerationDecodesToNullopt);
    RUN(duplicateNodeIdDecodesToNullopt);
    RUN(malformedLeafFieldTypeDecodesToNullopt);
    return 0;
}
