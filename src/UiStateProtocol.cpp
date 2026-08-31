#include <ssg/UiStateProtocol.h>
#include <ssg/detail/generated/ui_wire_schema.h>

#include <cassert>
#include <set>
#include <string>
#include <utility>

namespace ssg {

namespace {

bool isNull(const ProtocolValue* field) {
    return field && field->kind() == ProtocolValue::Kind::NullValue;
}

std::optional<std::string> textField(const ProtocolValue& object,
                                     std::string_view key) {
    const ProtocolValue* field = object.field(key);
    if (!field || !field->asText()) return std::nullopt;
    return *field->asText();
}

ProtocolValue encodeLeaf(const UiLeafState& leaf) {
    return ProtocolValue::makeObject(
        {{"value", ProtocolValue::makeText(leaf.value)},
         {"label", ProtocolValue::makeText(leaf.label)},
         {"command", leaf.command ? ProtocolValue::makeText(*leaf.command)
                                  : ProtocolValue::makeNull()},
         {"checked", leaf.checked ? ProtocolValue::makeBool(*leaf.checked)
                                  : ProtocolValue::makeNull()},
         {"role", ProtocolValue::makeUint(
                     static_cast<std::uint64_t>(leaf.role))},
         {"active", leaf.active ? ProtocolValue::makeBool(*leaf.active)
                                : ProtocolValue::makeNull()}});
}

ProtocolValue encodeNode(const UiNodeState& node) {
    return ProtocolValue::makeObject(
        {{"id", ProtocolValue::makeText(node.id.value())},
         {"leaf",
          node.leaf ? encodeLeaf(*node.leaf) : ProtocolValue::makeNull()}});
}

ProtocolValue encodeFocusPath(
    const std::optional<std::vector<UiNodeId>>& focusPath) {
    if (!focusPath) return ProtocolValue::makeNull();
    assert(!focusPath->empty());
    ProtocolValue::Array path;
    path.reserve(focusPath->size());
    for (const UiNodeId& id : *focusPath) {
        path.push_back(ProtocolValue::makeText(id.value()));
    }
    return ProtocolValue::makeArray(std::move(path));
}

// Decode a leaf record; nullopt on a malformed field TYPE (value/label not a
// string, a present-but-non-null command that is not a string, or a checked that
// is not a boolean).
std::optional<UiLeafState> decodeLeaf(const ProtocolValue& value) {
    if (!value.asObject()) return std::nullopt;
    const auto text = textField(value, "value");
    const auto label = textField(value, "label");
    if (!text || !label) return std::nullopt;
    UiLeafState leaf;
    leaf.value = *text;
    leaf.label = *label;
    if (const ProtocolValue* command = value.field("command");
        command && !isNull(command)) {
        const auto* asText = command->asText();
        if (!asText) return std::nullopt;
        leaf.command = *asText;
    }
    if (const ProtocolValue* checked = value.field("checked");
        checked && !isNull(checked)) {
        const auto asBool = checked->asBool();
        if (!asBool) return std::nullopt;
        leaf.checked = *asBool;
    }
    if (const ProtocolValue* active = value.field("active");
        active && !isNull(active)) {
        const auto asBool = active->asBool();
        if (!asBool) return std::nullopt;
        leaf.active = *asBool;
    }
    const ProtocolValue* role = value.field("role");
    leaf.role = static_cast<SemanticRole>(*role->asUint());
    return leaf;
}

std::optional<UiNodeState> decodeNode(const ProtocolValue& value) {
    if (!value.asObject()) return std::nullopt;
    const auto id = textField(value, "id");
    if (!id || id->empty()) return std::nullopt;
    UiNodeState node;
    node.id = UiNodeId{*id};
    if (const ProtocolValue* leaf = value.field("leaf"); leaf && !isNull(leaf)) {
        auto decoded = decodeLeaf(*leaf);
        if (!decoded) return std::nullopt;
        node.leaf = std::move(*decoded);
    }
    return node;
}

}  // namespace

ProtocolValue encodeUiState(const UiStateSection& section) {
    ProtocolValue::Array nodes;
    nodes.reserve(section.nodes.size());
    for (const auto& node : section.nodes) nodes.push_back(encodeNode(node));
    return ProtocolValue::makeObject(
        {{"generation",
          ProtocolValue::makeUint(section.generation.value())},
         {"nodes", ProtocolValue::makeArray(std::move(nodes))},
         {"focus_path", encodeFocusPath(section.focusPath)}});
}

std::optional<UiStateSection> decodeUiState(const ProtocolValue& value) {
    if (!detail::generated::validateUiStateSectionWire(value)) {
        return std::nullopt;
    }
    const ProtocolValue* generation = value.field("generation");
    if (!generation || !generation->asUint()) return std::nullopt;
    const ProtocolValue* nodesField = value.field("nodes");
    if (!nodesField || !nodesField->asArray()) return std::nullopt;

    UiStateSection section;
    section.generation = Generation{*generation->asUint()};
    std::set<std::string> seen;
    for (const auto& nodeValue : *nodesField->asArray()) {
        auto node = decodeNode(nodeValue);
        if (!node) return std::nullopt;
        if (!seen.insert(node->id.value()).second) {
            return std::nullopt;  // a duplicate node id is malformed
        }
        section.nodes.push_back(std::move(*node));
    }
    if (const ProtocolValue* focusPath = value.field("focus_path");
        focusPath && !isNull(focusPath)) {
        const auto* path = focusPath->asArray();
        if (!path || path->empty()) return std::nullopt;
        std::vector<UiNodeId> decodedPath;
        decodedPath.reserve(path->size());
        for (const ProtocolValue& idValue : *path) {
            const auto* id = idValue.asText();
            if (!id || id->empty()) return std::nullopt;
            decodedPath.emplace_back(*id);
        }
        section.focusPath = std::move(decodedPath);
    }
    return section;
}

}  // namespace ssg
