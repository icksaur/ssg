#include <ssg/CommandTransition.h>

#include "runtime/transition_internals.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace ssg {

namespace {

const std::array<TreeProviderBinding, 3> kPanelTreeProviders{
    TreeProviderBinding{TreeProviderId{"filesystem"},
                        TreeProviderKind::Filesystem},
    TreeProviderBinding{TreeProviderId{"git"}, TreeProviderKind::Git},
    TreeProviderBinding{TreeProviderId{"symbols"}, TreeProviderKind::Symbols},
};

[[noreturn]] void rejectCorrupt(const char* what) {
    throw std::logic_error(what);
}

bool isBuiltInPanelTreeProvider(const TreeProviderBinding& binding) {
    return std::ranges::find(kPanelTreeProviders, binding) !=
           kPanelTreeProviders.end();
}

}  // namespace

std::span<const TreeProviderBinding> builtInPanelTreeProviders() {
    return kPanelTreeProviders;
}

TreeProviderBinding builtInPanelTreeProvider(TreeProviderKind kind) {
    const auto found = std::ranges::find(
        kPanelTreeProviders, kind, &TreeProviderBinding::kind);
    if (found == kPanelTreeProviders.end()) {
        rejectCorrupt("corrupt TreeProviderKind enumerator");
    }
    return *found;
}

TreeProviderBinding cyclePanelTreeProvider(
    const TreeProviderBinding& provider, CycleDirection direction) {
    const auto at = std::ranges::find(kPanelTreeProviders, provider);
    if (at == kPanelTreeProviders.end()) {
        rejectCorrupt("active tree provider is outside the panel cycle");
    }
    std::size_t step = 0;
    switch (direction) {
    case CycleDirection::Next:
        step = 1;
        break;
    case CycleDirection::Previous:
        step = kPanelTreeProviders.size() - 1;
        break;
    default:
        rejectCorrupt("corrupt CycleDirection enumerator");
    }
    const std::size_t index =
        static_cast<std::size_t>(at - kPanelTreeProviders.begin());
    return kPanelTreeProviders[
        (index + step) % kPanelTreeProviders.size()];
}

std::optional<PromptRegion> activePromptRegion(const PromptSurface& prompt) {
    if (!prompt.active()) return std::nullopt;
    return promptFocusRegion(prompt.request()->kind);
}

void PreparedTransition::installInto(WholeScreenTruth& truth,
                                     UiInteractionState& interaction,
                                     PromptSurface& prompt, TreeModel& tree,
                                     std::uint64_t& revisionSource) && {
    if (tree_) {
        if (tree_->create) {
            const std::uint64_t consumed = tree_->create->revision().value();
            tree.replaceProvider(std::move(*tree_->create));
            // Advance the single revision source past the consumed create revision;
            // preflight rejected exhaustion, so consumed + 1 does not overflow.
            revisionSource = std::max(revisionSource, consumed + 1);
        }
        (void)tree.activateProvider(tree_->activate);
    }
    prompt = std::move(prompt_);
    interaction = std::move(interaction_);
    truth = std::move(truth_);
}

// The per-variant preflight logic, friended so it is the sole constructor of a
// PreparedTransition; a caller can only obtain one through prepareTransition.
struct TransitionBuilder {
    static PreparedTransition make(WholeScreenTruth truth,
                                   const ValidatedSchema& schema, PromptSurface prompt,
                                   std::optional<TreeBackingPlan> tree) {
        // The prompt-focus region is derived from the result prompt, never stored in truth.
        const std::optional<PromptRegion> region = activePromptRegion(prompt);
        UiInteractionState interaction =
            buildWholeScreenInteraction(schema, truth, region);
        return PreparedTransition{std::move(truth), std::move(interaction),
                                  std::move(prompt), std::move(tree)};
    }

    // Toggle the panel closed, restoring the retained panel-return focus. Shared by
    // panel.toggle-while-shown and reselect-same-provider-while-shown.
    static PreparedTransition hidePanel(const WholeScreenTruth& truth,
                                        const TransitionInputs& inputs) {
        WholeScreenTruth next = truth;
        next.panelPresent = false;
        next.baseFocus = truth.panelReturnFocus;
        return make(std::move(next), inputs.schema, inputs.prompt, std::nullopt);
    }

    static std::optional<PreparedTransition> prepare(TogglePanel,
                                                     const TransitionInputs& inputs) {
        const WholeScreenTruth& truth = inputs.truth;
        if (truth.panelPresent) return hidePanel(truth, inputs);
        WholeScreenTruth next = truth;
        next.panelReturnFocus = truth.baseFocus;
        next.panelPresent = true;
        next.baseFocus = BaseFocus::Panel;
        return make(std::move(next), inputs.schema, inputs.prompt, std::nullopt);
    }

    // Fully prepare the tree backing for `provider` so commit is infallible, or signal
    // rejection. Match id AND kind: an id present under the wrong kind is a corrupt
    // binding and is rejected, never activated or replaced. An absent Filesystem provider
    // is also a genuine rejection -- it is seeded with real nodes, never created empty.
    // A new snapshot's revision comes from the single revision source, never
    // invented as existing+1. Preflight rejects exhaustion so installing the
    // prepared snapshot and advancing the source remain infallible.
    static bool prepareTreeBacking(const TreeProviderBinding& binding,
                                   const TransitionInputs& inputs,
                                   std::optional<TreeBackingPlan>& out) {
        if (!isBuiltInPanelTreeProvider(binding)) return false;
        const auto existing = std::find_if(
            inputs.presentProviders.begin(), inputs.presentProviders.end(),
            [&](const TreeProviderPresence& p) { return p.binding.id == binding.id; });
        const bool matching = existing != inputs.presentProviders.end() &&
                              existing->binding.kind == binding.kind;
        TreeBackingPlan plan{binding.id, std::nullopt};
        if (!matching) {
            if (existing != inputs.presentProviders.end()) return false;
            if (!treeProviderCanBeCreatedEmpty(binding.kind)) return false;
            const std::uint64_t next = inputs.nextTreeRevision.value();
            if (next == std::numeric_limits<std::uint64_t>::max()) return false;
            plan.create =
                TreeProviderSnapshot{binding.id, binding.kind, inputs.nextTreeRevision, {}};
        }
        out = std::move(plan);
        return true;
    }

    static std::optional<PreparedTransition> prepare(ShowPanelProvider request,
                                                     const TransitionInputs& inputs) {
        const WholeScreenTruth& truth = inputs.truth;
        const TreeProviderBinding& binding = request.binding;
        if (!isBuiltInPanelTreeProvider(binding)) return std::nullopt;
        // Reselecting the shown provider hides the panel -- this command's semantics.
        if (truth.panelPresent && inputs.activeProvider == binding) {
            return hidePanel(truth, inputs);
        }

        std::optional<TreeBackingPlan> plan;
        if (!prepareTreeBacking(binding, inputs, plan)) return std::nullopt;

        WholeScreenTruth next = truth;
        if (!truth.panelPresent) next.panelReturnFocus = truth.baseFocus;
        next.panelPresent = true;
        next.baseFocus = BaseFocus::Panel;
        return make(std::move(next), inputs.schema, inputs.prompt, std::move(plan));
    }

    static std::optional<PreparedTransition> prepare(SwitchPanelProvider request,
                                                     const TransitionInputs& inputs) {
        // Change the provider backing only; panel visibility and focus are preserved (this
        // is cycling next/previous, not a show). Never toggles off on reselect.
        const TreeProviderBinding& binding = request.binding;
        std::optional<TreeBackingPlan> plan;
        if (!prepareTreeBacking(binding, inputs, plan)) return std::nullopt;
        WholeScreenTruth next = inputs.truth;
        return make(std::move(next), inputs.schema, inputs.prompt, std::move(plan));
    }

    static std::optional<PreparedTransition> prepare(OpenFinder request,
                                                     const TransitionInputs& inputs) {
        const PickerDescriptor* descriptor = pickerCatalog().find(request.picker);
        if (descriptor == nullptr) return std::nullopt;

        // Apply the open to a COPY of the prompt for parity: the copy IS the committed
        // prompt state, so the commit's prompt install is a move, not a re-run of open.
        // Opening over an already-active prompt replaces it (as the live opener does); no
        // conflicting-prompt rejection is introduced.
        PromptSurface prompt = inputs.prompt;
        const PromptCommandResult opened = prompt.open(PromptRequest{
            PromptKind::Palette, std::string{descriptor->promptTitle},
            {{"query", "command palette query", ""}}, {}, std::nullopt});
        if (!opened.accepted()) return std::nullopt;

        WholeScreenTruth next = inputs.truth;
        next.openPicker = request.picker;
        return make(std::move(next), inputs.schema, std::move(prompt), std::nullopt);
    }

    static std::optional<PreparedTransition> prepare(CloseFinder,
                                                     const TransitionInputs& inputs) {
        // Cancel on a copy. cancel is not inherently infallible; the preflight contract
        // is to REFUSE rather than fabricate a cleared state, so a rejected cancel
        // rejects the whole transition.
        PromptSurface prompt = inputs.prompt;
        if (!prompt.cancel().accepted()) return std::nullopt;
        WholeScreenTruth next = inputs.truth;
        next.openPicker = std::nullopt;
        return make(std::move(next), inputs.schema, std::move(prompt), std::nullopt);
    }
};

std::optional<PreparedTransition> prepareTransition(const CommandTransition& transition,
                                                    const TransitionInputs& inputs) {
    return std::visit(
        [&](auto request) { return TransitionBuilder::prepare(request, inputs); },
        transition);
}

}  // namespace ssg
