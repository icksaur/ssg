#pragma once

#include <cstdint>
#include <optional>

#include <ssg/Picker.h>
#include <ssg/PaletteSearcher.h>
#include <ssg/PromptSurface.h>
#include <ssg/PromptStatusViewState.h>
#include <ssg/TreeModel.h>
#include <ssg/UiTree.h>
#include <ssg/UiInteractionState.h>

namespace ssg {

enum class CycleDirection : std::uint8_t { Next, Previous };

class InteractionState {
public:
    InteractionState(UiComposition initialAssembly, TreeModel& tree,
                     std::uint64_t firstTreeRevision = 1,
                     PickerActivationId firstPickerActivation =
                         PickerActivationId{1});

    bool togglePanel();
    bool showPanelProvider(TreeProviderKind kind);
    bool switchPanelProvider(CycleDirection direction);
    bool openFinder(PickerKind kind);
    bool closeFinder();

    PromptCommandResult openPrompt(PromptRequest request);
    PromptCommandResult submitPrompt();
    PromptCommandResult cancelPrompt();
    PromptCommandResult updatePromptValue(std::size_t index, std::string value);
    PromptCommandResult focusPromptControl(std::string_view controlId);
    PromptCommandResult focusNextPromptControl();

    void toggleDistractionFree();
    void focusEditor();
    bool focusPanel();

    bool refreshNoticePresence(bool present);
    bool refreshExternalModificationPresence(bool present);
    bool captureExternalFocus();
    bool releaseExternalFocus();

    bool updateComposition(UiComposition assembly);
    bool refreshStatusActions(std::vector<StatusActionNode> actions);
    TreeRevision allocateTreeRevision();

    [[nodiscard]] const PromptSurface& prompt() const noexcept;
    [[nodiscard]] const std::vector<StatusActionNode>& statusActions() const noexcept;
    [[nodiscard]] FocusTarget effectiveFocus() const noexcept;
    [[nodiscard]] std::optional<PickerKind> openPicker() const noexcept;
    [[nodiscard]] const std::optional<PickerActivation>&
    openPickerActivation() const noexcept;
    [[nodiscard]] std::uint64_t routingGeneration() const noexcept;
    [[nodiscard]] const UiSchema& schema() const noexcept;
    [[nodiscard]] std::vector<UiNodeId> focusPath() const;
    [[nodiscard]] PalettePresenceOverlay pickerPresenceOverlay() const;

private:
    [[nodiscard]] UiComposition assembled(const UiComposition& base,
                                          const PromptSurface& prompt) const;
    [[nodiscard]] UiInteractionState project(
        const UiSchema& schema, const PromptSurface& prompt) const;
    void adopt(PromptSurface prompt);
    bool activatePanelProvider(TreeProviderBinding binding, bool panelPresent,
                               BaseFocus baseFocus,
                               BaseFocus panelReturnFocus);

    UiComposition baseComposition_;
    std::vector<StatusActionNode> statusActions_;
    UiSchema schema_;
    TreeModel& tree_;
    std::uint64_t nextTreeRevision_;
    PickerActivationId nextPickerActivation_;
    std::optional<PickerActivation> openPickerActivation_;
    std::uint64_t routingGeneration_ = 0;
    PromptSurface prompt_;
    bool panelPresent_ = false;
    bool distractionFree_ = false;
    std::optional<PickerKind> openPicker_;
    BaseFocus baseFocus_ = BaseFocus::Editor;
    BaseFocus panelReturnFocus_ = BaseFocus::Editor;
    bool noticePresent_ = false;
    bool externalModificationPresent_ = false;
    bool externalFocusHeld_ = false;
    UiInteractionState interaction_;
};

}  // namespace ssg
