#include <ssg/EditorSessionImpl.h>
#include <ssg/CommandCatalog.h>
#include <ssg/PaletteSearcher.h>
#include <ssg/Theme.h>
#include <ssg/UiTree.h>

#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <variant>

namespace ssg {

namespace {

struct UiTreeValues {
    StatusFieldProjection status;
    std::string helpHintLabel;
    std::vector<StatusActionNode> statusActions;
};

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

UiNode* mutableNode(UiNode& node, const UiNodeId& id) {
    if (node.id == id) return &node;
    if (auto* container = std::get_if<UiContainer>(&node.content)) {
        for (auto& child : container->children) {
            if (auto* found = mutableNode(child, id)) return found;
        }
    }
    return nullptr;
}

UiNode& requireNode(UiSchema& schema, std::string_view id) {
    UiNode* node = mutableNode(schema.root, UiNodeId{std::string{id}});
    if (!node) {
        throw std::logic_error("populateUiTree: missing fixed node \"" +
                               std::string{id} + "\"");
    }
    return *node;
}

WidgetDescriptor& requireLeaf(UiNode& node, WidgetKind kind,
                              std::string_view id) {
    auto* leaf = std::get_if<UiLeaf>(&node.content);
    if (!leaf || leaf->widget.kind != kind) {
        throw std::logic_error("populateUiTree: node \"" + std::string{id} +
                               "\" is not the expected widget kind");
    }
    return leaf->widget;
}

UiContainer& requireContainer(UiNode& node, std::string_view id) {
    auto* container = std::get_if<UiContainer>(&node.content);
    if (!container) {
        throw std::logic_error("populateUiTree: node \"" + std::string{id} +
                               "\" is not a container");
    }
    return *container;
}

SemanticRole roleOr(const std::optional<std::string>& authored,
                    SemanticRole fallback) {
    if (authored) {
        if (const auto parsed = semanticRoleFromName(*authored)) return *parsed;
    }
    return fallback;
}

const StatusField* findStatusField(const std::vector<StatusField>& fields,
                                   std::string_view id) {
    for (const auto& field : fields) {
        if (field.id == id) return &field;
    }
    return nullptr;
}

void populateStatusField(UiSchema& schema, std::string_view nodeId,
                         const std::vector<StatusField>& fields,
                         std::string_view fieldId, SemanticRole defaultRole) {
    UiNode& node = requireNode(schema, nodeId);
    WidgetDescriptor& widget = requireLeaf(node, WidgetKind::Field, nodeId);
    const StatusField* field = findStatusField(fields, fieldId);
    if (!field || field->value.empty() || field->accessibleLabel.empty()) {
        node.resolved.reset();
        return;
    }
    node.resolved = UiLeafState{field->value, field->accessibleLabel,
                                field->commandId, std::nullopt,
                                roleOr(widget.role, defaultRole)};
}

void populateHelpHint(UiSchema& schema, const std::string& helpHintLabel) {
    UiNode& node = requireNode(schema, kFooterHintNodeId);
    WidgetDescriptor& widget =
        requireLeaf(node, WidgetKind::Field, kFooterHintNodeId);
    if (helpHintLabel.empty()) {
        node.resolved.reset();
        return;
    }
    node.resolved = UiLeafState{helpHintLabel, helpHintLabel, widget.command,
                                std::nullopt,
                                roleOr(widget.role, SemanticRole::Footer)};
}

void populateStatusActions(UiSchema& schema,
                           const std::vector<StatusActionNode>& actions) {
    UiNode& node = requireNode(schema, kFooterStatusActionsNodeId);
    UiContainer& container =
        requireContainer(node, kFooterStatusActionsNodeId);
    for (auto& child : container.children) {
        WidgetDescriptor& widget =
            requireLeaf(child, WidgetKind::Field, child.id.value());
        const auto found = std::find_if(
            actions.begin(), actions.end(), [&](const StatusActionNode& action) {
                return action.id == child.id;
            });
        if (found == actions.end() || found->accessibleLabel.empty()) {
            child.resolved.reset();
            continue;
        }
        child.resolved = UiLeafState{
            found->accessibleLabel, found->accessibleLabel,
            std::optional<std::string>{found->commandId}, std::nullopt,
            roleOr(widget.role, SemanticRole::StatusInfo)};
    }
}

WidgetKind expectedPromptControlKind(PromptControlKind kind) {
    switch (kind) {
    case PromptControlKind::Input: return WidgetKind::TextInput;
    case PromptControlKind::Toggle: return WidgetKind::Checkbox;
    case PromptControlKind::Count: return WidgetKind::Label;
    }
    throw std::logic_error("populateUiTree: unknown prompt control kind");
}

void populatePromptControls(UiSchema& schema,
                            const std::vector<PromptControl>& controls,
                            std::size_t activeInput) {
    std::size_t inputIndex = 0;
    for (const auto& control : controls) {
        const UiNodeId nodeId = footerPromptControlNodeId(control.id);
        UiNode& node = requireNode(schema, nodeId.value());
        WidgetDescriptor& widget = requireLeaf(
            node, expectedPromptControlKind(control.kind), nodeId.value());
        const SemanticRole role = roleOr(widget.role, SemanticRole::Prompt);
        const std::optional<std::string> command =
            control.command.empty()
                ? std::nullopt
                : std::optional<std::string>{control.command};
        switch (control.kind) {
        case PromptControlKind::Input: {
            const bool active = inputIndex == activeInput;
            ++inputIndex;
            if (control.accessibleLabel.empty()) {
                node.resolved.reset();
                break;
            }
            node.resolved = UiLeafState{control.value, control.accessibleLabel,
                                        command, std::nullopt, role,
                                        std::optional<bool>{active}};
            break;
        }
        case PromptControlKind::Toggle:
            node.resolved = UiLeafState{
                control.accessibleLabel, control.accessibleLabel, command,
                std::optional<bool>{control.checked}, role};
            break;
        case PromptControlKind::Count:
            if (control.value.empty() || control.accessibleLabel.empty()) {
                node.resolved.reset();
                break;
            }
            node.resolved = UiLeafState{control.value, control.accessibleLabel,
                                        command, std::nullopt, role};
            break;
        }
    }
}

void populateUiTree(UiSchema& schema, const UiTreeValues& values) {
    populateStatusField(schema, kHeaderPathFieldNodeId, values.status.header,
                        kPathStatusFieldId, SemanticRole::Header);
    populateStatusField(schema, kHeaderBranchFieldNodeId, values.status.header,
                        kBranchStatusFieldId, SemanticRole::Header);
    populateStatusField(schema, kFooterStatusFieldNodeId, values.status.footer,
                        kStatusValueFieldId, SemanticRole::Footer);
    populateStatusField(schema, kFooterFollowFieldNodeId, values.status.footer,
                        kFollowStatusFieldId, SemanticRole::Footer);
    populateHelpHint(schema, values.helpHintLabel);
    populateStatusActions(schema, values.statusActions);
}

} // namespace

std::optional<EditorSession::Impl::ResolvedPromptControls>
EditorSession::Impl::resolvedPromptControls() const {
    const auto& prompt = interaction.prompt();
    const auto& request = prompt.request();
    if (!request || promptFocusRegion(request->kind) != PromptRegion::Footer) {
        return std::nullopt;
    }
    ResolvedPromptControls resolved{
        resolvePromptControls(*request), prompt.activeInput()};
    if (request->kind != PromptKind::Find && request->kind != PromptKind::Replace) {
        return resolved;
    }
    const auto findState = findReplace.viewState();
    for (auto& control : resolved.controls) {
        switch (control.kind) {
        case PromptControlKind::Input:
            if (control.id == "find.query") {
                control.value = findState.query;
            } else if (control.id == "replace.replacement") {
                control.value = findState.replacement;
            }
            break;
        case PromptControlKind::Count: {
            const auto position =
                findState.activeMatch ? *findState.activeMatch + 1 : 0;
            control.value = std::to_string(position) + "/" +
                            std::to_string(findState.matches.size());
            break;
        }
        case PromptControlKind::Toggle:
            if (control.id == "find.toggle_case") {
                control.checked = findState.options.caseSensitive;
            } else if (control.id == "find.toggle_whole_word") {
                control.checked = findState.options.wholeWord;
            } else if (control.id == "find.toggle_regex") {
                control.checked = findState.options.regex;
            }
            break;
        }
    }
    return resolved;
}

PromptStatusViewState EditorSession::Impl::promptStatusView() const {
    // Semantic prompt state: which prompt is open (authoritative, present even for
    // a header-hosted prompt with no footer view) and the status bar. No
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
    auto statusText = status.footerText();
    auto followProjection = follow.footerProjection();
    auto fields = projectStatusFields(
        {.workspaceRoot = root,
         .homeDirectory = homeDirectory,
         .currentBranch = currentGitBranch,
         .statusValue = statusText,
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
    populateUiTree(
        uiTree, UiTreeValues{
                    uiStatusFields(), helpHintLabel(keymap),
                    interaction.statusActions()});
    if (auto prompt = resolvedPromptControls()) {
        populatePromptControls(uiTree, prompt->controls, prompt->activeInput);
    }
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
    auto const catalogRevision = catalog.revision();
    if (!commandCandidateCacheValid ||
        catalogRevision != commandCandidateCatalogRevision ||
        keymap != commandCandidateKeymap) {
        commandCandidateCache.clear();
        for (auto const* command : catalog.commands()) {
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
