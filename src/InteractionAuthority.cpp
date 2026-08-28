#include <ssg/InteractionAuthority.h>
#include <ssg/WholeScreenAssembly.h>

#include <algorithm>
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
    : baseComposition_{std::move(initialAssembly)},
      schema_{baseComposition_},
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
    ++routingGeneration_;
    return true;
}

void InteractionAuthority::adopt(WholeScreenTruth next, PromptSurface prompt) {
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
    // adopt is the single owner-swap for prompt/focus/presence, so every routing
    // change that flows through it (open/submit/cancel prompt, focusEditor/Panel,
    // a presence refresh) advances the routing generation here.
    ++routingGeneration_;
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
    if (result.accepted()) {
        WholeScreenSchema candidate = schema_;
        candidate.update(withFooterPrompt(baseComposition_, copy));
        WholeScreenTruth next = truth_;
        next.openPicker.reset();
        UiInteractionState projection = buildWholeScreenInteraction(
            candidate.validated(), next, activePromptRegion(copy));
        schema_ = std::move(candidate);
        prompt_ = std::move(copy);
        truth_ = std::move(next);
        interaction_ = std::move(projection);
        ++routingGeneration_;
    }
    return result;
}

PromptCommandResult InteractionAuthority::submitPrompt() {
    PromptSurface copy = prompt_;
    PromptCommandResult result = copy.submit();
    if (result.accepted()) adopt(truth_, std::move(copy));
    return result;
}

PromptCommandResult InteractionAuthority::cancelPrompt() {
    PromptSurface copy = prompt_;
    PromptCommandResult result = copy.cancel();
    if (result.accepted()) adopt(truth_, std::move(copy));
    return result;
}

PromptCommandResult InteractionAuthority::updatePromptValue(std::size_t index,
                                                            std::string value) {
    // A value edit cannot change the prompt's activity, kind, region, presence, or focus,
    // so the interaction projection is unchanged -- swap only the prompt, no rebuild.
    PromptSurface copy = prompt_;
    PromptCommandResult result = copy.updateValue(index, std::move(value));
    if (result.accepted()) {
        prompt_ = std::move(copy);
        ++routingGeneration_;
    }
    return result;
}

PromptCommandResult InteractionAuthority::focusPromptControl(
    std::string_view controlId) {
    // Which input owns the keyboard changes the semantic prompt view but not the
    // tree topology, presence, or the footer.prompt focus-capture anchor -- swap
    // only the prompt, no rebuild.
    PromptSurface copy = prompt_;
    PromptCommandResult result = copy.focusInput(controlId);
    if (result.accepted()) {
        prompt_ = std::move(copy);
        ++routingGeneration_;
    }
    return result;
}

PromptCommandResult InteractionAuthority::focusNextPromptControl() {
    PromptSurface copy = prompt_;
    PromptCommandResult result = copy.focusNextInput();
    if (result.accepted()) {
        prompt_ = std::move(copy);
        ++routingGeneration_;
    }
    return result;
}

void InteractionAuthority::toggleDistractionFree() {
    WholeScreenTruth next = truth_;
    next.distractionFree = !next.distractionFree;
    adopt(std::move(next), prompt_);
}

void InteractionAuthority::focusEditor() {
    // Build from a prospective truth, then adopt both together -- truth_ is never mutated
    // before the projection is rebuilt.
    WholeScreenTruth next = truth_;
    next.baseFocus = BaseFocus::Editor;
    adopt(std::move(next), prompt_);
}

bool InteractionAuthority::focusPanel() {
    if (!truth_.panelPresent) return false;
    WholeScreenTruth next = truth_;
    next.baseFocus = BaseFocus::Panel;
    adopt(std::move(next), prompt_);
    return true;
}

bool InteractionAuthority::refreshNoticePresence(bool present) {
    if (truth_.noticePresent == present) return false;
    // Build from a prospective truth, then adopt both together -- truth_ is never
    // mutated before the projection is rebuilt.
    WholeScreenTruth next = truth_;
    next.noticePresent = present;
    adopt(std::move(next), prompt_);
    return true;
}

bool InteractionAuthority::refreshExternalModificationPresence(bool present) {
    if (truth_.externalModificationPresent == present &&
        (present || !truth_.externalFocusHeld)) {
        return false;
    }
    WholeScreenTruth next = truth_;
    next.externalModificationPresent = present;
    // Presence dropping clears focus: the capture auto-pops and a later disk event
    // that re-raises the bar never reactively steals the keyboard.
    if (!present) next.externalFocusHeld = false;
    adopt(std::move(next), prompt_);
    return true;
}

bool InteractionAuthority::captureExternalFocus() {
    if (!truth_.externalModificationPresent || truth_.externalFocusHeld) {
        return false;
    }
    WholeScreenTruth next = truth_;
    next.externalFocusHeld = true;
    adopt(std::move(next), prompt_);
    return true;
}

bool InteractionAuthority::releaseExternalFocus() {
    if (!truth_.externalFocusHeld) return false;
    WholeScreenTruth next = truth_;
    next.externalFocusHeld = false;
    adopt(std::move(next), prompt_);
    return true;
}

bool InteractionAuthority::updateComposition(UiComposition assembly) {    // Prepare both replacements before swapping either: update a COPY of the schema, build
    // the projection over it, then adopt both together, so a rebuild failure cannot leave a
    // new schema paired with the old interaction.
    WholeScreenSchema candidate = schema_;
    UiComposition projected = prompt_.active()
                                  ? withFooterPrompt(assembly, prompt_)
                                  : assembly;
    if (!candidate.update(std::move(projected))) {
        baseComposition_ = std::move(assembly);
        return false;
    }
    UiInteractionState projection = buildWholeScreenInteraction(
        candidate.validated(), truth_, activePromptRegion(prompt_));
    schema_ = std::move(candidate);
    baseComposition_ = std::move(assembly);
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
