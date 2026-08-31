#include "ssg/UiFrame.h"

#include "ssg/Style.h"
#include "ssg/WholeScreenAssembly.h"
#include "test_helpers.h"

#include <algorithm>
#include <string>
#include <utility>

namespace {

using namespace ssg;

struct Parts {
    UiSchema schema;
    UiStateSection state;
    UiPresenceSection presence;
};

Parts parts() {
    UiSchema schema{
        Generation{3},
        assembleWholeScreen({}, "help.open", StyleDimensions{},
                            Style{}.inputLineSigil, std::nullopt)
            .root};
    const auto validated = ValidatedSchema::validate(schema);
    ASSERT_TRUE(validated.ok());
    UiStateSection state;
    state.generation = schema.generation;
    for (const auto& id : validated.schema().nodeIds()) {
        state.nodes.push_back(UiNodeState{id, std::nullopt});
    }
    state.focusPath =
        std::vector<UiNodeId>{UiNodeId{std::string{kEditorNodeId}}};
    return {schema, state,
            buildPresenceSection(
                validated.schema(),
                PresenceConfig::allPresent(validated.schema()))};
}

UiFrame frame() {
    auto value = parts();
    return UiFrame::require(std::move(value.schema), std::move(value.state),
                            std::move(value.presence));
}

TEST(frameRejectsMismatchedRecordsGenerationsAndHiddenFocus) {
    auto valid = parts();
    ASSERT_TRUE(UiFrame::create(valid.schema, valid.state, valid.presence)
                    .has_value());

    auto wrongGeneration = valid;
    wrongGeneration.state.generation = Generation{4};
    ASSERT_FALSE(UiFrame::create(wrongGeneration.schema, wrongGeneration.state,
                                 wrongGeneration.presence)
                     .has_value());

    auto missingState = valid;
    missingState.state.nodes.pop_back();
    ASSERT_FALSE(UiFrame::create(missingState.schema, missingState.state,
                                 missingState.presence)
                     .has_value());

    auto duplicatePresence = valid;
    duplicatePresence.presence.nodes.back() =
        duplicatePresence.presence.nodes.front();
    ASSERT_FALSE(UiFrame::create(duplicatePresence.schema,
                                 duplicatePresence.state,
                                 duplicatePresence.presence)
                     .has_value());

    auto hiddenFocus = valid;
    const auto editor = std::find_if(
        hiddenFocus.presence.nodes.begin(), hiddenFocus.presence.nodes.end(),
        [](const UiPresenceRecord& record) {
            return record.id.value() == kEditorNodeId;
        });
    editor->present = false;
    ASSERT_FALSE(UiFrame::create(hiddenFocus.schema, hiddenFocus.state,
                                 hiddenFocus.presence)
                     .has_value());

    auto hiddenAncestor = valid;
    const auto content = std::find_if(
        hiddenAncestor.presence.nodes.begin(),
        hiddenAncestor.presence.nodes.end(),
        [](const UiPresenceRecord& record) {
            return record.id.value() == kContentNodeId;
        });
    content->present = false;
    ASSERT_FALSE(UiFrame::create(hiddenAncestor.schema, hiddenAncestor.state,
                                 hiddenAncestor.presence)
                     .has_value());

    auto hiddenRetainedBase = valid;
    hiddenRetainedBase.state.focusPath->push_back(
        UiNodeId{std::string{kHeaderPromptInputNodeId}});
    const auto baseEditor = std::find_if(
        hiddenRetainedBase.presence.nodes.begin(),
        hiddenRetainedBase.presence.nodes.end(),
        [](const UiPresenceRecord& record) {
            return record.id.value() == kEditorNodeId;
        });
    baseEditor->present = false;
    ASSERT_TRUE(UiFrame::create(hiddenRetainedBase.schema,
                                hiddenRetainedBase.state,
                                hiddenRetainedBase.presence)
                    .has_value());
}

TEST(deltaChangesOnlyNamedRecordsAndRejectsStaleOrMalformedChanges) {
    const UiFrame base = frame();
    auto targetParts = parts();
    auto& document = *std::find_if(
        targetParts.state.nodes.begin(), targetParts.state.nodes.end(),
        [](const UiNodeState& record) {
            return record.id.value() == kDocumentNodeId;
        });
    document.leaf = UiLeafState{"changed"};
    const UiFrame target =
        UiFrame::require(std::move(targetParts.schema),
                         std::move(targetParts.state),
                         std::move(targetParts.presence));

    const auto delta = UiFrameDeltaCodec{}.derive(base, target);
    const auto* changes = std::get_if<UiFrameChanges>(&delta.body());
    ASSERT_TRUE(changes != nullptr);
    ASSERT_EQ(changes->state.size(), std::size_t{1});
    ASSERT_TRUE(changes->presence.empty());
    ASSERT_TRUE(delta.base() == delta.target());
    const auto replay = UiFrameDeltaCodec{}.replay(base, delta);
    ASSERT_TRUE(replay.accepted());
    ASSERT_TRUE(*replay.frame == target);

    auto staleVersion = base.version();
    staleVersion.presenceBasis =
        PresenceBasis{staleVersion.presenceBasis.value() + 1};
    const auto stale =
        UiFrameDelta::changes(staleVersion, target.version(), *changes);
    ASSERT_TRUE(UiFrameDeltaCodec{}.replay(base, stale).error ==
                UiFrameReplayError::StaleVersion);

    UiFrameChanges unknown = *changes;
    unknown.state.front().id = UiNodeId{"missing"};
    const auto malformed =
        UiFrameDelta::changes(base.version(), target.version(), unknown);
    ASSERT_TRUE(UiFrameDeltaCodec{}.replay(base, malformed).error ==
                UiFrameReplayError::MalformedDelta);
}

TEST(presenceChangesAdvanceBasisAndReplayAtomically) {
    const UiFrame base = frame();
    auto targetParts = parts();
    auto& notice = *std::find_if(
        targetParts.presence.nodes.begin(), targetParts.presence.nodes.end(),
        [](const UiPresenceRecord& record) {
            return record.id.value() == kNoticeNodeId;
        });
    notice.present = false;
    targetParts.presence.basis =
        PresenceBasis{base.version().presenceBasis.value() + 1};
    const UiFrame target =
        UiFrame::require(std::move(targetParts.schema),
                         std::move(targetParts.state),
                         std::move(targetParts.presence));
    const auto delta = UiFrameDeltaCodec{}.derive(base, target);
    const auto replay = UiFrameDeltaCodec{}.replay(base, delta);
    ASSERT_TRUE(replay.accepted());
    ASSERT_TRUE(*replay.frame == target);

    const auto* changes = std::get_if<UiFrameChanges>(&delta.body());
    ASSERT_TRUE(changes != nullptr);
    const auto unchangedBasis =
        UiFrameDelta::changes(base.version(), base.version(), *changes);
    ASSERT_TRUE(UiFrameDeltaCodec{}.replay(base, unchangedBasis).error ==
                UiFrameReplayError::MalformedDelta);
}

TEST(focusRestorationRequiresVisibleEndpointInTheSameCommit) {
    auto capturedParts = parts();
    capturedParts.state.focusPath->push_back(
        UiNodeId{std::string{kHeaderPromptInputNodeId}});
    auto& editorPresence = *std::find_if(
        capturedParts.presence.nodes.begin(),
        capturedParts.presence.nodes.end(),
        [](const UiPresenceRecord& record) {
            return record.id.value() == kEditorNodeId;
        });
    editorPresence.present = false;
    const UiFrame captured =
        UiFrame::require(std::move(capturedParts.schema),
                         std::move(capturedParts.state),
                         std::move(capturedParts.presence));

    auto restoredParts = parts();
    restoredParts.presence.basis =
        PresenceBasis{captured.version().presenceBasis.value() + 1};
    const UiFrame restored =
        UiFrame::require(std::move(restoredParts.schema),
                         std::move(restoredParts.state),
                         std::move(restoredParts.presence));
    const auto restoration = UiFrameDeltaCodec{}.derive(captured, restored);
    const auto replay = UiFrameDeltaCodec{}.replay(captured, restoration);
    ASSERT_TRUE(replay.accepted());
    ASSERT_TRUE(*replay.frame == restored);

    UiFrameChanges invalidPop;
    invalidPop.focusPathChanged = true;
    invalidPop.focusPath =
        std::vector<UiNodeId>{UiNodeId{std::string{kEditorNodeId}}};
    const auto invalid = UiFrameDelta::changes(
        captured.version(), captured.version(), std::move(invalidPop));
    ASSERT_TRUE(UiFrameDeltaCodec{}.replay(captured, invalid).error ==
                UiFrameReplayError::InvalidFrame);
}

}  // namespace

int main() {
    RUN(frameRejectsMismatchedRecordsGenerationsAndHiddenFocus);
    RUN(deltaChangesOnlyNamedRecordsAndRejectsStaleOrMalformedChanges);
    RUN(presenceChangesAdvanceBasisAndReplayAtomically);
    RUN(focusRestorationRequiresVisibleEndpointInTheSameCommit);
    return failed == 0 ? 0 : 1;
}
