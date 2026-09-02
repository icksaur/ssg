#include <ssg/UiFrame.h>

#include <ssg/Style.h>
#include <ssg/UiTree.h>
#include <ssg/UiStateResolver.h>
#include <ssg/WholeScreenAssembly.h>

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace ssg {
namespace {

template <typename Record>
bool recordsMatchSchema(const UiSchema& schema,
                        const std::vector<Record>& records) {
    const auto ids = uiSchemaNodeIds(schema);
    if (ids.size() != records.size()) return false;
    std::set<UiNodeId> seen;
    return std::all_of(records.begin(), records.end(), [&](const Record& record) {
        return ids.contains(record.id) && seen.insert(record.id).second;
    });
}

bool effectivePresence(const UiNode& node, bool ancestorsPresent,
                       const std::map<UiNodeId, bool>& direct,
                       std::map<UiNodeId, bool>& effective) {
    const auto found = direct.find(node.id);
    if (found == direct.end()) return false;
    const bool present = ancestorsPresent && found->second;
    effective.emplace(node.id, present);
    if (const auto* container = std::get_if<UiContainer>(&node.content)) {
        for (const auto& child : container->children) {
            if (!effectivePresence(child, present, direct, effective)) return false;
        }
    }
    return true;
}

bool validFocusPath(const UiSchema& schema, const UiStateSection& state,
                    const UiPresenceSection& presence) {
    if (!state.focusPath || state.focusPath->empty()) return false;
    std::map<UiNodeId, bool> direct;
    for (const auto& record : presence.nodes) {
        if (!direct.emplace(record.id, record.present).second) return false;
    }
    std::map<UiNodeId, bool> effective;
    if (!effectivePresence(schema.root, true, direct, effective)) return false;
    const bool everyNodeExists = std::all_of(
        state.focusPath->begin(), state.focusPath->end(),
        [&](const UiNodeId& id) {
            const UiNode* node = findUiNode(schema, id);
            return effective.contains(id) && node && node->focusContext;
        });
    const auto active = effective.find(state.focusPath->back());
    const UiNode* base = findUiNode(schema, state.focusPath->front());
    const bool validBase =
        base && (base->focusContext == FocusTarget::Editor ||
                 base->focusContext == FocusTarget::Panel);
    return everyNodeExists && validBase && active != effective.end() &&
           active->second;
}

}  // namespace

UiFrame::UiFrame() {
    UiSchema schema{
        Generation{0},
        assembleWholeScreen({}, "help.open", StyleDimensions{},
                            Style{}.inputLineSigil)
            .root};
    const auto validated = ValidatedSchema::validate(schema);
    UiStateSection state;
    state.generation = schema.generation;
    for (const auto& id : validated.schema().nodeIds()) {
        state.nodes.push_back(UiNodeState{id, std::nullopt});
    }

    state.focusPath =
        std::vector<UiNodeId>{UiNodeId{std::string{kEditorNodeId}}};
    UiPresenceSection presence = buildPresenceSection(
        validated.schema(), PresenceConfig::allPresent(validated.schema()));
    auto frame = create(std::move(schema), std::move(state),
                        std::move(presence));
    if (!frame) throw std::logic_error("default UI frame is invalid");
    *this = std::move(*frame);
}

FocusTarget UiFrame::effectiveFocus() const noexcept {
    return *findUiNode(schema_, focusPath().back())->focusContext;
}

std::optional<UiFrame> UiFrame::create(UiSchema schema, UiStateSection state,
                                       UiPresenceSection presence) {
    const auto validated = ValidatedSchema::validate(schema);
    if (!validated.ok() || !validateWellKnownAreas(schema).ok()) {
        return std::nullopt;
    }
    if (state.generation != schema.generation ||
        presence.generation != schema.generation) {
        return std::nullopt;
    }
    if (!recordsMatchSchema(schema, state.nodes) ||
        !recordsMatchSchema(schema, presence.nodes)) {
        return std::nullopt;
    }
    if (!validFocusPath(schema, state, presence)) return std::nullopt;
    return UiFrame{std::move(schema), std::move(state), std::move(presence),
                   ValidatedTag{}};
}

UiFrame UiFrame::require(UiSchema schema, UiStateSection state,
                         UiPresenceSection presence) {
    auto frame =
        create(std::move(schema), std::move(state), std::move(presence));
    if (!frame) throw std::invalid_argument("invalid UI frame");
    return std::move(*frame);
}

}  // namespace ssg
