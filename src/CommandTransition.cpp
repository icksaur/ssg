#include <ssg/CommandTransition.h>

#include <algorithm>
#include <utility>

namespace ssg {

namespace {

// The provider cycle order -- the order next/previous walks. Matches the panel provider
// order the shell is constructed with.
constexpr std::array<PanelProvider, 3> kCycle{
    PanelProvider::FileTree, PanelProvider::GitStatus, PanelProvider::Symbols};

UiInteractionState buildFor(const WholeScreenTruth& truth,
                            const ValidatedSchema& schema) {
    return buildWholeScreenInteraction(schema, truth);
}

bool alreadyPresent(const std::vector<TreeProviderId>& present,
                    const TreeProviderId& id) {
    return std::find(present.begin(), present.end(), id) != present.end();
}

}  // namespace

std::string_view panelProviderLabel(PanelProvider provider) {
    switch (provider) {
    case PanelProvider::GitStatus:
        return "git";
    case PanelProvider::Symbols:
        return "symbols";
    case PanelProvider::FileTree:
        break;
    }
    return "files";
}

TreeProviderBinding panelProviderTreeBinding(PanelProvider provider) {
    switch (provider) {
    case PanelProvider::GitStatus:
        return TreeProviderBinding{TreeProviderId{"git"}, TreeProviderKind::Git};
    case PanelProvider::Symbols:
        return TreeProviderBinding{TreeProviderId{"symbols"},
                                   TreeProviderKind::Symbols};
    case PanelProvider::FileTree:
        break;
    }
    return TreeProviderBinding{TreeProviderId{"filesystem"},
                               TreeProviderKind::Filesystem};
}

PanelProvider cyclePanelProvider(PanelProvider provider, CycleDirection direction) {
    const auto at = std::find(kCycle.begin(), kCycle.end(), provider);
    const std::size_t index = static_cast<std::size_t>(at - kCycle.begin());
    const std::size_t step = direction == CycleDirection::Next ? 1 : kCycle.size() - 1;
    return kCycle[(index + step) % kCycle.size()];
}

namespace {

// Toggle the panel closed, restoring the retained panel-return focus. Shared by the
// panel.toggle-while-shown and the reselect-same-provider-while-shown paths.
PreparedTransition hidePanel(const WholeScreenTruth& truth,
                             const TransitionInputs& inputs) {
    WholeScreenTruth next = truth;
    next.panelPresent = false;
    next.baseFocus = truth.panelReturnFocus;
    return PreparedTransition{next, buildFor(next, inputs.schema), inputs.prompt,
                              std::nullopt, false};
}

std::optional<PreparedTransition> prepare(TogglePanel, const TransitionInputs& inputs) {
    const WholeScreenTruth& truth = inputs.truth;
    if (truth.panelPresent) return hidePanel(truth, inputs);
    WholeScreenTruth next = truth;
    next.panelReturnFocus = truth.baseFocus;
    next.panelPresent = true;
    next.baseFocus = BaseFocus::Panel;
    return PreparedTransition{next, buildFor(next, inputs.schema), inputs.prompt,
                              std::nullopt, false};
}

std::optional<PreparedTransition> prepare(ShowPanelProvider request,
                                          const TransitionInputs& inputs) {
    const WholeScreenTruth& truth = inputs.truth;
    // Reselecting the shown provider hides the panel -- part of this command's semantics.
    if (truth.panelPresent && truth.selectedProvider == request.provider) {
        return hidePanel(truth, inputs);
    }

    // Fully prepare the tree backing so commit is infallible: activate an existing
    // provider, or build the snapshot for a creatable (Git/Symbols) one. A missing
    // Filesystem provider is a genuine rejection -- it is seeded, never created here.
    const TreeProviderBinding binding = panelProviderTreeBinding(request.provider);
    TreeBackingPlan plan{binding.id, std::nullopt};
    if (!alreadyPresent(inputs.presentProviders, binding.id)) {
        if (binding.kind == TreeProviderKind::Filesystem) return std::nullopt;
        plan.create = TreeProviderSnapshot{binding.id, binding.kind,
                                           inputs.nextTreeRevision, {}};
    }

    WholeScreenTruth next = truth;
    if (!truth.panelPresent) next.panelReturnFocus = truth.baseFocus;
    next.panelPresent = true;
    next.selectedProvider = request.provider;
    next.baseFocus = BaseFocus::Panel;
    return PreparedTransition{next, buildFor(next, inputs.schema), inputs.prompt,
                              std::move(plan), false};
}

std::optional<PreparedTransition> prepare(OpenFinder request,
                                          const TransitionInputs& inputs) {
    const PickerDescriptor* descriptor = pickerCatalog().find(request.picker);
    if (descriptor == nullptr) return std::nullopt;

    // Apply the open to a COPY of the prompt for parity: the copy IS the committed prompt
    // state, so the commit's prompt install is a move, not a re-run of open. Opening over
    // an already-active prompt replaces it (as the live opener does); no conflicting-prompt
    // rejection is introduced.
    PromptSurface prompt = inputs.prompt;
    const PromptCommandResult opened = prompt.open(PromptRequest{
        PromptKind::Palette, std::string{descriptor->promptTitle},
        {{"query", "command palette query", ""}}, {}, std::nullopt});
    if (!opened.accepted()) return std::nullopt;

    WholeScreenTruth next = inputs.truth;
    next.openPicker = request.picker;
    return PreparedTransition{next, buildFor(next, inputs.schema), std::move(prompt),
                              std::nullopt, request.picker == PickerKind::File};
}

std::optional<PreparedTransition> prepare(CloseFinder, const TransitionInputs& inputs) {
    // Cancel on a copy: cancel is not inherently infallible, so applying it to the copy
    // captures the real resulting prompt state for the commit to swap in.
    PromptSurface prompt = inputs.prompt;
    (void)prompt.cancel();
    WholeScreenTruth next = inputs.truth;
    next.openPicker = std::nullopt;
    return PreparedTransition{next, buildFor(next, inputs.schema), std::move(prompt),
                              std::nullopt, false};
}

}  // namespace

std::optional<PreparedTransition> prepareTransition(const CommandTransition& transition,
                                                    const TransitionInputs& inputs) {
    return std::visit([&](auto request) { return prepare(request, inputs); }, transition);
}

}  // namespace ssg
