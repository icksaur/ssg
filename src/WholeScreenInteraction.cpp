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
    // Content shows exactly one of tabview/findresults: findresults when a picker is open,
    // else the document tab view.
    const bool pickerOpen = truth.openPicker.has_value();
    hidden.push_back(nodeId(pickerOpen ? kTabViewNodeId : kFindResultsNodeId));

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

    UiInteractionState state{std::move(schema), std::move(hidden)};

    // Base focus never strands on an absent panel: Panel is honored only when present.
    state.setBaseFocus(
        truth.baseFocus == BaseFocus::Panel && truth.panelPresent ? BaseFocus::Panel
                                                                  : BaseFocus::Editor);

    // A prompt anchors its focus capture on the node keystrokes route to: the header
    // input line itself for a Palette prompt (so the capture addresses the query
    // node, not merely its container), the footer prompt surface for a footer-region
    // prompt. Both hosts are present when addressed, so captureFocus admits the
    // capture.
    if (promptRegion) {
        const UiNodeId host = headerPrompt ? inputLine : footerPrompt;
        state.captureFocus(FocusCapture{host, FocusTarget::Prompt});
    }
    return state;
}

}  // namespace ssg
