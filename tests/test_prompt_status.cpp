#include <ssg/PromptSurface.h>
#include <ssg/StatusBar.h>
#include "test_helpers.h"

#include <string>
#include <vector>

namespace {

using namespace ssg;

PromptRequest request(PromptKind kind) {
    PromptRequest value;
    value.kind = kind;
    value.accessibleLabel =
        kind == PromptKind::Path ? "Path" :
        kind == PromptKind::Replace ? "Replace" : "Find";
    value.inputs.push_back(
        {kind == PromptKind::Path ? "path" : "find",
         kind == PromptKind::Path ? "Path" : "Find text", "needle"});
    if (kind == PromptKind::Replace) {
        value.inputs.push_back({"replace", "Replacement text", "value"});
    }
    if (kind == PromptKind::Find || kind == PromptKind::Replace) {
        value.toggles.push_back({"case", "Case sensitive", false, 6});
        value.toggles.push_back({"word", "Whole word", true, 7});
        value.matchCount = PromptMatchCount{"matches", "Match count", "2/7"};
    }
    return value;
}

TEST(promptSubmitAndCancelAreNonModal) {
    PromptSurface surface;
    ASSERT_TRUE(surface.open(request(PromptKind::Replace)).accepted());
    const auto submitted = surface.submit();
    ASSERT_TRUE(submitted.accepted());
    ASSERT_TRUE(submitted.submission.has_value());
    ASSERT_EQ(submitted.submission->values,
              (std::vector<std::string>{"needle", "value"}));
    ASSERT_FALSE(surface.active());

    ASSERT_TRUE(surface.open(request(PromptKind::Path)).accepted());
    const auto cancelled = surface.cancel();
    ASSERT_TRUE(cancelled.accepted());
    ASSERT_FALSE(cancelled.submission.has_value());
    ASSERT_FALSE(surface.active());
}

TEST(promptOpenResetsTheActiveInputPerKindAndOnTransition) {
    PromptSurface surface;
    // Replace opens focused on its replacement input (index 1), not its query.
    ASSERT_TRUE(surface.open(request(PromptKind::Replace)).accepted());
    ASSERT_EQ(surface.activeInput(), std::size_t{1});
    // A kind transition re-opens and RESETS: Find focuses its sole input (0).
    ASSERT_TRUE(surface.open(request(PromptKind::Find)).accepted());
    ASSERT_EQ(surface.activeInput(), std::size_t{0});
    // The single-input generic path prompt also resets to 0.
    ASSERT_TRUE(surface.open(request(PromptKind::Path)).accepted());
    ASSERT_EQ(surface.activeInput(), std::size_t{0});
}

TEST(promptFocusOnlyAddressesAnInputNeverAToggleOrCount) {
    PromptSurface surface;
    ASSERT_TRUE(surface.open(request(PromptKind::Replace)).accepted());
    // Both authored input identities can receive focus.
    ASSERT_TRUE(surface.focusInput("find").accepted());
    ASSERT_EQ(surface.activeInput(), std::size_t{0});
    ASSERT_TRUE(surface.focusInput("replace").accepted());
    ASSERT_EQ(surface.activeInput(), std::size_t{1});
    // A non-input id (a toggle or the match count can never take
    // focus) is rejected as UnknownInput and leaves the active input unchanged.
    const auto rejected = surface.focusInput("case");
    ASSERT_FALSE(rejected.accepted());
    ASSERT_EQ(rejected.error->code, PromptErrorCode::UnknownInput);
    ASSERT_EQ(surface.activeInput(), std::size_t{1});
    // focusNextInput wraps across the inputs only.
    ASSERT_TRUE(surface.focusNextInput().accepted());
    ASSERT_EQ(surface.activeInput(), std::size_t{0});
    ASSERT_TRUE(surface.focusNextInput().accepted());
    ASSERT_EQ(surface.activeInput(), std::size_t{1});
}

TEST(promptControlsCarryTheirOperatingCommands) {
    // Toggle controls carry the command that operates them; inputs are operated
    // through typed routing and carry no command string.
    PromptRequest findReq;
    findReq.kind = PromptKind::Find;
    findReq.accessibleLabel = "Find";
    findReq.inputs.push_back({"find.query", "Find text", "needle"});
    ASSERT_TRUE(resolvePromptControls(findReq).front().command.empty());
    PromptRequest replaceReq;
    replaceReq.kind = PromptKind::Replace;
    replaceReq.accessibleLabel = "Replace";
    replaceReq.inputs.push_back({"find.query", "Find text", "needle"});
    replaceReq.inputs.push_back({"replace.replacement", "Replacement text", "value"});
    const auto replaceControls = resolvePromptControls(replaceReq);
    ASSERT_TRUE(replaceControls.at(0).command.empty());
    ASSERT_TRUE(replaceControls.at(1).command.empty());
    PromptRequest pathReq;
    pathReq.kind = PromptKind::Path;
    pathReq.accessibleLabel = "Path";
    pathReq.inputs.push_back({"path", "Path", "/tmp"});
    ASSERT_TRUE(resolvePromptControls(pathReq).front().command.empty());
}

StatusItem status(std::uint64_t id, StatusPriority priority,
                  std::string text, std::vector<UiAction> actions = {}) {
    return StatusItem{StatusId{id}, priority, std::move(text),
                      std::move(actions)};
}

std::vector<std::uint64_t> ids(const StatusViewState& view) {
    std::vector<std::uint64_t> values;
    for (const auto& item : view.items) {
        values.push_back(item.id.value());
    }
    return values;
}

TEST(statusPriorityAndNavigationTransitionTable) {
    StatusBar bar;
    ASSERT_TRUE(bar.enqueue(status(1, StatusPriority::Information, "one")).accepted);
    ASSERT_TRUE(bar.enqueue(status(2, StatusPriority::Warning, "two")).accepted);
    ASSERT_TRUE(bar.enqueue(status(3, StatusPriority::Error, "three")).accepted);
    ASSERT_TRUE(bar.enqueue(status(4, StatusPriority::Warning, "four")).accepted);

    ASSERT_EQ(ids(bar.viewState()),
              (std::vector<std::uint64_t>{3, 2, 4, 1}));
    auto view = bar.viewState();
    ASSERT_EQ(view.selected, std::size_t{0});
    bar.next();
    view = bar.viewState();
    ASSERT_EQ(view.selected, std::size_t{1});
    bar.previous();
    view = bar.viewState();
    ASSERT_EQ(view.selected, std::size_t{0});
    bar.previous();
    view = bar.viewState();
    ASSERT_EQ(view.selected, std::size_t{3});
    bar.dismiss();
    ASSERT_EQ(ids(bar.viewState()),
              (std::vector<std::uint64_t>{3, 2, 4}));
    view = bar.viewState();
    ASSERT_EQ(view.selected, std::size_t{2});
}

TEST(statusCapacityAdmissionAndEvictionTable) {
    StatusBar bar;
    for (std::uint64_t id = 1; id <= StatusBar::kCapacity; ++id) {
        ASSERT_TRUE(bar.enqueue(
            status(id, StatusPriority::Information, std::to_string(id))).accepted);
    }
    const auto rejected =
        bar.enqueue(status(17, StatusPriority::Progress, "rejected"));
    ASSERT_FALSE(rejected.accepted);
    auto view = bar.viewState();
    ASSERT_EQ(view.items.size(), StatusBar::kCapacity);

    const auto admitted =
        bar.enqueue(status(18, StatusPriority::Error, "admitted"));
    ASSERT_TRUE(admitted.accepted);
    ASSERT_EQ(admitted.evicted, std::optional<StatusId>{StatusId{1}});
    view = bar.viewState();
    ASSERT_EQ(view.items.front().id, StatusId{18});
}

TEST(statusActionsProjectCanonicalOpaqueNodeIdentities) {
    StatusBar bar;
    const auto enqueued = bar.enqueue(status(
        7, StatusPriority::Error, "Build failed",
        {UiAction{"retry", "Retry build", "build.retry"},
         UiAction{"r\xC3\xA9try", "Retry localized", "build.localized"}}));
    ASSERT_TRUE(enqueued.accepted);
    const auto nodes = bar.actionNodes();
    ASSERT_EQ(nodes.size(), std::size_t{2});
    ASSERT_EQ(nodes[0].id,
              UiNodeId{"footer.status_action/7/" +
                       std::to_string(enqueued.generation) +
                       "/7265747279"});
    ASSERT_EQ(nodes[0].accessibleLabel, std::string{"Retry build"});
    ASSERT_EQ(nodes[0].commandId, std::string{"build.retry"});
    ASSERT_EQ(nodes[1].id,
              UiNodeId{"footer.status_action/7/" +
                       std::to_string(enqueued.generation) +
                       "/72c3a9747279"});

    StatusViewState differentStatus = bar.viewState();
    differentStatus.items[0].id = StatusId{8};
    ASSERT_NE(projectStatusActionNodes(differentStatus)[0].id, nodes[0].id);
    StatusViewState differentGeneration = bar.viewState();
    ++differentGeneration.items[0].generation;
    ASSERT_NE(projectStatusActionNodes(differentGeneration)[0].id,
              nodes[0].id);
}

TEST(statusBarRejectsDuplicateActionIdentityBeforeMutation) {
    StatusBar bar;
    const auto rejected = bar.enqueue(status(
        7, StatusPriority::Error, "Build failed",
        {UiAction{"retry", "Retry build", "build.retry"},
         UiAction{"retry", "Retry elsewhere", "build.other"}}));
    ASSERT_FALSE(rejected.accepted);
    ASSERT_TRUE(bar.viewState().items.empty());
    ASSERT_TRUE(bar.actionNodes().empty());
}

TEST(footerTextAndAccessibilityMatchGolden) {
    StatusBar bar;
    std::vector<UiAction> actions{
        {"retry", "Retry build", "build.retry"},
        {"log", "Open log", "log.open"}};
    ASSERT_TRUE(bar.enqueue(
        status(9, StatusPriority::Error, "Build failed", actions)).accepted);
    ASSERT_EQ(bar.footerText(), std::string{"Build failed 1/1"});
    const auto statusView = bar.viewState();
    ASSERT_EQ(statusView.items[0].actions[0].id, std::string{"retry"});
    ASSERT_EQ(statusView.items[0].actions[1].label, std::string{"Open log"});

    ASSERT_FALSE(statusView.items[0].accessibleLabel.empty());
    for (const auto& action : statusView.items[0].actions) {
        ASSERT_FALSE(action.label.empty());
    }
}

} // namespace

SSG_TEST_SUITE(ssg_prompt_status_tests) {
    RUN(promptSubmitAndCancelAreNonModal);
    RUN(promptOpenResetsTheActiveInputPerKindAndOnTransition);
    RUN(promptFocusOnlyAddressesAnInputNeverAToggleOrCount);
    RUN(promptControlsCarryTheirOperatingCommands);
    RUN(statusPriorityAndNavigationTransitionTable);
    RUN(statusCapacityAdmissionAndEvictionTable);
    RUN(statusActionsProjectCanonicalOpaqueNodeIdentities);
    RUN(statusBarRejectsDuplicateActionIdentityBeforeMutation);
    RUN(footerTextAndAccessibilityMatchGolden);
    return failed == 0 ? 0 : 1;
}
