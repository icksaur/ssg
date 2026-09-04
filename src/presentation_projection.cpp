#include <ssg/editor_session_internal.h>
#include <ssg/prompt_resolution.h>
#include <ssg/ui_tree_population.h>
#include <ssg/CommandCatalog.h>
#include <ssg/PaletteSearcher.h>
#include <ssg/UiTree.h>

#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <variant>

namespace ssg {

namespace {

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
    if (fieldId == kPathStatusFieldId) return std::string{"panel.show_files"};
    if (fieldId == kBranchStatusFieldId) {
        return std::string{"panel.show_git_status"};
    }
    if (fieldId == kFollowStatusFieldId) return followProjection.resumeCommand;
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

PromptStatusViewState EditorSession::Impl::promptStatusView() const {
    // Semantic prompt state: which prompt is open (authoritative, present even for
    // a header-hosted prompt with no footer view) and the status queue. No
    // dimensions are needed to resolve this semantic state.
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

StatusFieldProjection EditorSession::Impl::uiStatusFields() const {
    auto statusProjection = status.footerProjection();
    auto followProjection = follow.footerProjection();
    auto fields = projectStatusFields(
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

UiSchema EditorSession::Impl::projectedUiTree() const {
    // Population writes directly into a copy of the same single UiSchema the
    // interaction authority owns, so the published tree's visibility and
    // resolved values correspond node-for-node.
    UiSchema uiTree = interaction.schema();
    detail::populateUiTree(
        uiTree, detail::UiTreeValues{
                    uiStatusFields(), helpHintLabel(keymap),
                    interaction.statusActions(),
                    detail::resolveRuntimePromptControls(
                        interaction.prompt(), findReplace.viewState())});
    uiTree.focusPath = interaction.focusPath();
    return requirePublishedUiTree(std::move(uiTree));
}

TreeViewState EditorSession::Impl::treeView() const {
    // Semantic only: providers, nodes, selection, and expansion.
    return tree.viewState();
}

PaletteViewState EditorSession::Impl::paletteView() const {
    PaletteViewState view;
    view.presenceOverlay = interaction.pickerPresenceOverlay();
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
