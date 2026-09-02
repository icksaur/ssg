#pragma once

#include <ssg/UiNodeState.h>
#include <ssg/UiPresence.h>
#include <ssg/UiTree.h>
#include <ssg/focus.h>

#include <optional>
#include <utility>
#include <vector>

namespace ssg {

struct UiFrameVersion {
    Generation generation{0};
    PresenceBasis presenceBasis{0};

    friend bool operator==(const UiFrameVersion&, const UiFrameVersion&) = default;
};

class UiFrame {
public:
    UiFrame();

    [[nodiscard]] static std::optional<UiFrame> create(
        UiSchema schema, UiStateSection state, UiPresenceSection presence);
    [[nodiscard]] static UiFrame require(
        UiSchema schema, UiStateSection state, UiPresenceSection presence);

    [[nodiscard]] const UiSchema& schema() const noexcept { return schema_; }
    [[nodiscard]] const UiStateSection& state() const noexcept { return state_; }
    [[nodiscard]] const UiPresenceSection& presence() const noexcept {
        return presence_;
    }
    [[nodiscard]] const std::vector<UiNodeId>& focusPath() const noexcept {
        return *state_.focusPath;
    }
    [[nodiscard]] FocusTarget effectiveFocus() const noexcept;
    [[nodiscard]] UiFrameVersion version() const noexcept {
        return {schema_.generation, presence_.basis};
    }

    friend bool operator==(const UiFrame&, const UiFrame&) = default;

private:
    struct ValidatedTag {};
    UiFrame(UiSchema schema, UiStateSection state, UiPresenceSection presence,
            ValidatedTag)
        : schema_{std::move(schema)},
          state_{std::move(state)},
          presence_{std::move(presence)} {}

    UiSchema schema_;
    UiStateSection state_;
    UiPresenceSection presence_;
};

}  // namespace ssg
