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
#include <ssg/TextInputCommands.h>
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

struct ApplyTextInput {
    TextInputCommand command = TextInputCommand::Insert;
    TextInputArguments arguments;
};

struct ApplySelections {
    FileDocumentId document;
    SelectionSet selections;
    bool focusEditor = false;
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
        Focus,
    } kind = Kind::Append;
    std::string text;
};

struct ActivateTab {
    TabId tabId;
};

struct CloseTab {
    TabId tabId;
};

struct ActivateTreeNode {
    TreeNodeId nodeId;
};

struct UpdateFindQuery {
    std::string query;
};

struct UpdateReplacement {
    std::string replacement;
};

using EditorMutation =
    std::variant<ApplyTextInput, ApplySelections, FocusPane, SearchQueryChange,
                 ActivateTab, CloseTab, PromptValueArguments, UpdateFindQuery,
                 UpdateReplacement>;

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

struct InvokeExternalAction {
    ExternalActionInvocation invocation;
};

struct ActivateUiNode {
    UiNodeId nodeId;
};

using InputRouteAction =
    std::variant<RouteUnhandled, RouteRejected, RouteAccepted,
                 RouteClientOwned, RouteViewAction, RouteDispatch,
                 InvokeExternalAction, ActivateUiNode, ActivateTreeNode>;

struct RoutedInput {
    InputRouteAction action;
    // Engaged means "replace the editor's pointer gesture with this one" on
    // acceptance; a default-constructed gesture therefore clears it.  Empty
    // means the gesture is left alone.
    std::optional<DocumentPointerGesture> gestureOnAccepted;
    bool clearGestureOnRejection = false;
};

[[nodiscard]] RoutedInput routeInput(InputRoutingSnapshot const& snapshot,
                                     ClientInput const& input);

}  // namespace ssg
