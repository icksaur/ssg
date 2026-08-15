#include <ssg/InteractionAuthority.h>

#include <utility>

namespace ssg {

InteractionAuthority::InteractionAuthority(UiComposition initialAssembly, TreeModel& tree,
                                           std::uint64_t firstTreeRevision)
    : schema_{std::move(initialAssembly)},
      tree_{tree},
      nextTreeRevision_{firstTreeRevision},
      prompt_{},
      truth_{},
      interaction_{buildWholeScreenInteraction(schema_.validated(), truth_,
                                               std::nullopt)} {}

std::vector<TreeProviderPresence> InteractionAuthority::presentProviders() const {
    std::vector<TreeProviderPresence> present;
    for (const TreeProviderView& view : tree_.viewState().providers) {
        present.push_back(TreeProviderPresence{
            TreeProviderBinding{view.providerId, view.kind},
            tree_.providerRevision(view.providerId).value_or(TreeRevision{0})});
    }
    return present;
}

bool InteractionAuthority::apply(const CommandTransition& transition) {
    // Peek the revision source and prepare in one step: hiding prepare+install behind this
    // method means no allocation can occur between the peek and the consuming install.
    TransitionInputs inputs{truth_, schema_.validated(), prompt_, presentProviders(),
                            TreeRevision{nextTreeRevision_}};
    std::optional<PreparedTransition> prepared = prepareTransition(transition, inputs);
    if (!prepared) return false;
    std::move(*prepared).installInto(truth_, interaction_, prompt_, tree_,
                                     nextTreeRevision_);
    return true;
}

void InteractionAuthority::applyPromptState(PromptSurface prompt) {
    WholeScreenTruth next = truth_;
    // A picker identity is meaningful only while its Palette prompt is active; any other
    // prompt state (a generic prompt, or a closed prompt) clears it.
    const bool activePalette =
        prompt.active() && prompt.request()->kind == PromptKind::Palette;
    if (!activePalette) next.openPicker.reset();

    UiInteractionState projection = buildWholeScreenInteraction(
        schema_.validated(), next, activePromptRegion(prompt));

    prompt_ = std::move(prompt);
    truth_ = std::move(next);
    interaction_ = std::move(projection);
}

PromptCommandResult InteractionAuthority::openPrompt(PromptRequest request) {
    PromptSurface copy = prompt_;
    PromptCommandResult result = copy.open(std::move(request));
    if (result.accepted()) applyPromptState(std::move(copy));
    return result;
}

PromptCommandResult InteractionAuthority::submitPrompt() {
    PromptSurface copy = prompt_;
    PromptCommandResult result = copy.submit();
    if (result.accepted()) applyPromptState(std::move(copy));
    return result;
}

PromptCommandResult InteractionAuthority::cancelPrompt() {
    PromptSurface copy = prompt_;
    PromptCommandResult result = copy.cancel();
    if (result.accepted()) applyPromptState(std::move(copy));
    return result;
}

PromptCommandResult InteractionAuthority::updatePromptValue(std::size_t index,
                                                            std::string value) {
    PromptSurface copy = prompt_;
    PromptCommandResult result = copy.updateValue(index, std::move(value));
    if (result.accepted()) applyPromptState(std::move(copy));
    return result;
}

bool InteractionAuthority::updateComposition(UiComposition assembly) {
    const bool advanced = schema_.update(std::move(assembly));
    // On a generation advance, migrate by rebuilding from the SAME truth and prompt over
    // the new schema; the fresh projection resets the presence basis for the generation.
    if (advanced) applyPromptState(prompt_);
    return advanced;
}

TreeRevision InteractionAuthority::allocateTreeRevision() {
    return TreeRevision{nextTreeRevision_++};
}

}  // namespace ssg
