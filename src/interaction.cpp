#include <ssg/interaction.h>
#include <ssg/whole_screen_schema.h>

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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

struct WholeScreenProjection {
    bool panelPresent = false;
    bool distractionFree = false;
    std::optional<PickerKind> openPicker;
    BaseFocus baseFocus = BaseFocus::Editor;
    bool noticePresent = false;
    bool externalModificationPresent = false;
    bool externalFocusHeld = false;
};

UiNodeId nodeId(std::string_view id) { return UiNodeId{std::string{id}}; }

UiInteractionState buildWholeScreenInteraction(
    UiSchema schema, const WholeScreenProjection& truth,
    std::optional<PromptRegion> promptRegion) {
    std::vector<UiNodeId> hidden;
    if (!truth.panelPresent) hidden.push_back(nodeId(kPanelNodeId));
    if (truth.distractionFree) {
        for (const std::string_view region :
             {kNoticeNodeId, kExternalModNodeId, kPanelNodeId, kTabBarNodeId,
              kFooterNodeId}) {
            hidden.push_back(nodeId(region));
        }
        if (promptRegion != PromptRegion::Header)
            hidden.push_back(nodeId(kHeaderNodeId));
        if (promptRegion != PromptRegion::Footer)
            hidden.push_back(nodeId(kFooterPromptNodeId));
    }
    const bool pickerOpen = truth.openPicker.has_value();
    hidden.push_back(
        nodeId(pickerOpen ? kEditorNodeId : kFindResultsViewportNodeId));
    if (pickerOpen) hidden.push_back(nodeId(kTabBarNodeId));

    const UiNodeId inputLine = nodeId(kHeaderPromptInputNodeId);
    const bool headerPrompt = promptRegion && *promptRegion == PromptRegion::Header;
    const bool hasInputLine = findUiNode(schema, inputLine) != nullptr;
    if (headerPrompt && !hasInputLine) {
        throw std::logic_error("header prompt requires the input_line node");
    }
    if (hasInputLine && !headerPrompt) hidden.push_back(inputLine);

    const UiNodeId footerPrompt = nodeId(kFooterPromptNodeId);
    const bool footerPromptOpen =
        promptRegion && *promptRegion == PromptRegion::Footer;
    const bool hasFooterPrompt = findUiNode(schema, footerPrompt) != nullptr;
    if (footerPromptOpen && !hasFooterPrompt) {
        throw std::logic_error("footer prompt requires the footer.prompt node");
    }
    if (hasFooterPrompt && !footerPromptOpen) hidden.push_back(footerPrompt);
    if (footerPromptOpen) hidden.push_back(nodeId(kFooterNodeId));

    const UiNodeId notice = nodeId(kNoticeNodeId);
    const bool hasNotice = findUiNode(schema, notice) != nullptr;
    if (truth.noticePresent && !hasNotice) {
        throw std::logic_error("draft notice requires the notice node");
    }
    if (hasNotice && !truth.noticePresent) hidden.push_back(notice);

    const UiNodeId externalMod = nodeId(kExternalModNodeId);
    const bool hasExternalMod = findUiNode(schema, externalMod) != nullptr;
    if (truth.externalModificationPresent && !hasExternalMod) {
        throw std::logic_error(
            "external modification section requires the externalmod node");
    }
    if (hasExternalMod && !truth.externalModificationPresent) {
        hidden.push_back(externalMod);
    }

    UiInteractionState state{std::move(schema), std::move(hidden)};
    state.setBaseFocus(
        truth.baseFocus == BaseFocus::Panel && truth.panelPresent &&
                !truth.distractionFree
            ? BaseFocus::Panel
            : BaseFocus::Editor);
    if (truth.externalModificationPresent && hasExternalMod &&
        truth.externalFocusHeld) {
        state.captureFocus(FocusCapture{externalMod});
    }
    if (promptRegion) {
        state.captureFocus(FocusCapture{headerPrompt ? inputLine : footerPrompt});
    }
    return state;
}

PalettePresenceOverlay derivePickerPresenceOverlay(
    const UiSchema& schema, WholeScreenProjection truth) {
    truth.openPicker.reset();
    const auto closedState =
        buildWholeScreenInteraction(schema, truth, std::nullopt);
    truth.openPicker = PickerKind::Command;
    const auto openState =
        buildWholeScreenInteraction(schema, truth, PromptRegion::Header);

    PalettePresenceOverlay overlay;
    for (const UiNodeId& id : uiSchemaNodeIds(schema)) {
        const bool before = isUiNodeVisible(closedState.schema(), id);
        const bool after = isUiNodeVisible(openState.schema(), id);
        if (before == after) continue;
        overlay.ops.push_back(
            {after ? PalettePresenceOpKind::Show
                   : PalettePresenceOpKind::Hide,
             id});
    }
    return overlay;
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
      interaction_{buildWholeScreenInteraction(schema_.schema(), {}, std::nullopt)} {
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

UiInteractionState InteractionState::project(
    const UiSchema& schema, const PromptSurface& prompt) const {
    return buildWholeScreenInteraction(
        schema,
        {panelPresent_, distractionFree_, openPicker_, baseFocus_,
         noticePresent_, externalModificationPresent_, externalFocusHeld_},
        activePromptRegion(prompt));
}

void InteractionState::adopt(PromptSurface prompt) {
    const bool activePalette =
        prompt.active() && prompt.request()->kind == PromptKind::Palette;
    if (!activePalette) openPicker_.reset();

    UiInteractionState projection = project(schema_.schema(), prompt);

    prompt_ = std::move(prompt);
    if (!openPicker_) openPickerActivation_.reset();
    interaction_ = std::move(projection);
    ++routingGeneration_;
}

bool InteractionState::activatePanelProvider(
    TreeProviderBinding binding, bool panelPresent, BaseFocus baseFocus,
    BaseFocus panelReturnFocus) {
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
        schema_.schema(),
        {panelPresent, distractionFree_, openPicker_, baseFocus, noticePresent_,
         externalModificationPresent_, externalFocusHeld_},
        activePromptRegion(prompt_));
    if (created) {
        tree_.replaceProvider(std::move(*created));
        ++nextTreeRevision_;
    }
    (void)tree_.activateProvider(binding.id);
    panelPresent_ = panelPresent;
    baseFocus_ = baseFocus;
    panelReturnFocus_ = panelReturnFocus;
    interaction_ = std::move(projection);
    ++routingGeneration_;
    return true;
}

bool InteractionState::togglePanel() {
    auto& state = *this;
    if (state.panelPresent_) {
        state.panelPresent_ = false;
        state.baseFocus_ = state.panelReturnFocus_;
    } else {
        state.panelReturnFocus_ = state.baseFocus_;
        state.panelPresent_ = true;
        state.baseFocus_ = BaseFocus::Panel;
    }
    state.adopt(state.prompt_);
    return true;
}

bool InteractionState::showPanelProvider(TreeProviderKind kind) {
    auto& state = *this;
    const TreeProviderBinding binding = panelTreeProvider(kind);
    if (state.panelPresent_ &&
        state.tree_.activeProviderBinding() == binding) {
        return togglePanel();
    }
    const BaseFocus returnFocus =
        state.panelPresent_ ? state.panelReturnFocus_ : state.baseFocus_;
    return state.activatePanelProvider(binding, true, BaseFocus::Panel,
                                       returnFocus);
}

bool InteractionState::switchPanelProvider(CycleDirection direction) {
    auto& state = *this;
    const auto active = state.tree_.activeProviderBinding();
    if (!active) return false;
    return state.activatePanelProvider(
        cyclePanelTreeProvider(*active, direction), state.panelPresent_,
        state.baseFocus_, state.panelReturnFocus_);
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
    state.openPicker_ = kind;
    state.adopt(std::move(prompt));
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
    state.openPicker_.reset();
    state.adopt(std::move(prompt));
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
        state.openPicker_.reset();
        UiInteractionState projection = state.project(candidate.schema(), copy);
        state.schema_ = std::move(candidate);
        state.prompt_ = std::move(copy);
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
    if (result.accepted()) state.adopt(std::move(copy));
    return result;
}

PromptCommandResult InteractionState::cancelPrompt() {
    auto& state = *this;
    PromptSurface copy = state.prompt_;
    PromptCommandResult result = copy.cancel();
    if (result.accepted()) state.adopt(std::move(copy));
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
    state.distractionFree_ = !state.distractionFree_;
    state.adopt(state.prompt_);
}

void InteractionState::focusEditor() {
    auto& state = *this;
    state.baseFocus_ = BaseFocus::Editor;
    state.adopt(state.prompt_);
}

bool InteractionState::focusPanel() {
    auto& state = *this;
    if (!state.panelPresent_) return false;
    state.baseFocus_ = BaseFocus::Panel;
    state.adopt(state.prompt_);
    return true;
}

bool InteractionState::refreshNoticePresence(bool present) {
    auto& state = *this;
    if (state.noticePresent_ == present) return false;
    state.noticePresent_ = present;
    state.adopt(state.prompt_);
    return true;
}

bool InteractionState::refreshExternalModificationPresence(bool present) {
    auto& state = *this;
    if (state.externalModificationPresent_ == present &&
        (present || !state.externalFocusHeld_)) {
        return false;
    }
    state.externalModificationPresent_ = present;
    // Presence dropping clears focus: the capture auto-pops and a later disk event
    // that re-raises the bar never reactively steals the keyboard.
    if (!present) state.externalFocusHeld_ = false;
    state.adopt(state.prompt_);
    return true;
}

bool InteractionState::captureExternalFocus() {
    auto& state = *this;
    if (!state.externalModificationPresent_ || state.externalFocusHeld_) {
        return false;
    }
    state.externalFocusHeld_ = true;
    state.adopt(state.prompt_);
    return true;
}

bool InteractionState::releaseExternalFocus() {
    auto& state = *this;
    if (!state.externalFocusHeld_) return false;
    state.externalFocusHeld_ = false;
    state.adopt(state.prompt_);
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
    UiInteractionState projection = state.project(candidate.schema(), state.prompt_);
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
        interaction = state.project(candidate.schema(), state.prompt_);
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
    return openPicker_;
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
    return derivePickerPresenceOverlay(
        schema_.schema(),
        {panelPresent_, distractionFree_, openPicker_, baseFocus_,
         noticePresent_, externalModificationPresent_, externalFocusHeld_});
}

TreeRevision InteractionState::allocateTreeRevision() {
    if (nextTreeRevision_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::logic_error("interaction tree revision source is exhausted");
    }
    return TreeRevision{nextTreeRevision_++};
}

}  // namespace ssg
