#include <ssg/ScreenState.h>
#include <ssg/ScreenLayout.h>

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
    const std::size_t step = direction == CycleDirection::Next
                                 ? 1
                                 : kPanelTreeProviders.size() - 1;
    const std::size_t index =
        static_cast<std::size_t>(at - kPanelTreeProviders.begin());
    return kPanelTreeProviders[(index + step) % kPanelTreeProviders.size()];
}

std::optional<PromptRegion> activePromptRegion(const PromptSurface& prompt) {
    if (!prompt.active()) return std::nullopt;
    return promptFocusRegion(prompt.request()->kind);
}

struct ScreenProjection {
    bool panelPresent = false;
    bool distractionFree = false;
    std::optional<PickerKind> openPicker;
    BaseFocus baseFocus = BaseFocus::Editor;
    bool noticePresent = false;
    bool externalModificationPresent = false;
    bool externalFocusHeld = false;
};

UiNodeId nodeId(std::string_view id) { return UiNodeId{std::string{id}}; }

UiInteractionState buildInteraction(
    UiSchema schema, const ScreenProjection& truth,
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
    const UiSchema& schema, ScreenProjection truth) {
    truth.openPicker.reset();
    const auto closedState =
        buildInteraction(schema, truth, std::nullopt);
    truth.openPicker = PickerKind::Command;
    const auto openState =
        buildInteraction(schema, truth, PromptRegion::Header);

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

UiSchema validatedSchema(UiComposition composition) {
    UiSchema schema{std::move(composition.root)};
    const UiSchemaValidation result = validateUiSchema(schema);
    if (!result.ok()) {
        throw std::logic_error("screen layout failed validation: " +
                               *result.error);
    }
    return schema;
}

bool updateSchema(UiSchema& schema, UiComposition composition) {
    if (composition.root == schema.root) return false;
    schema = validatedSchema(std::move(composition));
    return true;
}

}  // namespace

ScreenState::ScreenState(UiComposition initialAssembly, TreeModel& tree,
                         PickerActivationId firstPickerActivation)
    : baseComposition_{std::move(initialAssembly)},
      tree_{tree},
      nextPickerActivation_{firstPickerActivation} {
    (void)validatedSchema(baseComposition_);
    if (!nextPickerActivation_.valid()) {
        throw std::invalid_argument(
           "screen picker activation source must be valid");
    }
}

UiComposition ScreenState::assembled(
    const UiComposition& base, const PromptSurface& prompt) const {
    UiComposition projected = withStatusActions(base, statusActions_);
    return prompt.active() ? withFooterPrompt(std::move(projected), prompt)
                           : projected;
}

UiInteractionState ScreenState::project() const {
    UiSchema schema{assembled(baseComposition_, prompt_).root};
    return buildInteraction(
        std::move(schema),
        {panelPresent_, distractionFree_, visiblePicker(), baseFocus_,
         noticePresent_, externalModificationPresent_, externalFocusHeld_},
        activePromptRegion(prompt_));
}

std::optional<PickerKind> ScreenState::visiblePicker() const noexcept {
    if (!prompt_.active() || prompt_.request()->kind != PromptKind::Palette) {
        return std::nullopt;
    }
    return openPicker_;
}

bool ScreenState::activatePanelProvider(
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

    if (!tree_.activateOrCreate(binding)) return false;
    panelPresent_ = panelPresent;
    baseFocus_ = baseFocus;
    panelReturnFocus_ = panelReturnFocus;
    return true;
}

bool ScreenState::togglePanel() {
    auto& state = *this;
    if (state.panelPresent_) {
        state.panelPresent_ = false;
        state.baseFocus_ = state.panelReturnFocus_;
    } else {
        state.panelReturnFocus_ = state.baseFocus_;
        state.panelPresent_ = true;
        state.baseFocus_ = BaseFocus::Panel;
    }
    return true;
}

bool ScreenState::showPanelProvider(TreeProviderKind kind) {
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

bool ScreenState::switchPanelProvider(CycleDirection direction) {
    auto& state = *this;
    const auto active = state.tree_.activeProviderBinding();
    if (!active) return false;
    return state.activatePanelProvider(
        cyclePanelTreeProvider(*active, direction), state.panelPresent_,
        state.baseFocus_, state.panelReturnFocus_);
}

bool ScreenState::openFinder(PickerKind kind) {
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
    state.prompt_ = std::move(prompt);
    state.openPickerActivation_ =
        PickerActivation{descriptor->wireMode, state.nextPickerActivation_};
    state.nextPickerActivation_ =
        PickerActivationId{state.nextPickerActivation_.value() + 1};
    return true;
}

bool ScreenState::closeFinder() {
    auto& state = *this;
    PromptSurface prompt = state.prompt_;
    if (!prompt.cancel().accepted()) return false;
    state.openPicker_.reset();
    state.prompt_ = std::move(prompt);
    state.openPickerActivation_.reset();
    return true;
}

void ScreenState::toggleDistractionFree() {
    auto& state = *this;
    state.distractionFree_ = !state.distractionFree_;
}

void ScreenState::focusEditor() {
    auto& state = *this;
    state.baseFocus_ = BaseFocus::Editor;
}

bool ScreenState::focusPanel() {
    auto& state = *this;
    if (!state.panelPresent_) return false;
    state.baseFocus_ = BaseFocus::Panel;
    return true;
}

bool ScreenState::refreshNoticePresence(bool present) {
    auto& state = *this;
    if (state.noticePresent_ == present) return false;
    state.noticePresent_ = present;
    return true;
}

bool ScreenState::refreshExternalModificationPresence(bool present) {
    auto& state = *this;
    if (state.externalModificationPresent_ == present &&
        (present || !state.externalFocusHeld_)) {
        return false;
    }
    state.externalModificationPresent_ = present;
    // Presence dropping clears focus: the capture auto-pops and a later disk event
    // that re-raises the bar never reactively steals the keyboard.
    if (!present) state.externalFocusHeld_ = false;
    return true;
}

bool ScreenState::captureExternalFocus() {
    auto& state = *this;
    if (!state.externalModificationPresent_ || state.externalFocusHeld_) {
        return false;
    }
    state.externalFocusHeld_ = true;
    return true;
}

bool ScreenState::releaseExternalFocus() {
    auto& state = *this;
    if (!state.externalFocusHeld_) return false;
    state.externalFocusHeld_ = false;
    return true;
}

bool ScreenState::updateComposition(UiComposition assembly) {
    auto& state = *this;
    UiSchema candidate{
        state.assembled(state.baseComposition_, state.prompt_).root};
    UiComposition projected = state.assembled(assembly, state.prompt_);
    if (!updateSchema(candidate, std::move(projected))) {
        state.baseComposition_ = std::move(assembly);
        return false;
    }
    state.baseComposition_ = std::move(assembly);
    return true;
}

bool ScreenState::refreshStatusActions(
    std::vector<StatusActionNode> actions) {
    auto& state = *this;
    if (actions == state.statusActions_) return false;
    UiSchema candidate{
        state.assembled(state.baseComposition_, state.prompt_).root};
    UiComposition projected = withStatusActions(state.baseComposition_, actions);
    if (state.prompt_.active()) {
        projected = withFooterPrompt(std::move(projected), state.prompt_);
    }
    (void)updateSchema(candidate, std::move(projected));
    state.statusActions_ = std::move(actions);
    return true;
}

PromptSurface& ScreenState::prompt() noexcept {
    return prompt_;
}

const PromptSurface& ScreenState::prompt() const noexcept {
    return prompt_;
}

const std::vector<StatusActionNode>& ScreenState::statusActions() const
    noexcept {
    return statusActions_;
}

FocusTarget ScreenState::effectiveFocus() const noexcept {
    if (prompt_.active()) return FocusTarget::Prompt;
    if (externalModificationPresent_ && externalFocusHeld_) {
        return FocusTarget::ExternalModification;
    }
    return baseFocus_ == BaseFocus::Panel && panelPresent_ && !distractionFree_
               ? FocusTarget::Panel
               : FocusTarget::Editor;
}

std::optional<PickerKind> ScreenState::openPicker() const noexcept {
    return visiblePicker();
}

std::optional<PickerActivation>
ScreenState::openPickerActivation() const noexcept {
    return visiblePicker() ? openPickerActivation_ : std::nullopt;
}

UiSchema ScreenState::schema() const {
    return project().schema();
}

std::vector<UiNodeId> ScreenState::focusPath() const {
    return project().focusPath();
}

PalettePresenceOverlay ScreenState::pickerPresenceOverlay() const {
    UiSchema current{assembled(baseComposition_, prompt_).root};
    return derivePickerPresenceOverlay(
        current,
        {panelPresent_, distractionFree_, visiblePicker(), baseFocus_,
         noticePresent_, externalModificationPresent_, externalFocusHeld_});
}

}  // namespace ssg
