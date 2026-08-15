// Round-trip + rejection + correspondence oracle (contract) for the presence-section
// wire codec. The knowable answers: encode->decode recovers a section exactly; a
// value the codec OWNS as malformed (no generation, no basis, a record without an id
// or a boolean present, a duplicate node id) decodes to nullopt, never a partial; and
// a section corresponds to a schema iff its generation matches and its record id-set
// is exactly the schema's node-id set.

#include "ssg/PresenceProtocol.h"

#include "ssg/UiPresence.h"
#include "ssg/UiTree.h"
#include "test_helpers.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace ssg;

UiNode container(std::string id, std::vector<UiNode> kids = {}) {
    UiNode node;
    node.id = UiNodeId{std::move(id)};
    UiContainer body;
    body.children = std::move(kids);
    node.content = body;
    return node;
}

// root > [ a, b > [ c ] ] -- four uniquely-named container nodes.
ValidatedSchema sampleSchema(Generation generation = Generation{7}) {
    UiNode root = container(
        "root", {container("a"), container("b", {container("c")})});
    auto result = ValidatedSchema::validate(UiSchema{generation, std::move(root)});
    ASSERT_TRUE(result.ok());
    return result.takeSchema();
}

TEST(encodeDecodeRoundTripsExactly) {
    const auto schema = sampleSchema();
    const auto section =
        buildPresenceSection(schema, PresenceConfig::allPresent(schema));
    const auto decoded = decodeUiPresence(encodeUiPresence(section));
    ASSERT_TRUE(decoded.has_value());
    if (decoded) ASSERT_TRUE(*decoded == section);
}

TEST(builtSectionCorrespondsToItsSchema) {
    const auto schema = sampleSchema();
    const auto section =
        buildPresenceSection(schema, PresenceConfig::allPresent(schema));
    ASSERT_TRUE(uiPresenceCorrespondsToSchema(section, schema));
}

TEST(correspondenceFailsOnGenerationMismatch) {
    const auto schema = sampleSchema(Generation{7});
    auto section = buildPresenceSection(schema, PresenceConfig::allPresent(schema));
    section.generation = Generation{8};
    ASSERT_FALSE(uiPresenceCorrespondsToSchema(section, schema));
}

TEST(correspondenceFailsWhenIdSetDrifts) {
    const auto schema = sampleSchema();
    auto section = buildPresenceSection(schema, PresenceConfig::allPresent(schema));
    ASSERT_TRUE(!section.nodes.empty());
    section.nodes.pop_back();  // a missing node id breaks correspondence
    ASSERT_FALSE(uiPresenceCorrespondsToSchema(section, schema));

    auto foreign = buildPresenceSection(schema, PresenceConfig::allPresent(schema));
    foreign.nodes.back().id = UiNodeId{"not-in-schema"};  // a foreign id, same count
    ASSERT_FALSE(uiPresenceCorrespondsToSchema(foreign, schema));
}

TEST(decodeRejectsMissingGeneration) {
    ProtocolValue value = ProtocolValue::makeObject(
        {{"basis", ProtocolValue::makeUint(0)},
         {"nodes", ProtocolValue::makeArray({})}});
    ASSERT_FALSE(decodeUiPresence(value).has_value());
}

TEST(decodeRejectsMissingBasis) {
    ProtocolValue value = ProtocolValue::makeObject(
        {{"generation", ProtocolValue::makeUint(1)},
         {"nodes", ProtocolValue::makeArray({})}});
    ASSERT_FALSE(decodeUiPresence(value).has_value());
}

TEST(decodeRejectsRecordWithoutId) {
    ProtocolValue::Array nodes;
    nodes.push_back(
        ProtocolValue::makeObject({{"present", ProtocolValue::makeBool(true)}}));
    ProtocolValue value = ProtocolValue::makeObject(
        {{"generation", ProtocolValue::makeUint(1)},
         {"basis", ProtocolValue::makeUint(0)},
         {"nodes", ProtocolValue::makeArray(std::move(nodes))}});
    ASSERT_FALSE(decodeUiPresence(value).has_value());
}

TEST(decodeRejectsRecordWithNonBoolPresent) {
    ProtocolValue::Array nodes;
    nodes.push_back(ProtocolValue::makeObject(
        {{"id", ProtocolValue::makeText("root")},
         {"present", ProtocolValue::makeUint(1)}}));
    ProtocolValue value = ProtocolValue::makeObject(
        {{"generation", ProtocolValue::makeUint(1)},
         {"basis", ProtocolValue::makeUint(0)},
         {"nodes", ProtocolValue::makeArray(std::move(nodes))}});
    ASSERT_FALSE(decodeUiPresence(value).has_value());
}

TEST(decodeRejectsDuplicateNodeId) {
    ProtocolValue::Array nodes;
    for (int i = 0; i < 2; ++i) {
        nodes.push_back(ProtocolValue::makeObject(
            {{"id", ProtocolValue::makeText("dup")},
             {"present", ProtocolValue::makeBool(true)}}));
    }
    ProtocolValue value = ProtocolValue::makeObject(
        {{"generation", ProtocolValue::makeUint(1)},
         {"basis", ProtocolValue::makeUint(0)},
         {"nodes", ProtocolValue::makeArray(std::move(nodes))}});
    ASSERT_FALSE(decodeUiPresence(value).has_value());
}

}  // namespace

int main() {
    RUN(encodeDecodeRoundTripsExactly);
    RUN(builtSectionCorrespondsToItsSchema);
    RUN(correspondenceFailsOnGenerationMismatch);
    RUN(correspondenceFailsWhenIdSetDrifts);
    RUN(decodeRejectsMissingGeneration);
    RUN(decodeRejectsMissingBasis);
    RUN(decodeRejectsRecordWithoutId);
    RUN(decodeRejectsRecordWithNonBoolPresent);
    RUN(decodeRejectsDuplicateNodeId);
    return failed;
}
