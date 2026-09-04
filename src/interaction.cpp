#include <ssg/interaction.h>
#include <ssg/WholeScreenAssembly.h>
#include <ssg/whole_screen_interaction.h>

#include <algorithm>
#include <array>
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

TreeProviderBinding panelTreeProvider(TreeProviderKind kind) {
    const auto found = std::ranges::find(
        kPanelTreeProviders, kind, &TreeProviderBinding::kind);
    if (found == kPanelTreeProviders.end()) {
        throw std::logic_error("corrupt TreeProviderKind enumerator");
    }
    return *found;
}

TreeProviderBinding cyclePanelTreeProvider(
    const TreeProviderBinding& provider, CycleDirection direction) {
    const auto at = std::ranges::find(kPanelTreeProviders, provider);
    if (at == kPanelTreeProviders.end()) {
        throw std::logic_error("active tree provider is outside the panel cycle");
    }
    std::size_t step;
    switch (direction) {
    case CycleDirection::Next:
        step = 1;
        break;
    case CycleDirection::Previous:
        step = kPanelTreeProviders.size() - 1;
        break;
    default:
        throw std::logic_error("corrupt CycleDirection enumerator");
    }
    const std::size_t index =
        static_cast<std::size_t>(at - kPanelTreeProviders.begin());
    return kPanelTreeProviders[(index + step) % kPanelTreeProviders.size()];
}

std::optional<PromptRegion> activePromptRegion(const PromptSurface& prompt) {
    if (!prompt.active()) return std::nullopt;
    return promptFocusRegion(prompt.request()->kind);
}

// The revision source must lead every existing provider so a replacement stamped from it
// strictly increases; a source behind one is a broken invariant, not a runtime condition.
std::uint64_t requireSourceAheadOfProviders(std::uint64_t source, const TreeModel& tree) {
    for (const auto& identity : tree.providerIdentities()) {
        if (source <= identity.revision.value()) {
            throw std::logic_error(
                "interaction revision source must lead every provider revision");
        }
    }
    return source;
}

}  // namespace

InteractionState::InteractionState(UiComposition initialAssembly, TreeModel& tree,
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
           "interaction picker activation source must be valid");
    }
}

UiComposition InteractionState::assembled(
    const UiComposition& base, const PromptSurface& prompt) const {
    UiComposition projected = withStatusActions(base, statusActions_);
    return prompt.active() ? withFooterPrompt(std::move(projected), prompt)
                           : projected;
}

void InteractionState::adopt(WholeScreenTruth next, PromptSurface prompt) {
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

bool InteractionState::activatePanelProvider(
    TreeProviderBinding binding, WholeScreenTruth next) {
    const auto identities = tree_.providerIdentities();
    const auto existing = std::ranges::find(
        identities, binding.id, [](const TreeModel::ProviderIdentity& identity) {
            return identity.binding.id;
        });
    if (existing != identities.end() && existing->binding.kind != binding.kind) {
        return false;
    }

    std::optional<TreeProviderSnapshot> created;
    if (existing == identities.end()) {
        if (!treeProviderCanBeCreatedEmpty(binding.kind) ||
            nextTreeRevision_ == std::numeric_limits<std::uint64_t>::max()) {
            return false;
        }
        created = TreeProviderSnapshot{
            binding.id, binding.kind, TreeRevision{nextTreeRevision_}, {}};
    }

    UiInteractionState projection = buildWholeScreenInteraction(
        schema_.schema(), next, activePromptRegion(prompt_));
    if (created) {
        tree_.replaceProvider(std::move(*created));
        ++nextTreeRevision_;
    }
    (void)tree_.activateProvider(binding.id);
    truth_ = std::move(next);
    interaction_ = std::move(projection);
    ++routingGeneration_;
    return true;
}

bool InteractionState::togglePanel() {
    auto& state = *this;
    WholeScreenTruth next = state.truth_;
    if (next.panelPresent) {
        next.panelPresent = false;
        next.baseFocus = next.panelReturnFocus;
    } else {
        next.panelReturnFocus = next.baseFocus;
        next.panelPresent = true;
        next.baseFocus = BaseFocus::Panel;
    }
    state.adopt(std::move(next), state.prompt_);
    return true;
}

bool InteractionState::showPanelProvider(TreeProviderKind kind) {
    auto& state = *this;
    const TreeProviderBinding binding = panelTreeProvider(kind);
    if (state.truth_.panelPresent &&
        state.tree_.activeProviderBinding() == binding) {
        return togglePanel();
    }
    WholeScreenTruth next = state.truth_;
    if (!next.panelPresent) next.panelReturnFocus = next.baseFocus;
    next.panelPresent = true;
    next.baseFocus = BaseFocus::Panel;
    return state.activatePanelProvider(binding, std::move(next));
}

bool InteractionState::switchPanelProvider(CycleDirection direction) {
    auto& state = *this;
    const auto active = state.tree_.activeProviderBinding();
    if (!active) return false;
    return state.activatePanelProvider(
        cyclePanelTreeProvider(*active, direction), state.truth_);
}

bool InteractionState::openFinder(PickerKind kind) {
    auto& state = *this;
    const PickerDescriptor* descriptor = pickerCatalog().find(kind);
    if (descriptor == nullptr ||
        state.nextPickerActivation_.value() ==
            std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }
    PromptSurface prompt = state.prompt_;
    const PromptCommandResult opened = prompt.open(PromptRequest{
        PromptKind::Palette, std::string{descriptor->promptTitle},
        {{"query", "command palette query", ""}}, {}, std::nullopt});
    if (!opened.accepted()) return false;
    WholeScreenTruth next = state.truth_;
    next.openPicker = kind;
    state.adopt(std::move(next), std::move(prompt));
    state.openPickerActivation_ =
        PickerActivation{descriptor->wireMode, state.nextPickerActivation_};
    state.nextPickerActivation_ =
        PickerActivationId{state.nextPickerActivation_.value() + 1};
    return true;
}

bool InteractionState::closeFinder() {
    auto& state = *this;
    PromptSurface prompt = state.prompt_;
    if (!prompt.cancel().accepted()) return false;
    WholeScreenTruth next = state.truth_;
    next.openPicker.reset();
    state.adopt(std::move(next), std::move(prompt));
    return true;
}

PromptCommandResult InteractionState::openPrompt(PromptRequest request) {
    auto& state = *this;
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

PromptCommandResult InteractionState::submitPrompt() {
    auto& state = *this;
    PromptSurface copy = state.prompt_;
    PromptCommandResult result = copy.submit();
    if (result.accepted()) state.adopt(state.truth_, std::move(copy));
    return result;
}

PromptCommandResult InteractionState::cancelPrompt() {
    auto& state = *this;
    PromptSurface copy = state.prompt_;
    PromptCommandResult result = copy.cancel();
    if (result.accepted()) state.adopt(state.truth_, std::move(copy));
    return result;
}

PromptCommandResult InteractionState::updatePromptValue(std::size_t index,
                                                         std::string value) {
    auto& state = *this;
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

PromptCommandResult InteractionState::focusPromptControl(
    std::string_view controlId) {
    auto& state = *this;
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

PromptCommandResult InteractionState::focusNextPromptControl() {
    auto& state = *this;
    PromptSurface copy = state.prompt_;
    PromptCommandResult result = copy.focusNextInput();
    if (result.accepted()) {
        state.prompt_ = std::move(copy);
        ++state.routingGeneration_;
    }
    return result;
}

void InteractionState::toggleDistractionFree() {
    auto& state = *this;
    WholeScreenTruth next = state.truth_;
    next.distractionFree = !next.distractionFree;
    state.adopt(std::move(next), state.prompt_);
}

void InteractionState::focusEditor() {
    auto& state = *this;
    // Build from a prospective truth, then adopt both together -- truth_ is never mutated
    // before the projection is rebuilt.
    WholeScreenTruth next = state.truth_;
    next.baseFocus = BaseFocus::Editor;
    state.adopt(std::move(next), state.prompt_);
}

bool InteractionState::focusPanel() {
    auto& state = *this;
    if (!state.truth_.panelPresent) return false;
    WholeScreenTruth next = state.truth_;
    next.baseFocus = BaseFocus::Panel;
    state.adopt(std::move(next), state.prompt_);
    return true;
}

bool InteractionState::refreshNoticePresence(bool present) {
    auto& state = *this;
    if (state.truth_.noticePresent == present) return false;
    // Build from a prospective truth, then adopt both together -- truth_ is never
    // mutated before the projection is rebuilt.
    WholeScreenTruth next = state.truth_;
    next.noticePresent = present;
    state.adopt(std::move(next), state.prompt_);
    return true;
}

bool InteractionState::refreshExternalModificationPresence(bool present) {
    auto& state = *this;
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

bool InteractionState::captureExternalFocus() {
    auto& state = *this;
    if (!state.truth_.externalModificationPresent || state.truth_.externalFocusHeld) {
        return false;
    }
    WholeScreenTruth next = state.truth_;
    next.externalFocusHeld = true;
    state.adopt(std::move(next), state.prompt_);
    return true;
}

bool InteractionState::releaseExternalFocus() {
    auto& state = *this;
    if (!state.truth_.externalFocusHeld) return false;
    WholeScreenTruth next = state.truth_;
    next.externalFocusHeld = false;
    state.adopt(std::move(next), state.prompt_);
    return true;
}

bool InteractionState::updateComposition(UiComposition assembly) {
    auto& state = *this;
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

bool InteractionState::refreshStatusActions(
    std::vector<StatusActionNode> actions) {
    auto& state = *this;
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

const PromptSurface& InteractionState::prompt() const noexcept {
    return prompt_;
}

const std::vector<StatusActionNode>& InteractionState::statusActions() const
    noexcept {
    return statusActions_;
}

FocusTarget InteractionState::effectiveFocus() const noexcept {
    return interaction_.effectiveFocus();
}

std::optional<PickerKind> InteractionState::openPicker() const noexcept {
    return truth_.openPicker;
}

const std::optional<PickerActivation>&
InteractionState::openPickerActivation() const noexcept {
    return openPickerActivation_;
}

std::uint64_t InteractionState::routingGeneration() const noexcept {
    return routingGeneration_;
}

const UiSchema& InteractionState::schema() const noexcept {
    return interaction_.schema();
}

std::vector<UiNodeId> InteractionState::focusPath() const {
    return interaction_.focusPath();
}

PalettePresenceOverlay InteractionState::pickerPresenceOverlay() const {
    return derivePickerPresenceOverlay(schema_.schema(), truth_);
}

TreeRevision InteractionState::allocateTreeRevision() {
    if (nextTreeRevision_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::logic_error("interaction tree revision source is exhausted");
    }
    return TreeRevision{nextTreeRevision_++};
}

}  // namespace ssg
