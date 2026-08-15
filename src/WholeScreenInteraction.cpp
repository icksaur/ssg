#include <ssg/WholeScreenInteraction.h>

#include <ssg/UiTree.h>  // node id constants

#include <string>
#include <utility>
#include <vector>

namespace ssg {

namespace {

UiNodeId nodeId(std::string_view id) { return UiNodeId{std::string{id}}; }

// The node id of a panel provider surface. Only the three panel providers have panel
// nodes; any other surface has no panel node (the caller only passes panel providers).
std::string_view providerNodeId(ViewSurface provider) {
    switch (provider) {
    case ViewSurface::GitStatus:
        return kGitStatusNodeId;
    case ViewSurface::Symbols:
        return kSymbolsNodeId;
    case ViewSurface::FileTree:
    case ViewSurface::TabView:
    case ViewSurface::FindResults:
        break;
    }
    return kFileTreeNodeId;
}

}  // namespace

UiInteractionState buildWholeScreenInteraction(ValidatedSchema schema,
                                               const WholeScreenTruth& truth) {
    const std::string_view selected = providerNodeId(truth.selectedProvider);

    std::vector<UiNodeId> hidden;
    if (!truth.panelPresent) hidden.push_back(nodeId(kPanelNodeId));
    // Exactly one panel provider node is locally present -- the selected one; the other
    // two are hidden. The panel's own presence gates whether the selected one lays out.
    for (const std::string_view provider :
         {kFileTreeNodeId, kGitStatusNodeId, kSymbolsNodeId}) {
        if (provider != selected) hidden.push_back(nodeId(provider));
    }
    // Content shows exactly one of tabview/findresults: the finder when open, else the
    // document tab view.
    hidden.push_back(nodeId(truth.finderOpen ? kTabViewNodeId : kFindResultsNodeId));

    UiInteractionState state{std::move(schema), std::move(hidden)};

    // Base focus never strands on an absent panel: Panel is honored only when present.
    state.setBaseFocus(
        truth.baseFocus == BaseFocus::Panel && truth.panelPresent ? BaseFocus::Panel
                                                                  : BaseFocus::Editor);

    // The finder holds a prompt-backed focus capture on the findresults node while open;
    // captureFocus admits it only because findresults is present when the finder is open.
    if (truth.finderOpen) {
        state.captureFocus(
            FocusCapture{nodeId(kFindResultsNodeId), FocusTarget::Prompt});
    }
    return state;
}

}  // namespace ssg
