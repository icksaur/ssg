#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include <ssg/CommandTransition.h>
#include <ssg/Picker.h>
#include <ssg/PaletteSearcher.h>
#include <ssg/PromptSurface.h>
#include <ssg/StatusQueue.h>
#include <ssg/TreeModel.h>
#include <ssg/UiTree.h>
#include <ssg/UiPresence.h>

namespace ssg {

class InteractionAuthority {
public:
    InteractionAuthority(UiComposition initialAssembly, TreeModel& tree,
                         std::uint64_t firstTreeRevision = 1,
                         PickerActivationId firstPickerActivation =
                             PickerActivationId{1});
    ~InteractionAuthority();

    InteractionAuthority(InteractionAuthority const&) = delete;
    InteractionAuthority& operator=(InteractionAuthority const&) = delete;
    InteractionAuthority(InteractionAuthority&&) noexcept;
    InteractionAuthority& operator=(InteractionAuthority&&) noexcept;

    bool apply(const CommandTransition& transition);

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
    [[nodiscard]] const ValidatedSchema& validatedSchema() const noexcept;
    [[nodiscard]] std::vector<UiNodeId> focusPath() const;
    [[nodiscard]] UiPresenceSection presenceSection(PresenceBasis basis) const;
    [[nodiscard]] PalettePresenceOverlay pickerPresenceOverlay() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ssg
