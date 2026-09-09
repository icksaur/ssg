#pragma once

#include <ssg/ClientInput.h>
#include <ssg/CompiledKeymap.h>
#include <ssg/DocumentPointerGesture.h>
#include <ssg/FollowEditsModel.h>
#include <ssg/PaletteSearcher.h>
#include <ssg/PaneTopology.h>
#include <ssg/PromptSurface.h>
#include <ssg/Selection.h>
#include <ssg/TabManager.h>
#include <ssg/TreeModel.h>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ssg {

struct InputRoutingNoticeAction {
    std::string id;
    CommandName command;
};

struct InputRoutingSnapshot {
    std::reference_wrapper<CompiledKeymap const> keymap;
    PromptRoutingState prompt;
    std::string_view clipboardText;
    std::string_view activeText;
    std::optional<FileDocumentId> activeDocument;
    std::uint64_t documentRevision = 0;
    std::optional<TabId> activeTab;
    std::optional<TabKind> activeTabKind;
    std::uint64_t diffRevision = 0;
    std::reference_wrapper<SelectionSet const> selections;
    FollowMode followMode = FollowMode::Following;
    std::optional<std::vector<InputRoutingNoticeAction>> noticeActions;
    std::vector<PaneId> panes;
    DocumentPointerGesture gesture;
    std::optional<TreeProviderKind> activeTreeProvider;
    bool searchEditing = false;
};

struct ApplySelections {
    FileDocumentId document;
    SelectionSet selections;
};

struct FocusPane {
    PaneId pane;
};

struct SearchQueryChange {
    enum class Kind : std::uint8_t {
        Append,
        DeleteGraphemeBack,
        MoveFirst,
        MoveLast,
        Submit,
    } kind = Kind::Append;
    std::string text;
};

using EditorMutation =
    std::variant<ApplySelections, FocusPane, SearchQueryChange>;

struct RouteUnhandled {};

struct RouteRejected {
    std::string message;
};

struct RouteAccepted {
    std::optional<EditorMutation> mutation;
};

struct RouteClientOwned {
    ClientOwnedInput input;
};

struct RouteViewAction {
    ViewAction action;
};

struct RouteDispatch {
    ClientCommand command;
};

using InputRouteAction =
    std::variant<RouteUnhandled, RouteRejected, RouteAccepted,
                 RouteClientOwned, RouteViewAction, RouteDispatch>;

struct KeepGesture {};
struct ClearGesture {};
struct SetGesture {
    DocumentPointerGesture gesture;
};

using GestureOnAccepted =
    std::variant<KeepGesture, ClearGesture, SetGesture>;

struct RoutedInput {
    InputRouteAction action;
    GestureOnAccepted gestureOnAccepted;
    bool clearGestureOnRejection = false;
};

[[nodiscard]] RoutedInput routeInput(InputRoutingSnapshot const& snapshot,
                                     ClientInput const& input);

}  // namespace ssg
