#include <ssg/WholeScreenInteraction.h>

#include <ssg/UiTree.h>  // node id constants

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ssg {

namespace {

UiNodeId nodeId(std::string_view id) { return UiNodeId{std::string{id}}; }

// The node id of a panel provider. The domain is closed; a corrupt enumerator is rejected
// rather than coerced to a plausible leaf, so invalid truth cannot produce valid presence.
std::string_view providerNodeId(PanelProvider provider) {
    switch (provider) {
    case PanelProvider::FileTree:
        return kFileTreeNodeId;
    case PanelProvider::GitStatus:
        return kGitStatusNodeId;
    case PanelProvider::Symbols:
        return kSymbolsNodeId;
    }
    throw std::logic_error("corrupt PanelProvider enumerator");
}

}  // namespace

UiInteractionState buildWholeScreenInteraction(ValidatedSchema schema,
                                               const WholeScreenTruth& truth,
                                               std::optional<PromptRegion> promptRegion) {
    // The provider node is present only when the panel is; provider choice is carried by
    // the separate last-active hint, never leaked into presence.
    const std::string_view selected =
        truth.panelPresent ? providerNodeId(truth.selectedProvider) : std::string_view{};

    std::vector<UiNodeId> hidden;
    if (!truth.panelPresent) hidden.push_back(nodeId(kPanelNodeId));
    for (const std::string_view provider :
         {kFileTreeNodeId, kGitStatusNodeId, kSymbolsNodeId}) {
        if (provider != selected) hidden.push_back(nodeId(provider));
    }
    if (truth.distractionFree) {
        for (const std::string_view region :
             {kNoticeNodeId, kExternalModNodeId, kPanelNodeId, kTabBarNodeId,
              kFooterNodeId}) {
            hidden.push_back(nodeId(region));
        }
        if (promptRegion != PromptRegion::Header)
            hidden.push_back(nodeId(kHeaderNodeId));
        if (promptRegion != PromptRegion::Footer)
            hidden.push_back(nodeId(kFooterPromptNodeId));
    }
    // Content shows exactly one of the editor/find-results branches.
    const bool pickerOpen = truth.openPicker.has_value();
    hidden.push_back(
        nodeId(pickerOpen ? kEditorNodeId : kFindResultsViewportNodeId));

    // The header prompt input is present only while a header-region prompt is open,
    // so a client draws its query line exactly when the picker is up. The
    // whole-screen schema always assembles the node; its absence under a header
    // prompt means the schema contract broke and must not be masked by focusing the
    // header container.
    const UiNodeId inputLine = nodeId(kHeaderPromptInputNodeId);
    const bool headerPrompt = promptRegion && *promptRegion == PromptRegion::Header;
    const bool hasInputLine = schema.contains(inputLine);
    if (headerPrompt && !hasInputLine) {
        throw std::logic_error("header prompt requires the input_line node");
    }
    if (hasInputLine && !headerPrompt) hidden.push_back(inputLine);

    // The footer prompt surface mirrors the header input line: always assembled,
    // present only while a footer-region prompt is open, so a native client draws
    // and drives the prompt exactly then. Its absence under a footer prompt means
    // the schema contract broke and must not be masked by focusing the footer.
    const UiNodeId footerPrompt = nodeId(kFooterPromptNodeId);
    const bool footerPromptOpen =
        promptRegion && *promptRegion == PromptRegion::Footer;
    const bool hasFooterPrompt = schema.contains(footerPrompt);
    if (footerPromptOpen && !hasFooterPrompt) {
        throw std::logic_error("footer prompt requires the footer.prompt node");
    }
    if (hasFooterPrompt && !footerPromptOpen) hidden.push_back(footerPrompt);

    // The notice region mirrors the header input line and footer prompt: always
    // assembled, present only while the active document raises a draft-conflict
    // notice, so a client draws the notice bar exactly then. Its absence under a
    // raised notice means the schema contract broke and must not be masked. Unlike
    // the prompt, the notice captures no focus -- its actions are click/command
    // triggers routed like any other command.
    const UiNodeId notice = nodeId(kNoticeNodeId);
    const bool hasNotice = schema.contains(notice);
    if (truth.noticePresent && !hasNotice) {
        throw std::logic_error("draft notice requires the notice node");
    }
    if (hasNotice && !truth.noticePresent) hidden.push_back(notice);

    // The external-modification node mirrors the notice: always assembled, present
    // only while a file is externally changed. Its absence under a raised section
    // means the schema contract broke and must not be masked. Unlike the notice, it
    // CAN capture focus -- but only through the explicit external.focus command
    // (truth.externalFocusHeld), never reactively.
    const UiNodeId externalMod = nodeId(kExternalModNodeId);
    const bool hasExternalMod = schema.contains(externalMod);
    if (truth.externalModificationPresent && !hasExternalMod) {
        throw std::logic_error(
            "external modification section requires the externalmod node");
    }
    if (hasExternalMod && !truth.externalModificationPresent) {
        hidden.push_back(externalMod);
    }

    UiInteractionState state{std::move(schema), std::move(hidden)};

    // Base focus never strands on an absent panel: Panel is honored only when present.
    state.setBaseFocus(
        truth.baseFocus == BaseFocus::Panel && truth.panelPresent &&
                !truth.distractionFree
            ? BaseFocus::Panel
            : BaseFocus::Editor);

    // A prompt anchors its focus capture on the node keystrokes route to: the header
    // input line itself for a Palette prompt (so the capture addresses the query
    // node, not merely its container), the footer prompt surface for a footer-region
    // prompt. Both hosts are present when addressed, so captureFocus admits the
    // capture.
    // The external-modification capture is DERIVED from truth each rebuild (pushed
    // only when the bar is present and the user has focused it), so it survives
    // unrelated rebuilds and is never stacked twice. Pushed BEFORE any prompt
    // capture, so when both are held the LIFO top -- and thus the active context --
    // is the prompt, and dismissing the prompt returns to the external context.
    if (truth.externalModificationPresent && hasExternalMod &&
        truth.externalFocusHeld) {
        state.captureFocus(
            FocusCapture{externalMod, FocusTarget::ExternalModification});
    }

    if (promptRegion) {
        const UiNodeId host = headerPrompt ? inputLine : footerPrompt;
        state.captureFocus(FocusCapture{host, FocusTarget::Prompt});
    }
    return state;
}

PalettePresenceOverlay derivePickerPresenceOverlay(
    const ValidatedSchema& schema, const WholeScreenTruth& truth) {
    WholeScreenTruth closed = truth;
    closed.openPicker.reset();
    WholeScreenTruth open = closed;
    open.openPicker = PickerKind::Command;

    const auto closedState =
        buildWholeScreenInteraction(schema, closed, std::nullopt);
    const auto openState =
        buildWholeScreenInteraction(schema, open, PromptRegion::Header);

    PalettePresenceOverlay overlay;
    overlay.generation = schema.generation();
    for (const UiNodeId& id : schema.nodeIds()) {
        const bool before = closedState.presence().isPresent(id);
        const bool after = openState.presence().isPresent(id);
        if (before == after) continue;
        overlay.ops.push_back(
            {after ? PalettePresenceOpKind::Show
                   : PalettePresenceOpKind::Hide,
             id});
    }
    return overlay;
}

}  // namespace ssg
