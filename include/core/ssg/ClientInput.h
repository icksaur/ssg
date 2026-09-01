#pragma once

#include <ssg/EditorClient.h>
#include <ssg/ExternalModificationFlow.h>
#include <ssg/Keymap.h>
#include <ssg/Picker.h>
#include <ssg/Selection.h>
#include <ssg/TabManager.h>
#include <ssg/TreeModel.h>
#include <ssg/UiTree.h>
#include <ssg/detail/generated/semantic_wire_manifest.h>

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
#define SSG_ENUMERATOR(symbol, ordinal) symbol = ordinal,
    SSG_CLIENT_INPUT_KIND_ENUMERATORS(SSG_ENUMERATOR)
#undef SSG_ENUMERATOR
};
#undef SSG_CLIENT_INPUT_KIND_ENUMERATORS

enum class InputPointerButton : std::uint8_t {
#define SSG_ENUMERATOR(symbol, ordinal) symbol = ordinal,
    SSG_INPUT_POINTER_BUTTON_ENUMERATORS(SSG_ENUMERATOR)
#undef SSG_ENUMERATOR
};
#undef SSG_INPUT_POINTER_BUTTON_ENUMERATORS

enum class InputPointerPhase : std::uint8_t {
#define SSG_ENUMERATOR(symbol, ordinal) symbol = ordinal,
    SSG_INPUT_POINTER_PHASE_ENUMERATORS(SSG_ENUMERATOR)
#undef SSG_ENUMERATOR
};
#undef SSG_INPUT_POINTER_PHASE_ENUMERATORS

enum class DocumentPointerEdge : std::uint8_t {
#define SSG_ENUMERATOR(symbol, ordinal) symbol = ordinal,
    SSG_DOCUMENT_POINTER_EDGE_ENUMERATORS(SSG_ENUMERATOR)
#undef SSG_ENUMERATOR
};
#undef SSG_DOCUMENT_POINTER_EDGE_ENUMERATORS

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

enum class SemanticScrollTarget : std::uint8_t {
#define SSG_ENUMERATOR(symbol, ordinal) symbol = ordinal,
    SSG_SEMANTIC_SCROLL_TARGET_ENUMERATORS(SSG_ENUMERATOR)
#undef SSG_ENUMERATOR
};
#undef SSG_SEMANTIC_SCROLL_TARGET_ENUMERATORS

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

struct ResolvedSelectionRange {
    ByteOffset anchor;
    ByteOffset active;

    friend bool operator==(const ResolvedSelectionRange&,
                           const ResolvedSelectionRange&) = default;
};

struct ResolvedSelectionInput {
    SemanticInputBasis basis;
    TabId activeTab;
    Revision documentRevision;
    std::vector<ResolvedSelectionRange> selections;

    friend bool operator==(const ResolvedSelectionInput&,
                           const ResolvedSelectionInput&) = default;
};

using ClientInput =
    std::variant<ClientKeyInput, TabPointerInput, TreePointerInput,
                 PickerPointerInput, ExternalActionPointerInput,
                 NoticeActionPointerInput,
                 DocumentPointerInput, ScrollLinesInput,
                 ScrollFractionInput, ViewNavigationInput,
                 ResolvedPaneFocusInput, ResolvedSelectionInput>;

enum class ClientOwnedInputKind : std::uint8_t {
#define SSG_ENUMERATOR(symbol, ordinal) symbol = ordinal,
    SSG_CLIENT_OWNED_INPUT_KIND_ENUMERATORS(SSG_ENUMERATOR)
#undef SSG_ENUMERATOR
};
#undef SSG_CLIENT_OWNED_INPUT_KIND_ENUMERATORS

struct ClientOwnedInput {
    ClientOwnedInputKind kind;
    std::string text;

    friend bool operator==(const ClientOwnedInput&,
                           const ClientOwnedInput&) = default;
};

enum class ClientInputOutcome : std::uint8_t {
#define SSG_ENUMERATOR(symbol, ordinal) symbol = ordinal,
    SSG_CLIENT_INPUT_OUTCOME_ENUMERATORS(SSG_ENUMERATOR)
#undef SSG_ENUMERATOR
};
#undef SSG_CLIENT_INPUT_OUTCOME_ENUMERATORS

struct ClientInputResult {
    ClientInputOutcome outcome;
    std::optional<ClientOwnedInput> clientOwned;
    std::optional<CommandResult> command;
    std::optional<PickerActivation> pickerActivation;
};

}  // namespace ssg
