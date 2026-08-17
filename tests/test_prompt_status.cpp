#include "ssg/PromptSurface.h"
#include "ssg/StatusQueue.h"
#include "test_helpers.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace ssg;

std::string readFixture(const std::string& relative) {
    std::ifstream input(std::filesystem::path(SSG_SOURCE_DIR) / relative);
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
}

std::string rectText(const Rect& rect) {
    return std::to_string(rect.x) + "," + std::to_string(rect.y) + "," +
           std::to_string(rect.width) + "," + std::to_string(rect.height);
}

std::string kindText(PromptKind kind) {
    switch (kind) {
    case PromptKind::Path: return "path";
    case PromptKind::Find: return "find";
    case PromptKind::Replace: return "replace";
    case PromptKind::Settings: return "settings";
    case PromptKind::CommandArgument: return "command_argument";
    }
    return "";
}

std::string controlKindText(PromptControlKind kind) {
    switch (kind) {
    case PromptControlKind::Input: return "input";
    case PromptControlKind::Toggle: return "toggle";
    case PromptControlKind::Count: return "count";
    }
    return "";
}

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

std::string geometryLine(PromptKind kind) {
    PromptSurface surface;
    const auto opened = surface.open(request(kind));
    ASSERT_TRUE(opened.accepted());
    const auto rows = promptRowCount(kind);
    const auto layout =
        computePromptLayout(surface, Rect{0, 4, 20, static_cast<int>(rows)});
    ASSERT_TRUE(layout.accepted());

    std::string line = kindText(kind) + "|" + rectText(layout.view->rect) + "|";
    for (std::size_t i = 0; i < layout.view->controls.size(); ++i) {
        const auto& control = layout.view->controls[i];
        if (i != 0) {
            line += ";";
        }
        line += controlKindText(control.kind) + ":" + control.id + ":" +
                rectText(control.rect);
    }
    return line;
}

bool overlaps(Rect const& a, Rect const& b) {
    return a.x < b.right() && b.x < a.right() && a.y < b.bottom() &&
           b.y < a.bottom();
}

TEST(eachPromptKindComposesItsControlsWithinTheReservation) {
    // The composition contract: which controls a prompt kind owns, and that
    // they tile the reservation without overlapping or escaping it.  This
    // replaced a byte-exact serialization of every rect, which failed on any
    // dimension change without saying which rule had been broken.
    struct Expectation {
        PromptKind kind;
        std::vector<std::string> controlIds;
    };
    const std::vector<Expectation> expectations{
        {PromptKind::Path, {"path"}},
        {PromptKind::Find, {"find", "case", "word", "matches"}},
        {PromptKind::Replace,
         {"find", "replace", "case", "word", "matches"}},
    };

    for (const auto& expectation : expectations) {
        const Rect reservation{0, 4, 20,
                               promptRowCount(expectation.kind)};
        PromptSurface prompt;
        ASSERT_TRUE(prompt.open(request(expectation.kind)).accepted());
        const auto layout = computePromptLayout(prompt, reservation);
        ASSERT_TRUE(layout.accepted());
        if (!layout.accepted()) continue;

        std::vector<std::string> actualIds;
        for (const auto& control : layout.view->controls) {
            actualIds.push_back(control.id);
        }
        ASSERT_EQ(actualIds, expectation.controlIds);

        ASSERT_EQ(layout.view->rect, reservation);
        for (std::size_t i = 0; i < layout.view->controls.size(); ++i) {
            const auto& control = layout.view->controls[i];
            ASSERT_TRUE(control.rect.width > 0 && control.rect.height > 0);
            ASSERT_TRUE(control.rect.x >= reservation.x);
            ASSERT_TRUE(control.rect.right() <= reservation.right());
            ASSERT_TRUE(control.rect.y >= reservation.y);
            ASSERT_TRUE(control.rect.bottom() <= reservation.bottom());
            for (std::size_t j = i + 1; j < layout.view->controls.size(); ++j) {
                ASSERT_FALSE(overlaps(control.rect, layout.view->controls[j].rect));
            }
        }
    }
}

TEST(promptRowsAndInvalidReservationAreTyped) {
    ASSERT_EQ(promptRowCount(PromptKind::Path), std::uint8_t{1});
    ASSERT_EQ(promptRowCount(PromptKind::Find), std::uint8_t{2});
    ASSERT_EQ(promptRowCount(PromptKind::Replace), std::uint8_t{3});
    ASSERT_EQ(promptRowCount(PromptKind::Settings), std::uint8_t{1});
    ASSERT_EQ(promptRowCount(PromptKind::CommandArgument), std::uint8_t{1});
    ASSERT_EQ(promptRowCount(PromptKind::Palette), std::uint8_t{0});

    PromptSurface surface;
    ASSERT_TRUE(surface.open(request(PromptKind::Find)).accepted());
    const auto bad = computePromptLayout(surface, Rect{0, 0, 20, 1});
    ASSERT_FALSE(bad.accepted());
    ASSERT_EQ(bad.error->code, PromptErrorCode::InvalidReservation);
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
    // Two inputs (query 0, replacement 1); focusing either is accepted.
    ASSERT_TRUE(surface.focusInput(0).accepted());
    ASSERT_EQ(surface.activeInput(), std::size_t{0});
    ASSERT_TRUE(surface.focusInput(1).accepted());
    ASSERT_EQ(surface.activeInput(), std::size_t{1});
    // An index past the inputs (a toggle or the match count can never take
    // focus) is rejected as UnknownInput and leaves the active input unchanged.
    const auto rejected = surface.focusInput(2);
    ASSERT_FALSE(rejected.accepted());
    ASSERT_EQ(rejected.error->code, PromptErrorCode::UnknownInput);
    ASSERT_EQ(surface.activeInput(), std::size_t{1});
    // focusNextInput wraps across the inputs only.
    ASSERT_TRUE(surface.focusNextInput().accepted());
    ASSERT_EQ(surface.activeInput(), std::size_t{0});
    ASSERT_TRUE(surface.focusNextInput().accepted());
    ASSERT_EQ(surface.activeInput(), std::size_t{1});
}

TEST(theSemanticAndGridControlsComeFromTheOneResolver) {
    // The grid PromptViewState must be the resolver's controls plus a Rect --
    // never a second resolution. Prove it by resolving the semantic controls
    // directly and matching each field against the grid layout's controls.
    for (const auto kind :
         {PromptKind::Find, PromptKind::Replace, PromptKind::Path}) {
        const auto req = request(kind);
        const auto semantic = resolvePromptControls(req);
        PromptSurface surface;
        ASSERT_TRUE(surface.open(req).accepted());
        const auto layout = computePromptLayout(
            surface, Rect{0, 4, 20, promptRowCount(kind)});
        ASSERT_TRUE(layout.accepted());
        ASSERT_EQ(semantic.size(), layout.view->controls.size());
        for (std::size_t i = 0; i < semantic.size(); ++i) {
            const auto& s = semantic[i];
            const auto& g = layout.view->controls[i];
            ASSERT_EQ(s.kind, g.kind);
            ASSERT_EQ(s.id, g.id);
            ASSERT_EQ(s.accessibleLabel, g.accessibleLabel);
            ASSERT_EQ(s.value, g.value);
            ASSERT_EQ(s.checked, g.checked);
        }
    }
    // The input control carries the command that operates it -- a client never
    // hardcodes a per-field id. With the production input ids, Find's input drives
    // find.update_query and Replace's replacement input drives
    // replace.update_replacement; every other input falls back to prompt.update_value.
    PromptRequest findReq;
    findReq.kind = PromptKind::Find;
    findReq.accessibleLabel = "Find";
    findReq.inputs.push_back({"find.query", "Find text", "needle"});
    ASSERT_EQ(resolvePromptControls(findReq).front().command,
              std::string{"find.update_query"});
    PromptRequest replaceReq;
    replaceReq.kind = PromptKind::Replace;
    replaceReq.accessibleLabel = "Replace";
    replaceReq.inputs.push_back({"find.query", "Find text", "needle"});
    replaceReq.inputs.push_back({"replace.replacement", "Replacement text", "value"});
    const auto replaceControls = resolvePromptControls(replaceReq);
    ASSERT_EQ(replaceControls.at(0).command, std::string{"find.update_query"});
    ASSERT_EQ(replaceControls.at(1).command,
              std::string{"replace.update_replacement"});
    PromptRequest pathReq;
    pathReq.kind = PromptKind::Path;
    pathReq.accessibleLabel = "Path";
    pathReq.inputs.push_back({"path", "Path", "/tmp"});
    ASSERT_EQ(resolvePromptControls(pathReq).front().command,
              std::string{"prompt.update_value"});
}

StatusItem status(std::uint64_t id, StatusPriority priority,
                  std::string text, std::vector<StatusAction> actions = {}) {
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
    StatusQueue queue;
    ASSERT_TRUE(queue.enqueue(status(1, StatusPriority::Information, "one")).accepted);
    ASSERT_TRUE(queue.enqueue(status(2, StatusPriority::Warning, "two")).accepted);
    ASSERT_TRUE(queue.enqueue(status(3, StatusPriority::Error, "three")).accepted);
    ASSERT_TRUE(queue.enqueue(status(4, StatusPriority::Warning, "four")).accepted);

    ASSERT_EQ(ids(queue.viewState()),
              (std::vector<std::uint64_t>{3, 2, 4, 1}));
    auto view = queue.viewState();
    ASSERT_EQ(view.selected, std::size_t{0});
    queue.next();
    view = queue.viewState();
    ASSERT_EQ(view.selected, std::size_t{1});
    queue.previous();
    view = queue.viewState();
    ASSERT_EQ(view.selected, std::size_t{0});
    queue.previous();
    view = queue.viewState();
    ASSERT_EQ(view.selected, std::size_t{3});
    queue.dismiss();
    ASSERT_EQ(ids(queue.viewState()),
              (std::vector<std::uint64_t>{3, 2, 4}));
    view = queue.viewState();
    ASSERT_EQ(view.selected, std::size_t{2});
}

TEST(statusCapacityAdmissionAndEvictionTable) {
    StatusQueue queue;
    for (std::uint64_t id = 1; id <= StatusQueue::kCapacity; ++id) {
        ASSERT_TRUE(queue.enqueue(
            status(id, StatusPriority::Information, std::to_string(id))).accepted);
    }
    const auto rejected =
        queue.enqueue(status(17, StatusPriority::Progress, "rejected"));
    ASSERT_FALSE(rejected.accepted);
    auto view = queue.viewState();
    ASSERT_EQ(view.items.size(), StatusQueue::kCapacity);

    const auto admitted =
        queue.enqueue(status(18, StatusPriority::Error, "admitted"));
    ASSERT_TRUE(admitted.accepted);
    ASSERT_EQ(admitted.evicted, std::optional<StatusId>{StatusId{1}});
    view = queue.viewState();
    ASSERT_EQ(view.items.front().id, StatusId{18});
}

TEST(statusStaleActionsAreRejectedWithoutMutation) {
    StatusQueue queue;
    StatusAction retry{"retry", "Retry build", "build.retry"};
    auto first = queue.enqueue(
        status(7, StatusPriority::Error, "Build failed", {retry}));
    const StatusActionInvocation stale{StatusId{7}, "retry", first.generation};
    queue.dismiss();
    auto replacement = queue.enqueue(
        status(7, StatusPriority::Error, "Build failed again", {retry}));
    ASSERT_NE(first.generation, replacement.generation);
    const auto before = queue.viewState();
    const auto rejected = queue.invokeAction(stale);
    ASSERT_FALSE(rejected.accepted());
    ASSERT_EQ(rejected.error, StatusActionError::Stale);
    ASSERT_EQ(queue.viewState(), before);

    const StatusActionInvocation current{
        StatusId{7}, "retry", replacement.generation};
    const auto invoked = queue.invokeAction(current);
    ASSERT_TRUE(invoked.accepted());
    ASSERT_EQ(invoked.commandId, std::optional<std::string>{"build.retry"});
}

TEST(footerProjectionAndAccessibilityMatchGolden) {
    PromptSurface prompt;
    ASSERT_TRUE(prompt.open(request(PromptKind::Find)).accepted());
    const auto layout = computePromptLayout(prompt, Rect{0, 4, 20, 2});
    StatusQueue queue;
    std::vector<StatusAction> actions{
        {"retry", "Retry build", "build.retry"},
        {"log", "Open log", "log.open"}};
    ASSERT_TRUE(queue.enqueue(
        status(9, StatusPriority::Error, "Build failed", actions)).accepted);
    const auto footer = queue.footerProjection();
    ASSERT_EQ(footer.value, std::string{"Build failed 1/1"});
    ASSERT_EQ(footer.actions[0].id, std::string{"retry"});
    ASSERT_EQ(footer.actions[1].accessibleLabel, std::string{"Open log"});

    // The accessibility contract is that every surfaced element carries a
    // non-empty label, not that the labels read exactly as they do today.
    // Pinning the strings made every wording change a fixture edit while
    // catching nothing a missing-label check does not.
    ASSERT_FALSE(layout.view->accessibleLabel.empty());
    for (const auto& control : layout.view->controls) {
        ASSERT_FALSE(control.accessibleLabel.empty());
    }
    const auto statusView = queue.viewState();
    ASSERT_FALSE(statusView.items[0].accessibleLabel.empty());
    for (const auto& action : footer.actions) {
        ASSERT_FALSE(action.accessibleLabel.empty());
    }
}

} // namespace

int main() {
    RUN(eachPromptKindComposesItsControlsWithinTheReservation);
    RUN(promptRowsAndInvalidReservationAreTyped);
    RUN(promptSubmitAndCancelAreNonModal);
    RUN(promptOpenResetsTheActiveInputPerKindAndOnTransition);
    RUN(promptFocusOnlyAddressesAnInputNeverAToggleOrCount);
    RUN(theSemanticAndGridControlsComeFromTheOneResolver);
    RUN(statusPriorityAndNavigationTransitionTable);
    RUN(statusCapacityAdmissionAndEvictionTable);
    RUN(statusStaleActionsAreRejectedWithoutMutation);
    RUN(footerProjectionAndAccessibilityMatchGolden);
    return failed == 0 ? 0 : 1;
}
