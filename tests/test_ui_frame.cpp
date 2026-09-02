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
                            Style{}.inputLineSigil)
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

    auto undeclaredFocusHost = valid;
    auto& root = std::get<UiContainer>(undeclaredFocusHost.schema.root.content);
    auto& body = std::get<UiContainer>(root.children[1].content);
    auto& contentContainer =
        std::get<UiContainer>(body.children[1].content);
    contentContainer.children[3].focusContext.reset();
    ASSERT_FALSE(UiFrame::create(undeclaredFocusHost.schema,
                                 undeclaredFocusHost.state,
                                 undeclaredFocusHost.presence)
                     .has_value());

    auto nonFocusNode = valid;
    nonFocusNode.state.focusPath =
        std::vector<UiNodeId>{UiNodeId{std::string{kNoticeNodeId}}};
    ASSERT_FALSE(UiFrame::create(nonFocusNode.schema, nonFocusNode.state,
                                 nonFocusNode.presence)
                     .has_value());

    auto unknownFocusNode = valid;
    unknownFocusNode.state.focusPath =
        std::vector<UiNodeId>{UiNodeId{"missing"}};
    ASSERT_FALSE(UiFrame::create(unknownFocusNode.schema,
                                 unknownFocusNode.state,
                                 unknownFocusNode.presence)
                     .has_value());
}

TEST(frameDerivesEffectiveContextFromTheEndpointHost) {
    const UiFrame editor = frame();
    ASSERT_TRUE(editor.effectiveFocus() == FocusTarget::Editor);

    auto promptParts = parts();
    promptParts.state.focusPath->push_back(
        UiNodeId{std::string{kHeaderPromptInputNodeId}});
    const UiFrame prompt =
        UiFrame::require(std::move(promptParts.schema),
                         std::move(promptParts.state),
                         std::move(promptParts.presence));
    ASSERT_TRUE(prompt.effectiveFocus() == FocusTarget::Prompt);
}

}  // namespace

SSG_TEST_SUITE(test_ui_frame) {
    RUN(frameRejectsMismatchedRecordsGenerationsAndHiddenFocus);
    RUN(frameDerivesEffectiveContextFromTheEndpointHost);
    return failed == 0 ? 0 : 1;
}
