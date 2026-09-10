#pragma once

#include <ssg/Command.h>
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

struct TabPointerInput {
    TabId tabId;
    InputPointerButton button = InputPointerButton::Primary;
    InputPointerPhase phase = InputPointerPhase::Press;

    friend bool operator==(const TabPointerInput&,
                           const TabPointerInput&) = default;
};

struct TreePointerInput {
    TreeNodeId nodeId;
    InputPointerButton button = InputPointerButton::Primary;
    InputPointerPhase phase = InputPointerPhase::Press;

    friend bool operator==(const TreePointerInput&,
                           const TreePointerInput&) = default;
};

struct SearchQueryPointerInput {
    InputPointerButton button = InputPointerButton::Primary;
    InputPointerPhase phase = InputPointerPhase::Press;

    friend bool operator==(const SearchQueryPointerInput&,
                           const SearchQueryPointerInput&) = default;
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
    ExternalActionInvocation invocation;
    InputPointerButton button = InputPointerButton::Primary;
    InputPointerPhase phase = InputPointerPhase::Press;

    friend bool operator==(const ExternalActionPointerInput&,
                           const ExternalActionPointerInput&) = default;
};

struct UiNodePointerInput {
    UiNodeId nodeId;

    friend bool operator==(const UiNodePointerInput&,
                           const UiNodePointerInput&) = default;
};

struct NoticeActionPointerInput {
    std::string actionId;
    InputPointerButton button = InputPointerButton::Primary;
    InputPointerPhase phase = InputPointerPhase::Press;

    friend bool operator==(const NoticeActionPointerInput&,
                           const NoticeActionPointerInput&) = default;
};

struct DocumentPointerInput {
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
    ScrollLines action;

    friend bool operator==(const ScrollLinesInput&,
                           const ScrollLinesInput&) = default;
};

struct ScrollFractionInput {
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
    std::uint64_t documentRevision;
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
    ViewTransition transition;

    friend bool operator==(const ViewTransitionInput&,
                           const ViewTransitionInput&) = default;
};

struct UpdatePromptValueInput {
    std::size_t index = 0;
    std::string value;

    friend bool operator==(const UpdatePromptValueInput&,
                           const UpdatePromptValueInput&) = default;
};

using ClientInput =
    std::variant<ClientKeyInput, TabPointerInput, TreePointerInput,
                 SearchQueryPointerInput, PickerPointerInput,
                 ExternalActionPointerInput, NoticeActionPointerInput,
                 UiNodePointerInput, DocumentPointerInput, ScrollLinesInput,
                 ScrollFractionInput, ViewTransitionInput, UpdatePromptValueInput>;

enum class ClientOwnedInputKind : std::uint8_t {
    AppendText = 0,
    DeleteGraphemeBackward = 1,
    DeleteWordBackward = 2,
    SelectNext = 3,
    SelectPrevious = 4,
    Submit = 5,
    // The TUI services these only after a resolved clipboard.paste gesture.
    SystemClipboardPasteIntoEditor = 6,
    SystemClipboardPasteIntoText = 7,
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
