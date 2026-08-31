#include "editor_session_internal.h"
#include "prompt_resolution.h"
#include "../grid_projection_state.h"

#include <ssg/CommandCatalog.h>

#include <ssg/CommandCatalog.h>
#include <ssg/ChromeLowering.h>
#include <ssg/Layout.h>
#include <ssg/PaletteSearcher.h>
#include <ssg/UiTree.h>

#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <variant>

namespace ssg {

namespace {

// A resolver from projected status fields: id -> (value, label, command). Shared by
// the grid lowering and the semantic dynamic-state resolution so a composed
// provider widget resolves to the same values on either path.
ChromeProviderResolver chromeResolverFor(std::vector<StatusField> header,
                                         std::vector<StatusField> footer,
                                         std::string helpLabel,
                                         std::vector<StatusActionNode> statusActions,
                                         std::optional<detail::ResolvedPromptControls> prompt =
                                             std::nullopt) {
    return [header = std::move(header), footer = std::move(footer),
            helpLabel = std::move(helpLabel),
            statusActions = std::move(statusActions),
            prompt = std::move(prompt)](
               std::string_view id) -> std::optional<ResolvedProvider> {
        if (id == "footer.hint") {
            return ResolvedProvider{helpLabel, helpLabel, std::string{"help.open"}};
        }
        for (const auto* group : {&header, &footer}) {
            for (const auto& field : *group) {
                if (field.id == id) {
                    return ResolvedProvider{field.value, field.accessibleLabel,
                                            field.commandId};
                }
            }
        }
        for (const auto& action : statusActions) {
            if (action.id.value() == id) {
                return ResolvedProvider{action.accessibleLabel,
                                        action.accessibleLabel,
                                        action.commandId};
            }
        }
        if (prompt) {
            std::size_t inputIndex = 0;
            for (const auto& control : prompt->controls) {
                if (footerPromptControlNodeId(control.id).value() == id) {
                    const bool isInput =
                        control.kind == PromptControlKind::Input;
                    const std::string value =
                        control.kind == PromptControlKind::Toggle
                            ? (control.checked ? "true" : "false")
                            : control.value;
                    return ResolvedProvider{
                        value, control.accessibleLabel,
                        control.command.empty()
                            ? std::nullopt
                            : std::optional<std::string>{control.command},
                        isInput ? std::optional<bool>{
                                      inputIndex == prompt->activeInput}
                                : std::nullopt};
                }
                if (control.kind == PromptControlKind::Input) ++inputIndex;
            }
        }
        return std::nullopt;
    };
}

std::string helpHintLabel(const KeymapViewState& keymap) {
    std::string keys;
    if (auto sequence = KeymapMatcher{keymap}.preferredBinding("help.open")) {
        keys = KeyCodec{}.formatSequence(*sequence);
    }
    return keys.empty() ? std::string{"help"} : keys + "  help";
}

std::optional<std::string> statusFieldCommandId(
    std::string_view fieldId,
    const FollowEditsFooterProjection& followProjection) {
    if (fieldId == "path") return std::string{"panel.show_files"};
    if (fieldId == "branch") return std::string{"panel.show_git_status"};
    if (fieldId == "follow") return followProjection.resumeCommand;
    return std::nullopt;
}

void bindStatusFieldCommands(std::vector<StatusField>& fields,
                             const FollowEditsFooterProjection& followProjection) {
    for (auto& field : fields) {
        field.commandId = statusFieldCommandId(field.id, followProjection);
    }
}

const UiNode* uiNodeById(const UiNode& node, std::string_view id) {
    if (node.id.value() == id) return &node;
    if (const auto* container = std::get_if<UiContainer>(&node.content)) {
        for (const auto& child : container->children) {
            if (const auto* found = uiNodeById(child, id)) return found;
        }
    }
    return nullptr;
}

} // namespace

DocumentViewState EditorSession::Impl::documentView() const {
    auto const* document = activeDocument();
    if (document == nullptr) return {Revision{0}, {}, ByteOffset{0}, std::nullopt};
    auto snapshot = document->snapshot();
    auto publishedRevision = snapshot.revision;
    auto caret = selection.selections.primary().active.byteOffset;
    if (caret.value() > snapshot.text.size()) caret = ByteOffset{snapshot.text.size()};
    std::optional<std::string> diffFileIdentity;
    if (const auto* tab = activeTabState();
        tab && tab->kind == TabKind::LiveDiff &&
        !tab->contentIdentity.empty()) {
        diffFileIdentity = tab->contentIdentity;
        publishedRevision = diff.viewState().revision;
    }
    return {publishedRevision, std::move(snapshot.text), caret,
            std::move(diffFileIdentity)};
}

TextEncodingViewState EditorSession::Impl::textEncodingView() const {
    auto state = activeWorkspaceState();
    return {state ? state->encoding : TextEncodingStatus{}};
}

PromptStatusViewState EditorSession::Impl::promptStatusView() const {
    // Semantic prompt state: which prompt is open (authoritative, present even for
    // a header-hosted prompt with no footer view) and the status queue. No
    // dimensions needed, so a semantic snapshot is obtainable without geometry.
    PromptStatusViewState view;
    if (interaction.prompt().request()) view.activeKind = interaction.prompt().request()->kind;
    view.status = status.viewState();
    return view;
}

std::optional<NoticeView> EditorSession::Impl::draftNotice() const {
    // Only the Conflict outcome raises the notice; a Restored draft is a quieter
    // state with no external change to resolve. The action command ids are already
    // registered; the host only dispatches them.
    const auto id = activeDocumentId();
    if (!id) return std::nullopt;
    const auto found = documentRuntimeStates.find(id->value());
    if (found == documentRuntimeStates.end() ||
        found->second.reopen != DraftReopenOutcome::Conflict) {
        return std::nullopt;
    }
    return NoticeView{
        "Unsaved draft: file changed on disk externally.",
        {{"draft.notice.diff", "diff", "draft.diff"},
         {"draft.notice.use_disk", "use disk", "draft.discard"},
         {"draft.notice.dismiss", "dismiss", "draft.dismiss"}}};
}

std::optional<NoticeView> EditorSession::Impl::noticeView() const {
    return draftNotice();
}

bool EditorSession::Impl::noticePresent() const {
    return draftNotice().has_value();
}

StatusFieldProjection EditorSession::Impl::chromeStatusFields() const {
    auto statusProjection = status.footerProjection();
    auto followProjection = follow.footerProjection();
    auto fields = projectStatusFields(
        statusFieldCatalog, statusFieldProviders,
        {.workspaceRoot = root,
         .homeDirectory = homeDirectory,
         .currentBranch = currentGitBranch,
         .statusValue = statusProjection.value,
         .followMode = followProjection.mode,
         .cwdPrefix = {}});
    bindStatusFieldCommands(fields.header, followProjection);
    bindStatusFieldCommands(fields.footer, followProjection);
    return fields;
}

SessionSnapshotSections EditorSession::Impl::sections(
    PaletteReport const& paletteReport) const {
    auto currentHistory = HistoryViewState{false, false, 0};
    if (auto id = activeDocumentId()) {
        auto found = documentRuntimeStates.find(id->value());
        if (found != documentRuntimeStates.end()) {
            currentHistory = found->second.history.viewState();
        }
    }
    auto treeSection = treeView();
    const UiInteractionState& interactionState = interaction.interaction();
    const ValidatedSchema& validatedSchema = interactionState.schema();
    UiSchema uiSchema = validatedSchema.schema();
    // Schema, resolved state, and presence all derive from the interaction
    // authority's single ValidatedSchema, so they correspond node-for-node and
    // share one generation.
    if (!validateWellKnownAreas(uiSchema).ok()) {
        throw std::logic_error(
            "resolveUiState: composed schema violates the well-known-area contract");
    }
    UiStateSection uiState = [&] {
        auto fields = chromeStatusFields();
        return resolveUiState(
            validatedSchema, chromeResolverFor(std::move(fields.header),
                                               std::move(fields.footer),
                                               helpHintLabel(keymap),
                                               interaction.statusActions(),
                                               detail::resolveRuntimePromptControls(
                                                   interaction.prompt(),
                                                   findReplace.viewState())));
    }();
    uiState.focusPath = interactionState.focusPath();
    UiPresenceSection uiPresence =
        buildPresenceSection(validatedSchema, interactionState.presence());
    uiPresence.basis = PresenceBasis{session->revision().value()};
    return {documentView(),
            selection.selections,
            currentHistory,
            clipboard.viewState(),
            promptStatusView(),
            search.viewState(),
            findReplace.viewState(),
            settings.viewState(),
            keymap,
            textEncodingView(),
            tabs.viewState(),
            diff.viewState(),
            external.viewState(),
            follow.viewState(),
            std::move(treeSection),
            activeSyntaxView(),
            lspSync,
            lspFeatures,
            theme,
            paletteView(),
            UiFrame::require(std::move(uiSchema), std::move(uiState),
                             std::move(uiPresence)),
            noticeView(),
            watcherAvailable.load(std::memory_order_relaxed)};
}

TreeViewState EditorSession::Impl::treeView() const {
    // Semantic only: providers, nodes, selection, and expansion.
    return tree.viewState();
}

PaletteViewState EditorSession::Impl::paletteView() const {
    PaletteViewState view;
    view.presenceOverlay = derivePickerPresenceOverlay(
        interaction.interaction().schema(), interaction.truth());
    if (auto open = interaction.openPicker()) {
        if (auto const* descriptor = pickerCatalog().find(*open)) {
            view.activePicker = interaction.openPickerActivation();
        }
    }
    // Every registered command is published continuously. Resolving each key hint
    // is O(bindings x commands), so cache until the catalog or keymap changes.
    auto const catalogRevision = session->catalog()->revision();
    if (!commandCandidateCacheValid ||
        catalogRevision != commandCandidateCatalogRevision ||
        keymap != commandCandidateKeymap) {
        commandCandidateCache.clear();
        for (auto const* command : session->catalog()->commands()) {
            std::string detail;
            if (auto sequence =
                    KeymapMatcher{keymap}.preferredBinding(command->id)) {
                detail = KeyCodec{}.formatSequence(*sequence);
                }
            commandCandidateCache.push_back(
                {command->id, command->displayLabel(), std::move(detail)});
        }
        commandCandidateCatalogRevision = catalogRevision;
        commandCandidateKeymap = keymap;
        commandCandidateCacheValid = true;
    }
    view.commandCandidates = commandCandidateCache;
    view.fileCandidates = fileCandidates;
    return view;
}

} // namespace ssg
