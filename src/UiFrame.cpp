#include <ssg/UiFrame.h>

#include <ssg/Style.h>
#include <ssg/UiTree.h>
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
        [&](const UiNodeId& id) { return effective.contains(id); });
    const auto active = effective.find(state.focusPath->back());
    return everyNodeExists && active != effective.end() && active->second;
}

template <typename Record>
bool hasDuplicateIds(const std::vector<Record>& records) {
    std::set<UiNodeId> ids;
    return std::any_of(records.begin(), records.end(),
                       [&](const Record& record) {
                           return !ids.insert(record.id).second;
                       });
}

template <typename Record>
void replaceRecords(std::vector<Record>& target,
                    const std::vector<Record>& changes) {
    for (const auto& change : changes) {
        const auto found = std::find_if(
            target.begin(), target.end(),
            [&](const Record& record) { return record.id == change.id; });
        *found = change;
    }
}

}  // namespace

UiFrame::UiFrame() {
    UiSchema schema{
        Generation{0},
        assembleWholeScreen({}, "help.open", StyleDimensions{},
                            Style{}.inputLineSigil, std::nullopt)
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

UiFrameDelta UiFrameDelta::replacement(UiFrameVersion base, UiFrame frame) {
    const UiFrameVersion target = frame.version();
    return UiFrameDelta{base, target,
                        UiFrameReplacement{std::move(frame)}};
}

UiFrameDelta UiFrameDelta::changes(UiFrameVersion base,
                                   UiFrameVersion target,
                                   UiFrameChanges changes) {
    return UiFrameDelta{base, target, std::move(changes)};
}

UiFrameDelta UiFrameDelta::legacyChanges(LegacyUiFrameChanges changes) {
    return UiFrameDelta{{}, {}, std::move(changes)};
}

UiFrameDelta UiFrameDeltaCodec::derive(const UiFrame& base,
                                       const UiFrame& target) const {
    if (base.schema() != target.schema()) {
        return UiFrameDelta::replacement(base.version(), target);
    }

    UiFrameChanges changes;
    for (const auto& next : target.state().nodes) {
        const auto before = std::find_if(
            base.state().nodes.begin(), base.state().nodes.end(),
            [&](const UiNodeState& record) { return record.id == next.id; });
        if (*before != next) changes.state.push_back(next);
    }
    for (const auto& next : target.presence().nodes) {
        const auto before = std::find_if(
            base.presence().nodes.begin(), base.presence().nodes.end(),
            [&](const UiPresenceRecord& record) { return record.id == next.id; });
        if (*before != next) changes.presence.push_back(next);
    }
    if (base.focusPath() != target.focusPath()) {
        changes.focusPathChanged = true;
        changes.focusPath = target.focusPath();
    }
    return UiFrameDelta::changes(base.version(), target.version(),
                                 std::move(changes));
}

UiFrameReplayResult UiFrameDeltaCodec::replay(
    const UiFrame& base, const UiFrameDelta& delta) const {
    if (const auto* legacy =
            std::get_if<LegacyUiFrameChanges>(&delta.body())) {
        auto state = legacy->state.value_or(base.state());
        if (!state.focusPath) state.focusPath = base.focusPath();
        auto frame = UiFrame::create(
            legacy->schema.value_or(base.schema()),
            std::move(state),
            legacy->presence.value_or(base.presence()));
        if (!frame) return {std::nullopt, UiFrameReplayError::InvalidFrame};
        return {std::move(frame), UiFrameReplayError::None};
    }
    if (base.version() != delta.base()) {
        return {std::nullopt, UiFrameReplayError::StaleVersion};
    }

    if (const auto* replacement =
            std::get_if<UiFrameReplacement>(&delta.body())) {
        if (replacement->frame.version() != delta.target() ||
            replacement->frame.schema() == base.schema() ||
            replacement->frame.version().generation ==
                base.version().generation) {
            return {std::nullopt, UiFrameReplayError::MalformedDelta};
        }
        return {replacement->frame, UiFrameReplayError::None};
    }

    const auto& changes = std::get<UiFrameChanges>(delta.body());
    if (delta.target().generation != base.version().generation ||
        delta.target().presenceBasis < base.version().presenceBasis ||
        (!changes.presence.empty() &&
         delta.target().presenceBasis <= base.version().presenceBasis) ||
        hasDuplicateIds(changes.state) ||
        hasDuplicateIds(changes.presence) ||
        (changes.focusPathChanged && !changes.focusPath) ||
        (!changes.focusPathChanged && changes.focusPath)) {
        return {std::nullopt, UiFrameReplayError::MalformedDelta};
    }

    UiStateSection state = base.state();
    UiPresenceSection presence = base.presence();
    const auto ids = uiSchemaNodeIds(base.schema());
    const auto knownState =
        std::all_of(changes.state.begin(), changes.state.end(),
                    [&](const UiNodeState& record) {
                        return ids.contains(record.id);
                    });
    const auto knownPresence =
        std::all_of(changes.presence.begin(), changes.presence.end(),
                    [&](const UiPresenceRecord& record) {
                        return ids.contains(record.id);
                    });
    if (!knownState || !knownPresence) {
        return {std::nullopt, UiFrameReplayError::MalformedDelta};
    }
    replaceRecords(state.nodes, changes.state);
    replaceRecords(presence.nodes, changes.presence);
    presence.basis = delta.target().presenceBasis;
    if (changes.focusPathChanged) state.focusPath = changes.focusPath;

    auto frame = UiFrame::create(base.schema(), std::move(state),
                                 std::move(presence));
    if (!frame) return {std::nullopt, UiFrameReplayError::InvalidFrame};
    return {std::move(frame), UiFrameReplayError::None};
}

}  // namespace ssg
