#include <ssg/PresenceProtocol.h>

#include <set>
#include <string>

namespace ssg {

namespace {

ProtocolValue encodeRecord(const UiPresenceRecord& record) {
    return ProtocolValue::makeObject(
        {{"id", ProtocolValue::makeText(record.id.value())},
         {"present", ProtocolValue::makeBool(record.present)}});
}

std::optional<UiPresenceRecord> decodeRecord(const ProtocolValue& value) {
    if (!value.asObject()) return std::nullopt;
    const ProtocolValue* id = value.field("id");
    if (!id || !id->asText() || id->asText()->empty()) return std::nullopt;
    const ProtocolValue* present = value.field("present");
    if (!present || !present->asBool()) return std::nullopt;
    return UiPresenceRecord{UiNodeId{*id->asText()}, *present->asBool()};
}

}  // namespace

ProtocolValue encodeUiPresence(const UiPresenceSection& section) {
    ProtocolValue::Array nodes;
    nodes.reserve(section.nodes.size());
    for (const auto& record : section.nodes) nodes.push_back(encodeRecord(record));
    return ProtocolValue::makeObject(
        {{"generation", ProtocolValue::makeUint(section.generation.value())},
         {"basis", ProtocolValue::makeUint(section.basis.value())},
         {"nodes", ProtocolValue::makeArray(std::move(nodes))}});
}

std::optional<UiPresenceSection> decodeUiPresence(const ProtocolValue& value) {
    if (!value.asObject()) return std::nullopt;
    const ProtocolValue* generation = value.field("generation");
    const ProtocolValue* basis = value.field("basis");
    const ProtocolValue* nodesField = value.field("nodes");
    if (!generation || !generation->asUint()) return std::nullopt;
    if (!basis || !basis->asUint()) return std::nullopt;
    if (!nodesField || !nodesField->asArray()) return std::nullopt;

    UiPresenceSection section;
    section.generation = Generation{*generation->asUint()};
    section.basis = PresenceBasis{*basis->asUint()};
    std::set<std::string> seen;
    for (const auto& nodeValue : *nodesField->asArray()) {
        auto record = decodeRecord(nodeValue);
        if (!record) return std::nullopt;
        if (!seen.insert(record->id.value()).second) {
            return std::nullopt;  // a duplicate node id is malformed
        }
        section.nodes.push_back(std::move(*record));
    }
    return section;
}

bool uiPresenceCorrespondsToSchema(const UiPresenceSection& section,
                                   const ValidatedSchema& schema) {
    if (section.generation != schema.generation()) return false;
    const std::set<UiNodeId>& schemaIds = schema.nodeIds();
    if (section.nodes.size() != schemaIds.size()) return false;
    std::set<UiNodeId> recordIds;
    for (const auto& record : section.nodes) recordIds.insert(record.id);
    return recordIds == schemaIds;
}

}  // namespace ssg
