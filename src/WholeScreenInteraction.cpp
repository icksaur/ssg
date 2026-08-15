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
                                               const WholeScreenTruth& truth) {
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
    // Content shows exactly one of tabview/findresults: the finder when a picker is
    // open, else the document tab view.
    const bool finderOpen = truth.openPicker.has_value();
    hidden.push_back(nodeId(finderOpen ? kTabViewNodeId : kFindResultsNodeId));

    UiInteractionState state{std::move(schema), std::move(hidden)};

    // Base focus never strands on an absent panel: Panel is honored only when present.
    state.setBaseFocus(
        truth.baseFocus == BaseFocus::Panel && truth.panelPresent ? BaseFocus::Panel
                                                                  : BaseFocus::Editor);

    // The finder holds a prompt-backed focus capture on the findresults node while a
    // picker is open; captureFocus admits it only because findresults is present then.
    if (finderOpen) {
        state.captureFocus(
            FocusCapture{nodeId(kFindResultsNodeId), FocusTarget::Prompt});
    }
    return state;
}

}  // namespace ssg
