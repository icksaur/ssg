#pragma once

#include <optional>

#include <ssg/Picker.h>
#include <ssg/PaletteSearcher.h>
#include <ssg/PromptSurface.h>
#include <ssg/TreeModel.h>
#include <ssg/UiTree.h>
#include <ssg/UiInteractionState.h>
#include <ssg/types.h>

namespace ssg {

class ScreenState {
public:
    ScreenState(UiComposition initialAssembly, TreeModel& tree,
                PickerActivationId firstPickerActivation =
                    PickerActivationId{1});

    bool togglePanel();
    bool showPanelProvider(TreeProviderKind kind);
    bool switchPanelProvider(CycleDirection direction);
    bool openFinder(PickerKind kind);
    bool closeFinder();

    void toggleDistractionFree();
    void focusEditor();
    bool showPanel();
    bool focusPanel();

    bool refreshExternalModificationPresence(bool present);
    bool captureExternalFocus();
    bool releaseExternalFocus();

    bool updateComposition(UiComposition assembly);
    [[nodiscard]] PromptSurface& prompt() noexcept;
    [[nodiscard]] const PromptSurface& prompt() const noexcept;
    [[nodiscard]] FocusTarget effectiveFocus() const noexcept;
    [[nodiscard]] std::optional<PickerKind> openPicker() const noexcept;
    [[nodiscard]] std::optional<PickerActivation>
    openPickerActivation() const noexcept;
    [[nodiscard]] UiSchema schema() const;
    [[nodiscard]] std::vector<UiNodeId> focusPath() const;
    [[nodiscard]] PalettePresenceOverlay pickerPresenceOverlay() const;

private:
    [[nodiscard]] UiComposition assembled(const UiComposition& base,
                                          const PromptSurface& prompt) const;
    [[nodiscard]] UiInteractionState project() const;
    [[nodiscard]] std::optional<PickerKind> visiblePicker() const noexcept;
    bool activatePanelProvider(TreeProviderBinding binding, bool panelPresent,
                               BaseFocus baseFocus,
                               BaseFocus panelReturnFocus);

    UiComposition baseComposition_;
    TreeModel& tree_;
    PickerActivationId nextPickerActivation_;
    std::optional<PickerActivation> openPickerActivation_;
    PromptSurface prompt_;
    bool panelPresent_ = false;
    bool distractionFree_ = false;
    std::optional<PickerKind> openPicker_;
    BaseFocus baseFocus_ = BaseFocus::Editor;
    BaseFocus panelReturnFocus_ = BaseFocus::Editor;
    bool externalModificationPresent_ = false;
    bool externalFocusHeld_ = false;
};

}  // namespace ssg
