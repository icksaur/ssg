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

    UiInteractionState state{std::move(schema), std::move(hidden)};

    // Base focus never strands on an absent panel: Panel is honored only when present.
    state.setBaseFocus(
        truth.baseFocus == BaseFocus::Panel && truth.panelPresent ? BaseFocus::Panel
                                                                  : BaseFocus::Editor);

    // A prompt anchors its focus capture on the region's host node -- the header input line
    // for a Palette prompt, the footer otherwise -- so keystrokes route there. Both hosts
    // are always present, so captureFocus admits the capture.
    if (promptRegion) {
        const std::string_view host = *promptRegion == PromptRegion::Header
                                          ? kHeaderNodeId
                                          : kFooterNodeId;
        state.captureFocus(FocusCapture{nodeId(host), FocusTarget::Prompt});
    }
    return state;
}

}  // namespace ssg
