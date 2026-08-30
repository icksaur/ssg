#pragma once

#include <ssg/EditorClient.h>
#include <ssg/ExternalModificationFlow.h>
#include <ssg/Keymap.h>
#include <ssg/Picker.h>
#include <ssg/Selection.h>
#include <ssg/StatusActionInvocation.h>
#include <ssg/TabManager.h>
#include <ssg/TreeModel.h>
#include <ssg/UiTree.h>

#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace ssg {

struct ClientKeyInput {
    KeyStroke stroke;
    std::string committedText;

    friend bool operator==(const ClientKeyInput&, const ClientKeyInput&) = default;
};

enum class ClientInputKind : std::uint8_t {
    Key,
    Tab,
    Tree,
    Picker,
    PromptControl,
    ExternalAction,
    StatusAction,
    PublishedUiAction,
    NoticeAction,
    Document,
    ScrollLines,
    ScrollFraction,
    ViewNavigation,
    ResolvedPaneFocus,
};

enum class InputPointerButton : std::uint8_t {
    Primary,
    Auxiliary,
    Secondary,
};

enum class InputPointerPhase : std::uint8_t {
    Press,
    Move,
    Release,
    Cancel,
};

enum class DocumentPointerEdge : std::uint8_t {
    None,
    Before,
    After,
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

struct PromptControlPointerInput {
    SemanticInputBasis basis;
    std::string controlId;
    InputPointerButton button = InputPointerButton::Primary;
    InputPointerPhase phase = InputPointerPhase::Press;

    friend bool operator==(const PromptControlPointerInput&,
                           const PromptControlPointerInput&) = default;
};

struct ExternalActionPointerInput {
    SemanticInputBasis basis;
    ExternalActionInvocation invocation;
    InputPointerButton button = InputPointerButton::Primary;
    InputPointerPhase phase = InputPointerPhase::Press;

    friend bool operator==(const ExternalActionPointerInput&,
                           const ExternalActionPointerInput&) = default;
};

struct StatusActionPointerInput {
    SemanticInputBasis basis;
    StatusActionInvocation invocation;
    InputPointerButton button = InputPointerButton::Primary;
    InputPointerPhase phase = InputPointerPhase::Press;

    friend bool operator==(const StatusActionPointerInput&,
                           const StatusActionPointerInput&) = default;
};

struct PublishedUiActionPointerInput {
    SemanticInputBasis basis;
    Generation schemaGeneration;
    UiNodeId nodeId;
    InputPointerButton button = InputPointerButton::Primary;
    InputPointerPhase phase = InputPointerPhase::Press;

    friend bool operator==(const PublishedUiActionPointerInput&,
                           const PublishedUiActionPointerInput&) = default;
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

enum class SemanticScrollTarget : std::uint8_t {
    Document,
    Tree,
};

struct ScrollLinesInput {
    SemanticInputBasis basis;
    SemanticScrollTarget target = SemanticScrollTarget::Document;
    std::int64_t rows = 0;

    friend bool operator==(const ScrollLinesInput&,
                           const ScrollLinesInput&) = default;
};

struct ScrollFractionInput {
    SemanticInputBasis basis;
    SemanticScrollTarget target = SemanticScrollTarget::Document;
    std::uint32_t numerator = 0;
    std::uint32_t denominator = 1;

    friend bool operator==(const ScrollFractionInput&,
                           const ScrollFractionInput&) = default;
};

struct ViewNavigationInput {
    // CONTRACT: observedRevision identifies the active document as well as its
    // state because every active-document switch advances EditorSession's
    // revision; exact revision validation must precede the follow transition.
    SemanticInputBasis basis;

    friend bool operator==(const ViewNavigationInput&,
                           const ViewNavigationInput&) = default;
};

struct ResolvedPaneFocusInput {
    SemanticInputBasis basis;

    friend bool operator==(const ResolvedPaneFocusInput&,
                           const ResolvedPaneFocusInput&) = default;
};

using ClientInput =
    std::variant<ClientKeyInput, TabPointerInput, TreePointerInput,
                 PickerPointerInput, PromptControlPointerInput,
                 ExternalActionPointerInput, StatusActionPointerInput,
                 PublishedUiActionPointerInput, NoticeActionPointerInput,
                 DocumentPointerInput, ScrollLinesInput,
                 ScrollFractionInput, ViewNavigationInput,
                 ResolvedPaneFocusInput>;

enum class ClientOwnedInputKind : std::uint8_t {
    AppendText,
    DeleteGraphemeBackward,
    DeleteWordBackward,
    SelectNext,
    SelectPrevious,
    Submit,
};

struct ClientOwnedInput {
    ClientOwnedInputKind kind;
    std::string text;

    friend bool operator==(const ClientOwnedInput&,
                           const ClientOwnedInput&) = default;
};

enum class ClientInputOutcome : std::uint8_t {
    Unhandled,
    ClientOwned,
    Dispatched,
    Rejected,
    ViewOwned,
};

struct ClientInputResult {
    ClientInputOutcome outcome;
    std::optional<ClientOwnedInput> clientOwned;
    std::optional<CommandResult> command;
    std::optional<PickerActivation> pickerActivation;
};

}  // namespace ssg
