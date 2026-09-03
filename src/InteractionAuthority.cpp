#include <ssg/InteractionAuthority.h>
#include <ssg/WholeScreenAssembly.h>
#include "runtime/transition_internals.h"
#include "runtime/interaction_state.h"
#include "runtime/whole_screen_interaction.h"
#include "runtime/whole_screen_schema.h"

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

struct InteractionAuthority::Impl {
    UiComposition baseComposition_;
    std::vector<StatusActionNode> statusActions_;
    WholeScreenSchema schema_;
    TreeModel& tree_;
    std::uint64_t nextTreeRevision_;
    PickerActivationId nextPickerActivation_;
    std::optional<PickerActivation> openPickerActivation_;
    std::uint64_t routingGeneration_ = 0;
    PromptSurface prompt_;
    WholeScreenTruth truth_;
    UiInteractionState interaction_;

    Impl(UiComposition initialAssembly, TreeModel& tree,
         std::uint64_t firstTreeRevision,
         PickerActivationId firstPickerActivation)
        : baseComposition_{std::move(initialAssembly)},
          schema_{baseComposition_},
          tree_{tree},
          nextTreeRevision_{requireSourceAheadOfProviders(firstTreeRevision, tree)},
          nextPickerActivation_{firstPickerActivation},
          interaction_{buildWholeScreenInteraction(schema_.schema(), truth_,
                                                  std::nullopt)} {
        if (!nextPickerActivation_.valid()) {
            throw std::invalid_argument(
                "InteractionAuthority picker activation source must be valid");
        }
    }

    [[nodiscard]] std::vector<TreeProviderPresence> presentProviders() const;
    [[nodiscard]] UiComposition assembled(const UiComposition& base,
                                          const PromptSurface& prompt) const;
    void adopt(WholeScreenTruth next, PromptSurface prompt);
};

InteractionAuthority::InteractionAuthority(UiComposition initialAssembly, TreeModel& tree,
                                           std::uint64_t firstTreeRevision,
                                           PickerActivationId firstPickerActivation)
    : impl_{std::make_unique<Impl>(std::move(initialAssembly), tree,
                                   firstTreeRevision, firstPickerActivation)} {}

InteractionAuthority::~InteractionAuthority() = default;
InteractionAuthority::InteractionAuthority(InteractionAuthority&&) noexcept = default;
InteractionAuthority& InteractionAuthority::operator=(InteractionAuthority&&) noexcept =
    default;

std::vector<TreeProviderPresence>
InteractionAuthority::Impl::presentProviders() const {
    std::vector<TreeProviderPresence> present;
    for (const auto& identity : tree_.providerIdentities()) {
        present.push_back(TreeProviderPresence{identity.binding, identity.revision});
    }
    return present;
}

UiComposition InteractionAuthority::Impl::assembled(
    const UiComposition& base, const PromptSurface& prompt) const {
    UiComposition projected = withStatusActions(base, statusActions_);
    return prompt.active() ? withFooterPrompt(std::move(projected), prompt)
                           : projected;
}

void InteractionAuthority::Impl::adopt(WholeScreenTruth next, PromptSurface prompt) {
    const bool activePalette =
        prompt.active() && prompt.request()->kind == PromptKind::Palette;
    if (!activePalette) next.openPicker.reset();

    UiInteractionState projection = buildWholeScreenInteraction(
        schema_.schema(), next, activePromptRegion(prompt));

    prompt_ = std::move(prompt);
    truth_ = std::move(next);
    if (!truth_.openPicker) openPickerActivation_.reset();
    interaction_ = std::move(projection);
    ++routingGeneration_;
}

bool InteractionAuthority::apply(const CommandTransition& transition) {
    auto& state = *impl_;
    const auto* opening = std::get_if<OpenFinder>(&transition);
    const auto* openingDescriptor =
        opening != nullptr ? pickerCatalog().find(opening->picker) : nullptr;
    if (opening != nullptr && openingDescriptor == nullptr) return false;
    if (opening != nullptr &&
        state.nextPickerActivation_.value() ==
            std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }
    // Peek the revision source and prepare in one step: hiding prepare+install behind this
    // method means no allocation can occur between the peek and the consuming install.
    TransitionInputs inputs{state.truth_, state.schema_.schema(), state.prompt_,
                            state.presentProviders(), state.tree_.activeProviderBinding(),
                            TreeRevision{state.nextTreeRevision_}};
    std::optional<PreparedTransition> prepared = prepareTransition(transition, inputs);
    if (!prepared) return false;
    std::move(*prepared).installInto(state.truth_, state.interaction_,
                                     state.prompt_, state.tree_,
                                     state.nextTreeRevision_);
    if (opening != nullptr) {
        state.openPickerActivation_ =
            PickerActivation{openingDescriptor->wireMode, state.nextPickerActivation_};
        state.nextPickerActivation_ =
            PickerActivationId{state.nextPickerActivation_.value() + 1};
    } else if (!state.truth_.openPicker) {
        state.openPickerActivation_.reset();
    }
    ++state.routingGeneration_;
    return true;
}

PromptCommandResult InteractionAuthority::openPrompt(PromptRequest request) {
    auto& state = *impl_;
    // Only a finder transition may establish picker identity; a generic open must never be
    // a Palette prompt, or it would masquerade as a picker without an identity.
    if (request.kind == PromptKind::Palette) {
        return PromptCommandResult{
            PromptError{PromptErrorCode::InvalidRequest,
                        "a generic prompt must not be a Palette prompt; open a picker "
                        "through a finder transition"},
            std::nullopt};
    }
    PromptSurface copy = state.prompt_;
    PromptCommandResult result = copy.open(std::move(request));
    if (result.accepted()) {
        WholeScreenSchema candidate = state.schema_;
        candidate.update(state.assembled(state.baseComposition_, copy));
        WholeScreenTruth next = state.truth_;
        next.openPicker.reset();
        UiInteractionState projection = buildWholeScreenInteraction(
            candidate.schema(), next, activePromptRegion(copy));
        state.schema_ = std::move(candidate);
        state.prompt_ = std::move(copy);
        state.truth_ = std::move(next);
        state.openPickerActivation_.reset();
        state.interaction_ = std::move(projection);
        ++state.routingGeneration_;
    }
    return result;
}

PromptCommandResult InteractionAuthority::submitPrompt() {
    auto& state = *impl_;
    PromptSurface copy = state.prompt_;
    PromptCommandResult result = copy.submit();
    if (result.accepted()) state.adopt(state.truth_, std::move(copy));
    return result;
}

PromptCommandResult InteractionAuthority::cancelPrompt() {
    auto& state = *impl_;
    PromptSurface copy = state.prompt_;
    PromptCommandResult result = copy.cancel();
    if (result.accepted()) state.adopt(state.truth_, std::move(copy));
    return result;
}

PromptCommandResult InteractionAuthority::updatePromptValue(std::size_t index,
                                                            std::string value) {
    auto& state = *impl_;
    // A value edit cannot change the prompt's activity, kind, region, presence, or focus,
    // so the interaction projection is unchanged -- swap only the prompt, no rebuild.
    PromptSurface copy = state.prompt_;
    PromptCommandResult result = copy.updateValue(index, std::move(value));
    if (result.accepted()) {
        state.prompt_ = std::move(copy);
        ++state.routingGeneration_;
    }
    return result;
}

PromptCommandResult InteractionAuthority::focusPromptControl(
    std::string_view controlId) {
    auto& state = *impl_;
    // Which input owns the keyboard changes the semantic prompt view but not the
    // tree topology, presence, or the footer.prompt focus-capture anchor -- swap
    // only the prompt, no rebuild.
    PromptSurface copy = state.prompt_;
    PromptCommandResult result = copy.focusInput(controlId);
    if (result.accepted()) {
        state.prompt_ = std::move(copy);
        ++state.routingGeneration_;
    }
    return result;
}

PromptCommandResult InteractionAuthority::focusNextPromptControl() {
    auto& state = *impl_;
    PromptSurface copy = state.prompt_;
    PromptCommandResult result = copy.focusNextInput();
    if (result.accepted()) {
        state.prompt_ = std::move(copy);
        ++state.routingGeneration_;
    }
    return result;
}

void InteractionAuthority::toggleDistractionFree() {
    auto& state = *impl_;
    WholeScreenTruth next = state.truth_;
    next.distractionFree = !next.distractionFree;
    state.adopt(std::move(next), state.prompt_);
}

void InteractionAuthority::focusEditor() {
    auto& state = *impl_;
    // Build from a prospective truth, then adopt both together -- truth_ is never mutated
    // before the projection is rebuilt.
    WholeScreenTruth next = state.truth_;
    next.baseFocus = BaseFocus::Editor;
    state.adopt(std::move(next), state.prompt_);
}

bool InteractionAuthority::focusPanel() {
    auto& state = *impl_;
    if (!state.truth_.panelPresent) return false;
    WholeScreenTruth next = state.truth_;
    next.baseFocus = BaseFocus::Panel;
    state.adopt(std::move(next), state.prompt_);
    return true;
}

bool InteractionAuthority::refreshNoticePresence(bool present) {
    auto& state = *impl_;
    if (state.truth_.noticePresent == present) return false;
    // Build from a prospective truth, then adopt both together -- truth_ is never
    // mutated before the projection is rebuilt.
    WholeScreenTruth next = state.truth_;
    next.noticePresent = present;
    state.adopt(std::move(next), state.prompt_);
    return true;
}

bool InteractionAuthority::refreshExternalModificationPresence(bool present) {
    auto& state = *impl_;
    if (state.truth_.externalModificationPresent == present &&
        (present || !state.truth_.externalFocusHeld)) {
        return false;
    }
    WholeScreenTruth next = state.truth_;
    next.externalModificationPresent = present;
    // Presence dropping clears focus: the capture auto-pops and a later disk event
    // that re-raises the bar never reactively steals the keyboard.
    if (!present) next.externalFocusHeld = false;
    state.adopt(std::move(next), state.prompt_);
    return true;
}

bool InteractionAuthority::captureExternalFocus() {
    auto& state = *impl_;
    if (!state.truth_.externalModificationPresent || state.truth_.externalFocusHeld) {
        return false;
    }
    WholeScreenTruth next = state.truth_;
    next.externalFocusHeld = true;
    state.adopt(std::move(next), state.prompt_);
    return true;
}

bool InteractionAuthority::releaseExternalFocus() {
    auto& state = *impl_;
    if (!state.truth_.externalFocusHeld) return false;
    WholeScreenTruth next = state.truth_;
    next.externalFocusHeld = false;
    state.adopt(std::move(next), state.prompt_);
    return true;
}

bool InteractionAuthority::updateComposition(UiComposition assembly) {    // Prepare both replacements before swapping either: update a COPY of the schema, build
    auto& state = *impl_;
    // the projection over it, then adopt both together, so a rebuild failure cannot leave a
    // new schema paired with the old interaction.
    WholeScreenSchema candidate = state.schema_;
    UiComposition projected = state.assembled(assembly, state.prompt_);
    if (!candidate.update(std::move(projected))) {
        state.baseComposition_ = std::move(assembly);
        return false;
    }
    UiInteractionState projection = buildWholeScreenInteraction(
        candidate.schema(), state.truth_, activePromptRegion(state.prompt_));
    state.schema_ = std::move(candidate);
    state.baseComposition_ = std::move(assembly);
    state.interaction_ = std::move(projection);
    return true;
}

bool InteractionAuthority::refreshStatusActions(
    std::vector<StatusActionNode> actions) {
    auto& state = *impl_;
    if (actions == state.statusActions_) return false;
    WholeScreenSchema candidate = state.schema_;
    UiComposition projected = withStatusActions(state.baseComposition_, actions);
    if (state.prompt_.active()) {
        projected = withFooterPrompt(std::move(projected), state.prompt_);
    }
    const bool schemaChanged = candidate.update(std::move(projected));
    std::optional<UiInteractionState> interaction;
    if (schemaChanged) {
        interaction = buildWholeScreenInteraction(
            candidate.schema(), state.truth_, activePromptRegion(state.prompt_));
    }
    state.statusActions_ = std::move(actions);
    if (schemaChanged) {
        state.schema_ = std::move(candidate);
        state.interaction_ = std::move(*interaction);
    }
    return true;
}

const PromptSurface& InteractionAuthority::prompt() const noexcept {
    return impl_->prompt_;
}

const std::vector<StatusActionNode>& InteractionAuthority::statusActions() const
    noexcept {
    return impl_->statusActions_;
}

FocusTarget InteractionAuthority::effectiveFocus() const noexcept {
    return impl_->interaction_.effectiveFocus();
}

std::optional<PickerKind> InteractionAuthority::openPicker() const noexcept {
    return impl_->truth_.openPicker;
}

const std::optional<PickerActivation>&
InteractionAuthority::openPickerActivation() const noexcept {
    return impl_->openPickerActivation_;
}

std::uint64_t InteractionAuthority::routingGeneration() const noexcept {
    return impl_->routingGeneration_;
}

const UiSchema& InteractionAuthority::schema() const noexcept {
    return impl_->interaction_.schema();
}

std::vector<UiNodeId> InteractionAuthority::focusPath() const {
    return impl_->interaction_.focusPath();
}

PalettePresenceOverlay InteractionAuthority::pickerPresenceOverlay() const {
    return derivePickerPresenceOverlay(impl_->schema_.schema(), impl_->truth_);
}

TreeRevision InteractionAuthority::allocateTreeRevision() {
    if (impl_->nextTreeRevision_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::logic_error("InteractionAuthority tree revision source is exhausted");
    }
    return TreeRevision{impl_->nextTreeRevision_++};
}

}  // namespace ssg
