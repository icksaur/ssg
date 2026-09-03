#pragma once

#include <ssg/CommandInvocation.h>
#include <ssg/ExternalModificationFlow.h>
#include <ssg/Keymap.h>
#include <ssg/PaneNavigation.h>
#include <ssg/Picker.h>
#include <ssg/Selection.h>
#include <ssg/TabManager.h>
#include <ssg/TreeModel.h>
#include <ssg/UiTree.h>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace ssg {

struct ClientKeyInput {
    KeyStroke stroke;
    std::string committedText;

    friend bool operator==(const ClientKeyInput&, const ClientKeyInput&) = default;
};

enum class InputPointerButton : std::uint8_t {
    Primary = 0,
    Auxiliary = 1,
    Secondary = 2,
};

enum class InputPointerPhase : std::uint8_t {
    Press = 0,
    Move = 1,
    Release = 2,
    Cancel = 3,
};

struct SemanticInputBasis {
    Revision observedRevision;

    friend bool operator==(const SemanticInputBasis&,
                           const SemanticInputBasis&) = default;
};

struct TabPointerInput {
    SemanticInputBasis basis;
    TabId tabId;
    InputPointerButton button = InputPointerButton::Primary;
    InputPointerPhase phase = InputPointerPhase::Press;

    friend bool operator==(const TabPointerInput&,
                           const TabPointerInput&) = default;
};

struct TreePointerInput {
    SemanticInputBasis basis;
    TreeNodeId nodeId;
    InputPointerButton button = InputPointerButton::Primary;
    InputPointerPhase phase = InputPointerPhase::Press;

    friend bool operator==(const TreePointerInput&,
                           const TreePointerInput&) = default;
};

struct PickerPointerInput {
    PickerActivation activation;
    std::string candidateId;
    InputPointerButton button = InputPointerButton::Primary;
    InputPointerPhase phase = InputPointerPhase::Press;

    friend bool operator==(const PickerPointerInput&,
                           const PickerPointerInput&) = default;
};

struct ExternalActionPointerInput {
    SemanticInputBasis basis;
    ExternalActionInvocation invocation;
    InputPointerButton button = InputPointerButton::Primary;
    InputPointerPhase phase = InputPointerPhase::Press;

    friend bool operator==(const ExternalActionPointerInput&,
                           const ExternalActionPointerInput&) = default;
};

struct NoticeActionPointerInput {
    SemanticInputBasis basis;
    std::string actionId;
    InputPointerButton button = InputPointerButton::Primary;
    InputPointerPhase phase = InputPointerPhase::Press;

    friend bool operator==(const NoticeActionPointerInput&,
                           const NoticeActionPointerInput&) = default;
};

struct DocumentPointerInput {
    SemanticInputBasis basis;
    std::optional<ByteOffset> position;
    bool additive = false;
    bool selectWord = false;
    InputPointerButton button = InputPointerButton::Primary;
    InputPointerPhase phase = InputPointerPhase::Press;
    DocumentPointerEdge edge = DocumentPointerEdge::None;

    friend bool operator==(const DocumentPointerInput&,
                           const DocumentPointerInput&) = default;
};

struct ScrollLinesInput {
    SemanticInputBasis basis;
    ScrollLines action;

    friend bool operator==(const ScrollLinesInput&,
                           const ScrollLinesInput&) = default;
};

struct ScrollFractionInput {
    SemanticInputBasis basis;
    ScrollFraction action;

    friend bool operator==(const ScrollFractionInput&,
                           const ScrollFractionInput&) = default;
};

struct PauseFollowTransition {
    friend bool operator==(const PauseFollowTransition&,
                           const PauseFollowTransition&) = default;
};

struct PaneFocusTransition {
    PaneId pane;

    friend bool operator==(const PaneFocusTransition&,
                           const PaneFocusTransition&) = default;
};

struct SelectionRangeTransition {
    ByteOffset anchor;
    ByteOffset active;

    friend bool operator==(const SelectionRangeTransition&,
                           const SelectionRangeTransition&) = default;
};

struct SelectionTransition {
    TabId activeTab;
    Revision documentRevision;
    std::vector<SelectionRangeTransition> selections;

    friend bool operator==(const SelectionTransition&,
                           const SelectionTransition&) = default;
};

struct PointerSelectionTransition {
    ByteOffset position;

    friend bool operator==(const PointerSelectionTransition&,
                           const PointerSelectionTransition&) = default;
};

using ViewTransition =
    std::variant<PauseFollowTransition, PaneFocusTransition,
                 SelectionTransition, PointerSelectionTransition>;

struct ViewTransitionInput {
    // CONTRACT: observedRevision identifies the active document as well as its
    // state because every active-document switch advances EditorSession's
    // revision; exact revision validation precedes every transition.
    SemanticInputBasis basis;
    ViewTransition transition;

    friend bool operator==(const ViewTransitionInput&,
                           const ViewTransitionInput&) = default;
};

using ClientInput =
    std::variant<ClientKeyInput, TabPointerInput, TreePointerInput,
                 PickerPointerInput, ExternalActionPointerInput,
                 NoticeActionPointerInput,
                 DocumentPointerInput, ScrollLinesInput,
                 ScrollFractionInput, ViewTransitionInput>;

enum class ClientOwnedInputKind : std::uint8_t {
    AppendText = 0,
    DeleteGraphemeBackward = 1,
    DeleteWordBackward = 2,
    SelectNext = 3,
    SelectPrevious = 4,
    Submit = 5,
};

struct ClientOwnedInput {
    ClientOwnedInputKind kind;
    std::string text;

    friend bool operator==(const ClientOwnedInput&,
                           const ClientOwnedInput&) = default;
};

enum class ClientInputOutcome : std::uint8_t {
    Unhandled = 0,
    ClientOwned = 1,
    Dispatched = 2,
    Rejected = 3,
    ViewOwned = 4,
};

struct ClientInputResult {
    ClientInputOutcome outcome;
    std::optional<ClientOwnedInput> clientOwned;
    std::optional<CommandResult> command;
    std::optional<PickerActivation> pickerActivation;
};

}  // namespace ssg
