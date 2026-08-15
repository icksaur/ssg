#include <ssg/InteractionAuthority.h>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace ssg {

namespace {

// The revision source must lead every existing provider so a replacement stamped from it
// strictly increases; a source behind one is a broken invariant, not a runtime condition.
std::uint64_t requireSourceAheadOfProviders(std::uint64_t source, const TreeModel& tree) {
    for (const auto& identity : tree.providerIdentities()) {
        if (source <= identity.revision.value()) {
            throw std::logic_error(
                "InteractionAuthority revision source must lead every provider revision");
        }
    }
    return source;
}

}  // namespace

InteractionAuthority::InteractionAuthority(UiComposition initialAssembly, TreeModel& tree,
                                           std::uint64_t firstTreeRevision)
    : schema_{std::move(initialAssembly)},
      tree_{tree},
      nextTreeRevision_{requireSourceAheadOfProviders(firstTreeRevision, tree)},
      prompt_{},
      truth_{},
      interaction_{buildWholeScreenInteraction(schema_.validated(), truth_,
                                               std::nullopt)} {}

std::vector<TreeProviderPresence> InteractionAuthority::presentProviders() const {
    std::vector<TreeProviderPresence> present;
    for (const auto& identity : tree_.providerIdentities()) {
        present.push_back(TreeProviderPresence{identity.binding, identity.revision});
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
    // Only a finder transition may establish picker identity; a generic open must never be
    // a Palette prompt, or it would masquerade as a picker without an identity.
    if (request.kind == PromptKind::Palette) {
        return PromptCommandResult{
            PromptError{PromptErrorCode::InvalidRequest,
                        "a generic prompt must not be a Palette prompt; open a picker "
                        "through a finder transition"},
            std::nullopt};
    }
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
    // A value edit cannot change the prompt's activity, kind, region, presence, or focus,
    // so the interaction projection is unchanged -- swap only the prompt, no rebuild.
    PromptSurface copy = prompt_;
    PromptCommandResult result = copy.updateValue(index, std::move(value));
    if (result.accepted()) prompt_ = std::move(copy);
    return result;
}

bool InteractionAuthority::updateComposition(UiComposition assembly) {
    // Prepare both replacements before swapping either: update a COPY of the schema, build
    // the projection over it, then adopt both together, so a rebuild failure cannot leave a
    // new schema paired with the old interaction.
    WholeScreenSchema candidate = schema_;
    if (!candidate.update(std::move(assembly))) return false;
    UiInteractionState projection = buildWholeScreenInteraction(
        candidate.validated(), truth_, activePromptRegion(prompt_));
    schema_ = std::move(candidate);
    interaction_ = std::move(projection);
    return true;
}

TreeRevision InteractionAuthority::allocateTreeRevision() {
    if (nextTreeRevision_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::logic_error("InteractionAuthority tree revision source is exhausted");
    }
    return TreeRevision{nextTreeRevision_++};
}

}  // namespace ssg

